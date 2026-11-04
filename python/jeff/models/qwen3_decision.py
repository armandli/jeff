"""Qwen3-8B decision model with QLoRA and a lightweight linear readout head.

Architecture
------------
- Base: Qwen3-8B (hybrid thinking/instruct model), loaded in 4-bit NF4 via bitsandbytes
- LoRA adapters on q_proj / k_proj / v_proj / o_proj / gate_proj / up_proj / down_proj
- Per-option score, all from ONE forward pass over the prompt:
      score_i = log p(letter_i | prompt)  +  DecisionHead(h_i)
  where the letter log-prob is read from the LM head at the final (answer)
  position, renormalised over the option letters, and h_i is the last hidden
  state at the final token of option i's text in the prompt. The head is a
  single Linear(hidden_size, 1), zero-initialised, so an untrained model is
  exactly the zero-shot letter scorer (`qwen3_choice`) and training learns a
  correction on top of it.
- Permutation debiasing (AnyJev style): at inference all cyclic rotations of
  the option order are scored in one batch and averaged per original option;
  in training each example gets a random option order.

Training flow
-------------
1. SFT phase  : cross-entropy over the per-option scores
2. RLCD phase : DPO on (chosen, rejected) option pairs

Inference flow
--------------
  model = Qwen3DecisionModel.from_pretrained(path)
  out = model.decide(DecisionInput(context="...", options=["A", "B", "C"]))
"""

from dataclasses import dataclass, field

import torch
import torch.nn as nn
import torch.nn.functional as F
from transformers import AutoModelForCausalLM, AutoTokenizer

from jeff.models.base import DecisionInput, DecisionModel, DecisionOutput
from jeff.models.qwen3_choice import cyclic_permutations, format_prompt

DEFAULT_MODEL = "Qwen/Qwen3-8B"

# ---------------------------------------------------------------------------
# Linear readout head
# ---------------------------------------------------------------------------

class DecisionHead(nn.Module):
    """Maps a per-option hidden-state vector to a scalar score correction.

    Zero-initialised so it starts as a no-op and the letter logits drive early training.
    Kept in fp32.
    """

    def __init__(self, hidden_size: int):
        super().__init__()
        self.proj = nn.Linear(hidden_size, 1, bias=False)
        nn.init.zeros_(self.proj.weight)

    def forward(self, hidden: torch.Tensor) -> torch.Tensor:
        # hidden: [..., hidden_size] → [...]
        return self.proj(hidden.float()).squeeze(-1)


# ---------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------

def encode_example(tokenizer, context: str, options: list[str], perm: list[int]) -> dict:
    """Tokenise one prompt; return input_ids and the token index ending each option.

    option_pos[i] is the position of the last token of the option shown under
    letter i (i.e. options[perm[i]]).
    """
    prompt = format_prompt(context, options, perm)
    enc = tokenizer(prompt, add_special_tokens=False, return_offsets_mapping=True)
    offsets = enc["offset_mapping"]

    # character span end of each "X. option" line inside the Options block
    start = prompt.index("\n\nOptions:\n") + len("\n\nOptions:\n")
    option_pos = []
    cursor = start
    for i, j in enumerate(perm):
        line = f"{chr(ord('A') + i)}. {options[j]}"
        assert prompt.startswith(line, cursor)
        end_char = cursor + len(line)          # exclusive
        # last token whose span starts before end_char
        tok = max(t for t, (s, e) in enumerate(offsets) if s < end_char and e > s)
        option_pos.append(tok)
        cursor = end_char + 1                  # skip "\n"
    return {"input_ids": enc["input_ids"], "option_pos": option_pos}


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
    n_permutations: int = 0                # 0 = all cyclic rotations
    max_seq_len: int = 1024


