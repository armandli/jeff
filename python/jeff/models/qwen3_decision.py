"""Qwen3-8B decision model with QLoRA and a learned MLP readout head.

Architecture
------------
- Base: Qwen3-8B (or Qwen3-8B-Instruct), loaded in 4-bit NF4 via bitsandbytes
- LoRA adapters on q_proj / k_proj / v_proj / o_proj / gate_proj / up_proj / down_proj
- DecisionHead: small MLP on top of the last hidden state at the answer position
  (replaces the raw next-token logit readout used by stock AnyJev)
- Permutation debiasing (AnyJev style): K cyclic rotations of option order,
  scores averaged before softmax to cancel positional bias

Training flow
-------------
1. SFT phase  : cross-entropy on correct option index via MLP head scores
2. RLCD phase : DPO on (chosen, rejected) option pairs

Inference flow
--------------
  model = Qwen3DecisionModel.from_pretrained(path)
  out = model.decide(DecisionInput(context="...", options=["A", "B", "C"]))
"""

import itertools
from dataclasses import dataclass, field

import torch
import torch.nn as nn
import torch.nn.functional as F
from transformers import AutoModelForCausalLM, AutoTokenizer

from jeff.models.base import DecisionInput, DecisionModel, DecisionOutput

DEFAULT_MODEL = "Qwen/Qwen3-8B-Instruct"

# ---------------------------------------------------------------------------
# MLP readout head
# ---------------------------------------------------------------------------

class DecisionHead(nn.Module):
    """Maps a single hidden-state vector to a scalar decision score.

    One forward pass per option (or per permuted option position); the scores
    are then compared across options before softmax.
    """

    def __init__(self, hidden_size: int):
        super().__init__()
        mid = hidden_size // 2
        self.net = nn.Sequential(
            nn.Linear(hidden_size, mid, bias=False),
            nn.GELU(),
            nn.LayerNorm(mid),
            nn.Linear(mid, 1, bias=False),
        )
        # Zero-init output layer so the MLP starts as a no-op relative to logits
        nn.init.zeros_(self.net[-1].weight)

    def forward(self, hidden: torch.Tensor) -> torch.Tensor:
        # hidden: [batch, hidden_size] → [batch]
        return self.net(hidden).squeeze(-1)


# ---------------------------------------------------------------------------
# Prompt formatting (AnyJev-compatible)
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = (
    "You are a precise decision-making system. "
    "Given a situation and a list of options, output only the letter of the best option."
)

def _format_prompt(context: str, options: list[str], perm: list[int]) -> tuple[str, str]:
    """Return (prompt_text, answer_prefix) for a given option permutation.

    perm is a list of original indices in the order they should be presented.
    answer_prefix is the token string we expect to read the hidden state at.
    """
    labels = [chr(ord("A") + i) for i in range(len(perm))]
    opts_text = "\n".join(f"{lbl}. {options[perm[i]]}" for i, lbl in enumerate(labels))
    prompt = (
        f"<|im_start|>system\n{SYSTEM_PROMPT}<|im_end|>\n"
        f"<|im_start|>user\n{context}\n\nOptions:\n{opts_text}<|im_end|>\n"
        f"<|im_start|>assistant\n"
    )
    return prompt, "Answer:"


def _cyclic_permutations(n: int, max_k: int = 4) -> list[list[int]]:
    """Return up to max_k cyclic rotations of range(n)."""
    base = list(range(n))
    perms = [base[i:] + base[:i] for i in range(n)]
    return perms[:max_k]


# ---------------------------------------------------------------------------
# Main model class
# ---------------------------------------------------------------------------

@dataclass
class Qwen3Config:
    model_name: str = DEFAULT_MODEL
    device: str = "cuda"
    load_in_4bit: bool = True              # QLoRA
    load_in_8bit: bool = False             # alternative: 8-bit
    bnb_4bit_quant_type: str = "nf4"
    bnb_4bit_compute_dtype: str = "bfloat16"
    bnb_double_quant: bool = True
    lora_r: int = 64
    lora_alpha: int = 128
    lora_dropout: float = 0.05
    lora_target_modules: list[str] = field(default_factory=lambda: [
        "q_proj", "k_proj", "v_proj", "o_proj",
        "gate_proj", "up_proj", "down_proj",
    ])
    n_permutations: int = 4                # cyclic option rotations for debiasing
    max_seq_len: int = 2048


