from dataclasses import dataclass, field
from pathlib import Path

from trl import SFTConfig, SFTTrainer


@dataclass
class JeffTrainingConfig:
    model_name: str = "Qwen/Qwen2.5-7B-Instruct"
    output_dir: str = "checkpoints/exp"
    num_train_epochs: int = 3
    max_steps: int = -1                  # >0 overrides epochs (smoke tests)
    per_device_train_batch_size: int = 4
    gradient_accumulation_steps: int = 8
    learning_rate: float = 2e-5
    warmup_ratio: float = 0.05
    bf16: bool = True
    gradient_checkpointing: bool = True
    max_length: int = 2048
    logging_steps: int = 10
    save_steps: int = 200
    eval_steps: int = 200
    lora_r: int = 64
    lora_alpha: int = 128
    lora_target_modules: list[str] = field(default_factory=lambda: ["q_proj", "v_proj"])
    report_to: str = "none"              # set "wandb" to log runs


class JeffTrainer:
    """SFT wrapper for jeff — LoRA fine-tune of a Qwen causal LM on prompt/completion pairs."""

    def __init__(self, config: JeffTrainingConfig):
        self.config = config

    def train(self, train_dataset: list[dict], eval_dataset: list[dict] | None = None) -> None:
        import torch
        from datasets import Dataset
        from peft import LoraConfig
        from transformers import AutoModelForCausalLM, AutoTokenizer

        cfg = self.config
        tokenizer = AutoTokenizer.from_pretrained(cfg.model_name)
        model = AutoModelForCausalLM.from_pretrained(cfg.model_name, dtype=torch.bfloat16)

        peft_cfg = LoraConfig(
            r=cfg.lora_r,
            lora_alpha=cfg.lora_alpha,
            target_modules=cfg.lora_target_modules,
            bias="none",
            task_type="CAUSAL_LM",
        )

        has_eval = bool(eval_dataset)
        args = SFTConfig(
            output_dir=cfg.output_dir,
            num_train_epochs=cfg.num_train_epochs,
            max_steps=cfg.max_steps,
            per_device_train_batch_size=cfg.per_device_train_batch_size,
            per_device_eval_batch_size=cfg.per_device_train_batch_size,
            gradient_accumulation_steps=cfg.gradient_accumulation_steps,
            learning_rate=cfg.learning_rate,
            warmup_steps=cfg.warmup_ratio,   # float in [0,1) = ratio (transformers 5)
            bf16=cfg.bf16,
            gradient_checkpointing=cfg.gradient_checkpointing,
            max_length=cfg.max_length,
            completion_only_loss=True,     # loss only on the answer index token(s)
            logging_steps=cfg.logging_steps,
            save_steps=cfg.save_steps,
            save_total_limit=3,
            eval_strategy="steps" if has_eval else "no",
            eval_steps=cfg.eval_steps if has_eval else None,
            report_to=cfg.report_to,
        )

        trainer = SFTTrainer(
            model=model,
            args=args,
            train_dataset=Dataset.from_list(train_dataset),
            eval_dataset=Dataset.from_list(eval_dataset) if has_eval else None,
            processing_class=tokenizer,
            peft_config=peft_cfg,
        )
        trainer.train()
        Path(cfg.output_dir).mkdir(parents=True, exist_ok=True)
        trainer.save_model()
        tokenizer.save_pretrained(cfg.output_dir)