class Qwen3DecisionModel(DecisionModel):
    """Qwen3-8B + QLoRA + linear decision head with AnyJev permutation debiasing."""

    def __init__(self, cfg: Qwen3Config | None = None):
        self.cfg = cfg or Qwen3Config()
        self.tokenizer = None
        self.backbone = None
        self.head = None
        self._letter_ids: dict[int, list[int]] = {}

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
        """Load a fine-tuned checkpoint (LoRA adapters + head)."""
        from peft import PeftModel

        obj = cls(cfg)
        obj._load_base(base_model)
        obj.backbone = PeftModel.from_pretrained(obj.backbone, checkpoint)
        obj.head.load_state_dict(torch.load(f"{checkpoint}/decision_head.pt",
                                            map_location=obj.head.proj.weight.device))
        obj.backbone.eval()
        return obj

    def load_base_for_training(self) -> None:
        """Load base model + attach LoRA adapters + init head for training."""
        from peft import LoraConfig, get_peft_model, prepare_model_for_kbit_training

        self._load_base(self.cfg.model_name)
        self.backbone = prepare_model_for_kbit_training(
            self.backbone,
            use_gradient_checkpointing=True,
            gradient_checkpointing_kwargs={"use_reentrant": False},
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
        self.tokenizer = AutoTokenizer.from_pretrained(model_name, padding_side="left")
        if self.tokenizer.pad_token is None:
            self.tokenizer.pad_token = self.tokenizer.eos_token

        bnb_cfg = None
        if cfg.load_in_4bit or cfg.load_in_8bit:
            from transformers import BitsAndBytesConfig
            bnb_cfg = BitsAndBytesConfig(
                load_in_4bit=cfg.load_in_4bit,
                load_in_8bit=cfg.load_in_8bit,
                bnb_4bit_quant_type=cfg.bnb_4bit_quant_type,
                bnb_4bit_compute_dtype=getattr(torch, cfg.bnb_4bit_compute_dtype),
                bnb_4bit_use_double_quant=cfg.bnb_double_quant,
            )
        self.backbone = AutoModelForCausalLM.from_pretrained(
            model_name,
            quantization_config=bnb_cfg,
            device_map="cuda:0" if cfg.device == "cuda" else cfg.device,
            dtype=torch.bfloat16,
        )
        dev = "cuda:0" if cfg.device == "cuda" else cfg.device
        self.head = DecisionHead(self.backbone.config.hidden_size).to(dev)

    def letter_ids(self, n: int) -> list[int]:
        if n not in self._letter_ids:
            ids = []
            for i in range(n):
                tok = self.tokenizer.encode(chr(ord("A") + i), add_special_tokens=False)
                assert len(tok) == 1
                ids.append(tok[0])
            self._letter_ids[n] = ids
        return self._letter_ids[n]

    # ------------------------------------------------------------------
    # Core scoring (shared by training and inference)
    # ------------------------------------------------------------------

    def _lm_parts(self):
        """(decoder, lm_head) of the (possibly PEFT-wrapped) causal LM."""
        m = self.backbone
        if hasattr(m, "get_base_model"):
            m = m.get_base_model()        # LoRA layers are injected in place
        return m.model, m.lm_head

    def forward_for_training(
        self,
        input_ids: torch.Tensor,          # [B, T] left-padded
        attention_mask: torch.Tensor,     # [B, T]
        option_pos: torch.Tensor,         # [B, N] token index ending each shown option
        option_mask: torch.Tensor,        # [B, N] True where the option exists
    ) -> tuple[torch.Tensor, torch.Tensor]:
        """Return (scores, letter_logprobs), both [B, N] in shown-letter order.

        Padded option slots get -inf. Only the final position goes through the
        LM head, so no [B, T, vocab] logits tensor is ever materialised.
        """
        decoder, lm_head = self._lm_parts()
        hidden = decoder(input_ids=input_ids, attention_mask=attention_mask).last_hidden_state
        B, N = option_pos.shape

        letters = self.letter_ids(N)
        last = hidden[:, -1, :]                                       # [B, H]
        w = lm_head.weight[letters]                                   # [N, H]
        letter_logits = last.float() @ w.float().T                    # [B, N]
        letter_logits = letter_logits.masked_fill(~option_mask, float("-inf"))
        letter_lp = torch.log_softmax(letter_logits, dim=-1)

        idx = option_pos.unsqueeze(-1).expand(-1, -1, hidden.size(-1))
        h_opt = hidden.gather(1, idx)                                 # [B, N, H]
        scores = letter_lp + self.head(h_opt)
        scores = scores.masked_fill(~option_mask, float("-inf"))
        return scores, letter_lp

    def collate(self, items: list[dict]) -> dict:
        """Left-pad encoded items into tensors (option positions shift by the pad)."""
        pad = self.tokenizer.pad_token_id
        T = max(len(it["input_ids"]) for it in items)
        N = max(len(it["option_pos"]) for it in items)
        ids, am, pos, om = [], [], [], []
        for it in items:
            k = T - len(it["input_ids"])
            ids.append([pad] * k + it["input_ids"])
            am.append([0] * k + [1] * len(it["input_ids"]))
            p = [x + k for x in it["option_pos"]]
            pos.append(p + [p[-1]] * (N - len(p)))
            om.append([True] * len(p) + [False] * (N - len(p)))
        out = {
            "input_ids": torch.tensor(ids),
            "attention_mask": torch.tensor(am),
            "option_pos": torch.tensor(pos),
            "option_mask": torch.tensor(om),
        }
        if "label" in items[0]:
            out["labels"] = torch.tensor([it["label"] for it in items])
        return out

    # ------------------------------------------------------------------
    # Inference
    # ------------------------------------------------------------------

    @torch.no_grad()
    def score_options_both(self, inp: DecisionInput) -> tuple[list[float], list[float]]:
        """Debiased (head+letter, letter-only) log-scores per original option."""
        n = len(inp.options)
        perms = cyclic_permutations(n, self.cfg.n_permutations)
        items = [encode_example(self.tokenizer, inp.context, inp.options, p) for p in perms]
        batch = {k: v.to(self.head.proj.weight.device) for k, v in self.collate(items).items()}
        scores, letter_lp = self.forward_for_training(**batch)
        out = []
        for s in (scores, letter_lp):
            acc = torch.zeros(n)
            for row, perm in zip(s.cpu(), perms):
                for pos, orig in enumerate(perm):
                    acc[orig] += row[pos]
            out.append(torch.log_softmax(acc / len(perms), dim=-1).tolist())
        return out[0], out[1]

    def score_options(self, inp: DecisionInput) -> list[float]:
        return self.score_options_both(inp)[0]

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