class Qwen3DecisionModel(DecisionModel):
    """Qwen3-8B + QLoRA + MLP decision head with AnyJev permutation debiasing."""

    def __init__(self, cfg: Qwen3Config | None = None):
        self.cfg = cfg or Qwen3Config()
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
        base_model: str = DEFAULT_MODEL,
        cfg: Qwen3Config | None = None,
    ) -> "Qwen3DecisionModel":
        """Load a fine-tuned checkpoint (LoRA adapters + MLP head)."""
        from peft import PeftModel

        obj = cls(cfg)
        obj._load_base(base_model)
        obj.backbone = PeftModel.from_pretrained(obj.backbone, checkpoint)
        head_path = f"{checkpoint}/decision_head.pt"
        obj.head.load_state_dict(torch.load(head_path, map_location=obj.cfg.device))
        return obj

    def load_base_for_training(self) -> None:
        """Load base model + attach LoRA adapters + init MLP head for training."""
        from peft import LoraConfig, get_peft_model, prepare_model_for_kbit_training

        self._load_base(self.cfg.model_name)

        self.backbone = prepare_model_for_kbit_training(
            self.backbone,
            use_gradient_checkpointing=True,
        )
        lora_cfg = LoraConfig(
            r=self.cfg.lora_r,
            lora_alpha=self.cfg.lora_alpha,
            lora_dropout=self.cfg.lora_dropout,
            target_modules=self.cfg.lora_target_modules,
            bias="none",
            task_type="CAUSAL_LM",
        )
        self.backbone = get_peft_model(self.backbone, lora_cfg)
        self.backbone.print_trainable_parameters()

    def _load_base(self, model_name: str) -> None:
        cfg = self.cfg

        # Tokenizer
        self.tokenizer = AutoTokenizer.from_pretrained(
            model_name,
            padding_side="left",
            trust_remote_code=True,
        )
        if self.tokenizer.pad_token is None:
            self.tokenizer.pad_token = self.tokenizer.eos_token

        # Quantisation config
        bnb_cfg = None
        if cfg.load_in_4bit or cfg.load_in_8bit:
            from transformers import BitsAndBytesConfig
            compute_dtype = getattr(torch, cfg.bnb_4bit_compute_dtype)
            bnb_cfg = BitsAndBytesConfig(
                load_in_4bit=cfg.load_in_4bit,
                load_in_8bit=cfg.load_in_8bit,
                bnb_4bit_quant_type=cfg.bnb_4bit_quant_type,
                bnb_4bit_compute_dtype=compute_dtype,
                bnb_4bit_use_double_quant=cfg.bnb_double_quant,
            )

        self.backbone = AutoModelForCausalLM.from_pretrained(
            model_name,
            quantization_config=bnb_cfg,
            device_map="auto" if cfg.device == "cuda" else cfg.device,
            torch_dtype=torch.bfloat16,
            trust_remote_code=True,
        )

        hidden_size = self.backbone.config.hidden_size
        self.head = DecisionHead(hidden_size).to(
            dtype=torch.bfloat16,
            device=cfg.device if cfg.device != "cuda" else "cuda:0",
        )

    # ------------------------------------------------------------------
    # Inference
    # ------------------------------------------------------------------

    def score_options(self, inp: DecisionInput) -> list[float]:
        """Score options with permutation debiasing (AnyJev approach)."""
        n = len(inp.options)
        perms = _cyclic_permutations(n, max_k=self.cfg.n_permutations)

        # accumulated scores[original_option_index] summed over permutations
        acc = [0.0] * n

        for perm in perms:
            prompt, _ = _format_prompt(inp.context, inp.options, perm)
            hidden_states = self._get_hidden_states(prompt)  # [n_options, hidden]
            # hidden_states is a single vector at the answer position for each option
            # here we score each option slot from a single forward pass
            scores_perm = self._score_from_hidden(hidden_states, n)

            # unscramble: map permuted position → original index
            for pos, orig_idx in enumerate(perm):
                acc[orig_idx] += scores_perm[pos]

        avg = [s / len(perms) for s in acc]
        return avg

    def decide(self, inp: DecisionInput) -> DecisionOutput:
        scores = self.score_options(inp)
        chosen = int(torch.tensor(scores).argmax().item())
        return DecisionOutput(chosen_index=chosen, scores=scores)

    def _get_hidden_states(self, prompt: str) -> torch.Tensor:
        """Run one forward pass, return the last hidden state at the answer position."""
        assert self.backbone is not None and self.tokenizer is not None
        enc = self.tokenizer(
            prompt,
            return_tensors="pt",
            truncation=True,
            max_length=self.cfg.max_seq_len,
        ).to(self.backbone.device)

        with torch.no_grad():
            out = self.backbone(**enc, output_hidden_states=True)

        # Last hidden state, last token position: [1, hidden_size]
        return out.hidden_states[-1][0, -1, :]  # [hidden_size]

    def _score_from_hidden(self, hidden: torch.Tensor, n_options: int) -> list[float]:
        """Apply MLP head.  hidden is [hidden_size] from a single forward pass.

        For the MLP-per-option variant (future): run a separate forward pass per
        option string and score each.  For now: the MLP maps the shared answer-
        position hidden state to a scalar; we use logit differences over label
        tokens as the per-option scores, then add the MLP correction.
        """
        assert self.head is not None
        correction = self.head(hidden.unsqueeze(0)).item()  # scalar bias term
        # TODO: per-option hidden states for full MLP-per-option scoring
        # Placeholder: uniform correction (MLP not yet discriminative without training)
        return [correction] * n_options

    # ------------------------------------------------------------------
    # Training helpers
    # ------------------------------------------------------------------

    def trainable_parameters(self):
        """Yield (name, param) for parameters that require gradients."""
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

    def forward_for_training(
        self,
        input_ids: torch.Tensor,
        attention_mask: torch.Tensor,
        answer_positions: torch.Tensor,   # [batch] — index of answer token in seq
        option_indices: torch.Tensor,     # [batch, n_options] — original option order per sample
    ) -> torch.Tensor:
        """Return per-sample logits over options for cross-entropy loss.

        answer_positions marks where in the sequence to read the hidden state.
        option_indices maps permuted position → original option for unshuffling.
        """
        out = self.backbone(
            input_ids=input_ids,
            attention_mask=attention_mask,
            output_hidden_states=True,
        )
        last_hidden = out.hidden_states[-1]  # [B, T, H]

        # Gather hidden state at answer position for each batch item
        pos = answer_positions.unsqueeze(-1).unsqueeze(-1)  # [B, 1, 1]
        pos = pos.expand(-1, 1, last_hidden.size(-1))       # [B, 1, H]
        h = last_hidden.gather(1, pos).squeeze(1)           # [B, H]

        scores = self.head(h)  # [B] scalar — used as bias; extend to per-option later
        # TODO: expand to [B, n_options] once per-option hidden states are supported
        return scores
