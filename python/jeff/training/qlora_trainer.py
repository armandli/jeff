"""QLoRA trainer for Qwen3-8B + DecisionHead.

Training stages:
  1. SFT  — cross-entropy over the MLP head's option scores on labelled decision examples
  2. RLCD — DPO on (chosen, rejected) option pairs using the same head

The MLP head is trained with full precision; the LoRA adapters and BnB 4-bit
base are handled by PEFT + bitsandbytes.
"""

from dataclasses import dataclass, field
from pathlib import Path

import torch
import torch.nn.functional as F
from torch.utils.data import DataLoader

from jeff.models.qwen3_decision import Qwen3Config, Qwen3DecisionModel


@dataclass
class QLoRATrainingConfig:
    # Model
    model_name: str = "Qwen/Qwen3-8B"
    load_in_4bit: bool = True
    lora_r: int = 64
    lora_alpha: int = 128
    lora_dropout: float = 0.05
    lora_target_modules: list[str] = field(default_factory=lambda: [
        "q_proj", "k_proj", "v_proj", "o_proj",
        "gate_proj", "up_proj", "down_proj",
    ])

    # Training
    output_dir: str = "checkpoints/exp_001_qlora"
    num_epochs: int = 3
    batch_size: int = 4
    gradient_accumulation_steps: int = 8
    learning_rate: float = 2e-4
    head_learning_rate: float = 1e-3    # MLP head trains faster than LoRA adapters
    warmup_ratio: float = 0.05
    max_grad_norm: float = 1.0
    bf16: bool = True
    gradient_checkpointing: bool = True

    # Logging
    log_every: int = 10
    save_every: int = 200
    wandb_project: str = "jeff"
    wandb_run: str = "exp_001_qlora"


class QLoRATrainer:
    """Two-phase trainer: SFT then optionally RLCD/DPO on the MLP head + LoRA."""

    def __init__(self, cfg: QLoRATrainingConfig):
        self.cfg = cfg

    def build_model(self) -> Qwen3DecisionModel:
        mcfg = Qwen3Config(
            model_name=self.cfg.model_name,
            load_in_4bit=self.cfg.load_in_4bit,
            lora_r=self.cfg.lora_r,
            lora_alpha=self.cfg.lora_alpha,
            lora_dropout=self.cfg.lora_dropout,
            lora_target_modules=self.cfg.lora_target_modules,
        )
        model = Qwen3DecisionModel(mcfg)
        model.load_base_for_training()
        return model

    def train_sft(self, train_dataset, eval_dataset=None) -> None:
        import wandb
        from torch.optim import AdamW
        from transformers import get_cosine_schedule_with_warmup

        cfg = self.cfg
        wandb.init(project=cfg.wandb_project, name=cfg.wandb_run, config=vars(cfg))

        model = self.build_model()

        # Separate param groups: lower LR for LoRA, higher for the MLP head
        lora_params = [p for n, p in model.backbone.named_parameters() if p.requires_grad]
        head_params = list(model.head.parameters())
        optimizer = AdamW(
            [
                {"params": lora_params, "lr": cfg.learning_rate},
                {"params": head_params, "lr": cfg.head_learning_rate},
            ],
            weight_decay=0.01,
        )

        loader = DataLoader(train_dataset, batch_size=cfg.batch_size, shuffle=True)
        total_steps = len(loader) * cfg.num_epochs // cfg.gradient_accumulation_steps
        warmup_steps = int(total_steps * cfg.warmup_ratio)
        scheduler = get_cosine_schedule_with_warmup(optimizer, warmup_steps, total_steps)

        scaler = torch.cuda.amp.GradScaler(enabled=cfg.bf16)

        step = 0
        for epoch in range(cfg.num_epochs):
            for batch_idx, batch in enumerate(loader):
                loss = self._sft_step(model, batch)
                loss = loss / cfg.gradient_accumulation_steps
                scaler.scale(loss).backward()

                if (batch_idx + 1) % cfg.gradient_accumulation_steps == 0:
                    scaler.unscale_(optimizer)
                    torch.nn.utils.clip_grad_norm_(
                        lora_params + head_params, cfg.max_grad_norm
                    )
                    scaler.step(optimizer)
                    scaler.update()
                    scheduler.step()
                    optimizer.zero_grad()
                    step += 1

                    if step % cfg.log_every == 0:
                        wandb.log({"train/loss": loss.item(), "step": step})

                    if step % cfg.save_every == 0:
                        self._save(model, cfg.output_dir, suffix=f"step{step}")

        self._save(model, cfg.output_dir, suffix="final")
        wandb.finish()

    def _sft_step(self, model: Qwen3DecisionModel, batch: dict) -> torch.Tensor:
        """Cross-entropy loss over option scores from the MLP head.

        Batch must contain:
          input_ids, attention_mask, answer_positions, labels (correct option index)
        """
        inp = {k: v.cuda() for k, v in batch.items() if k != "labels"}
        labels = batch["labels"].cuda()

        scores = model.forward_for_training(
            input_ids=inp["input_ids"],
            attention_mask=inp["attention_mask"],
            answer_positions=inp["answer_positions"],
            option_indices=inp.get("option_indices", torch.zeros_like(labels)),
        )
        # scores is [B] scalar per sample (to be expanded to [B, n_options] later)
        # placeholder loss until per-option scoring is implemented
        return F.binary_cross_entropy_with_logits(scores, labels.float())

    def _save(self, model: Qwen3DecisionModel, output_dir: str, suffix: str = "") -> None:
        path = str(Path(output_dir) / suffix) if suffix else output_dir
        model.save_pretrained(path)
        print(f"Saved checkpoint → {path}")
