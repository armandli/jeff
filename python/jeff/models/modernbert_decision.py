"""ModernBERT-large encoder decision model for fast System 1 decisions.

Architecture
------------
- Base: answerdotai/ModernBERT-large (395M, bidirectional encoder)
  or ModernBERT-base (149M) for maximum speed.
- LoRA on attention layers (Wqkv, o_proj); head in fp32 full precision.
- Scoring: for each option, encode "[CLS] context [SEP] option [SEP]" and read
  the [CLS] hidden state → Linear(hidden_size, 1) → scalar score.
  All N options for one example are encoded in a single batched forward pass.
- No permutation debiasing: each option is encoded on its own with the context, so
  an option's score cannot depend on where it sits in the list. Averaging over
  cyclic rotations (as the Qwen decoder models do) would only repeat the same work.

Speed rationale (vs Qwen3-8B decoder)
--------------------------------------
  - 395M params vs 8B — ~20× fewer operations
  - No autoregressive token generation — O(1) forward passes per decision
  - Encoder flash-attention — shorter effective sequence lengths
  Expect ~10-30ms/decision on a modern GPU vs ~200ms for Qwen3-8B QLoRA.

Inference
---------
  model = ModernBERTDecisionModel.from_pretrained(checkpoint)
  out = model.decide(DecisionInput(context="...", options=["A", "B", "C"]))
"""

from dataclasses import dataclass, field

import torch
import torch.nn as nn
import torch.nn.functional as F
from transformers import AutoModel, AutoTokenizer

from jeff.models.base import DecisionInput, DecisionModel, DecisionOutput

DEFAULT_MODEL = "answerdotai/ModernBERT-large"


# ---------------------------------------------------------------------------
# Linear decision head
# ---------------------------------------------------------------------------

class DecisionHead(nn.Module):
    """Single linear layer: [CLS] hidden → scalar option score."""

    def __init__(self, hidden_size: int):
        super().__init__()
        self.proj = nn.Linear(hidden_size, 1, bias=False)
        nn.init.zeros_(self.proj.weight)

    def forward(self, cls_hidden: torch.Tensor) -> torch.Tensor:
        # cls_hidden: [..., hidden_size] → [...]
        return self.proj(cls_hidden.float()).squeeze(-1)


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

@dataclass
class ModernBERTConfig:
    model_name: str = DEFAULT_MODEL
    device: str = "cuda"
    lora_r: int = 16
    lora_alpha: int = 32
    lora_dropout: float = 0.05
    # ModernBERT attention + MLP projection names
    lora_target_modules: list[str] = field(default_factory=lambda: [
        "Wqkv", "o_proj", "Wi", "Wo",
    ])
    full_finetune: bool = False        # True → fine-tune all weights (no LoRA)
    max_seq_len: int = 512
    n_permutations: int = 0            # ignored: option order cannot affect scores (see module doc)


# ---------------------------------------------------------------------------
# Encoding helpers
# ---------------------------------------------------------------------------

def encode_options(
    tokenizer,
    context: str,
    options: list[str],
    perm: list[int],
    max_seq_len: int,
    truncate: bool = True,
) -> dict:
    """Tokenise all options for one permutation as a batch of pair sequences.

    Returns {"input_ids": list[list[int]], "attention_mask": list[list[int]]}
    with one row per option in permuted order. Truncation cuts the end of the
    context, which is where a router's typed line sits, so training encodes with
    truncate=False and drops over-long rows instead.
    """
    contexts = [context] * len(perm)
    opted = [options[i] for i in perm]
    enc = tokenizer(
        contexts,
        opted,
        truncation=truncate,
        max_length=max_seq_len if truncate else None,
        padding=False,          # will pad in collate
        add_special_tokens=True,
    )
    return {
        "input_ids": enc["input_ids"],
        "attention_mask": enc["attention_mask"],
    }


# ---------------------------------------------------------------------------
# Main model class
# ---------------------------------------------------------------------------

class ModernBERTDecisionModel(DecisionModel):
    """ModernBERT-large encoder + linear decision head with permutation debiasing."""

    def __init__(self, cfg: ModernBERTConfig | None = None):
        self.cfg = cfg or ModernBERTConfig()
        self.tokenizer = None
        self.backbone = None
        self.head = None

    # ------------------------------------------------------------------
    # Loading
    # ------------------------------------------------------------------

    @classmethod
    def from_pretrained(
        cls,
        checkpoint: str,
        base_model: str | None = None,
        cfg: ModernBERTConfig | None = None,
    ) -> "ModernBERTDecisionModel":
        from peft import PeftModel

        obj = cls(cfg)
        obj._load_base(base_model or (cfg.model_name if cfg else DEFAULT_MODEL))
        obj.backbone = PeftModel.from_pretrained(obj.backbone, checkpoint)
        obj.head.load_state_dict(
            torch.load(f"{checkpoint}/decision_head.pt",
                       map_location=obj.head.proj.weight.device)
        )
        obj.backbone.eval()
        return obj

    def load_base_for_training(self) -> None:
        """Load backbone, attach LoRA (or prepare full fine-tune), init head."""
        self._load_base(self.cfg.model_name)

        if not self.cfg.full_finetune:
            from peft import LoraConfig, get_peft_model
            lora_cfg = LoraConfig(
                r=self.cfg.lora_r,
                lora_alpha=self.cfg.lora_alpha,
                lora_dropout=self.cfg.lora_dropout,
                target_modules=self.cfg.lora_target_modules,
                bias="none",
                task_type="FEATURE_EXTRACTION",
            )
            self.backbone = get_peft_model(self.backbone, lora_cfg)
            self.backbone.print_trainable_parameters()
        else:
            for p in self.backbone.parameters():
                p.requires_grad_(True)
            n = sum(p.numel() for p in self.backbone.parameters()) / 1e6
            print(f"Full fine-tune: {n:.0f}M backbone parameters trainable", flush=True)

    def _load_base(self, model_name: str) -> None:
        cfg = self.cfg
        self.tokenizer = AutoTokenizer.from_pretrained(
            model_name, trust_remote_code=True
        )
        self.backbone = AutoModel.from_pretrained(
            model_name,
            trust_remote_code=True,
            torch_dtype=torch.bfloat16,
            device_map="auto" if cfg.device == "cuda" else cfg.device,
        )
        hidden_size = self.backbone.config.hidden_size
        dev = "cuda:0" if cfg.device == "cuda" else cfg.device
        self.head = DecisionHead(hidden_size).to(dev)

    # ------------------------------------------------------------------
    # Core forward pass
    # ------------------------------------------------------------------

    def forward_for_training(
        self,
        input_ids: torch.Tensor,       # [B*N, T] all options flat
        attention_mask: torch.Tensor,  # [B*N, T]
        n_options: int,                # N options per example
    ) -> torch.Tensor:                 # [B, N] log-scores
        out = self.backbone(input_ids=input_ids, attention_mask=attention_mask)
        cls_h = out.last_hidden_state[:, 0, :]      # [B*N, H]
        scores = self.head(cls_h).view(-1, n_options)   # [B, N]
        return scores

    def collate(self, items: list[dict]) -> dict:
        """Pad a list of encoded items into tensors.

        Each item has keys "input_ids" (list[list[int]]) and "attention_mask",
        both of length N (options). Items may have different N or T.
        """
        pad_id = self.tokenizer.pad_token_id or 0
        n_options = max(len(it["input_ids"]) for it in items)
        all_ids, all_am, counts = [], [], []
        for it in items:
            all_ids.extend(it["input_ids"])
            all_am.extend(it["attention_mask"])
            counts.append(len(it["input_ids"]))

        T = max(len(x) for x in all_ids)
        ids_t = torch.tensor([x + [pad_id] * (T - len(x)) for x in all_ids])
        am_t  = torch.tensor([x + [0]      * (T - len(x)) for x in all_am])

        out = {"input_ids": ids_t, "attention_mask": am_t,
               "n_options": n_options, "counts": counts}
        if "label" in items[0]:
            out["labels"] = torch.tensor([it["label"] for it in items])
        return out

    # ------------------------------------------------------------------
    # Encode one training example
    # ------------------------------------------------------------------

    def encode_example(self, context: str, options: list[str], perm: list[int],
                       truncate: bool = True) -> dict:
        enc = encode_options(
            self.tokenizer, context, options, perm, self.cfg.max_seq_len, truncate
        )
        return enc   # {"input_ids": [[...], ...], "attention_mask": [[...], ...]}

    # ------------------------------------------------------------------
    # Inference
    # ------------------------------------------------------------------

    @torch.no_grad()
    def score_options(self, inp: DecisionInput) -> list[float]:
        """Log-probabilities over the options, one forward pass for all of them."""
        n = len(inp.options)
        enc = self.encode_example(inp.context, inp.options, list(range(n)))
        batch = self.collate([enc])
        dev = self.head.proj.weight.device
        scores = self.forward_for_training(
            batch["input_ids"].to(dev), batch["attention_mask"].to(dev), n_options=n
        ).squeeze(0).float().cpu()
        return torch.log_softmax(scores, dim=-1).tolist()

    def decide(self, inp: DecisionInput) -> DecisionOutput:
        scores = self.score_options(inp)
        chosen = max(range(len(scores)), key=scores.__getitem__)
        return DecisionOutput(chosen_index=chosen, scores=scores)

    # ------------------------------------------------------------------
    # Training helpers
    # ------------------------------------------------------------------

    def trainable_parameters(self):
        for name, p in self.backbone.named_parameters():
            if p.requires_grad:
                yield name, p
        yield from self.head.named_parameters()

    def save_pretrained(self, output_dir: str) -> None:
        import os
        os.makedirs(output_dir, exist_ok=True)
        self.backbone.save_pretrained(output_dir)
        self.tokenizer.save_pretrained(output_dir)
        torch.save(self.head.state_dict(), f"{output_dir}/decision_head.pt")
