"""Qwen3 option-letter scorer with batched permutation debiasing.

A working zero-shot (or LoRA fine-tuned) DecisionModel for Qwen3 chat models:

  - the prompt uses Qwen3's chat format in non-thinking mode (empty <think> block)
    and lists options as "A. ...", "B. ...";
  - the score of each option is the log-probability of its letter as the first
    assistant token, renormalised over the option letters;
  - all K cyclic rotations of the option order are scored in ONE batched forward
    pass and averaged per original option, which cancels the model's preference
    for particular letters/positions (AnyJev-style debiasing) at little extra
    latency.

Unlike `qwen3_decision.Qwen3DecisionModel` (whose linear head is still a
placeholder), this reads the LM head directly, so it works out of the box.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

import torch

from jeff.models.base import DecisionInput, DecisionModel, DecisionOutput

SYSTEM_PROMPT = (
    "You are a fast, precise decision-making system. Given a situation and a list "
    "of options, reply with only the letter of the best option."
)


@dataclass
class Qwen3ChoiceConfig:
    model_name: str = "Qwen/Qwen3-8B"
    adapter: str | None = None            # optional LoRA checkpoint dir
    device: str = "cuda"
    load_in_4bit: bool = True             # Qwen3-8B in bf16 (~16 GB) does not fit a 16 GB GPU
    n_permutations: int = 0               # 0 = all cyclic rotations, 1 = no debiasing
    system_prompt: str = SYSTEM_PROMPT


def format_prompt(context: str, options: list[str], perm: list[int],
                  system_prompt: str = SYSTEM_PROMPT) -> str:
    """Chat prompt presenting options[perm[i]] under letter i, ending where the letter goes."""
    letters = [chr(ord("A") + i) for i in range(len(perm))]
    opts = "\n".join(f"{l}. {options[j]}" for l, j in zip(letters, perm))
    return (
        f"<|im_start|>system\n{system_prompt}<|im_end|>\n"
        f"<|im_start|>user\n{context}\n\nOptions:\n{opts}\n\n"
        f"Answer with one letter.<|im_end|>\n"
        # Empty think block = Qwen3 non-thinking mode (matches the official
        # chat template with enable_thinking=False).
        f"<|im_start|>assistant\n<think>\n\n</think>\n\n"
    )


def cyclic_permutations(n: int, k: int = 0) -> list[list[int]]:
    perms = [list(range(i, n)) + list(range(i)) for i in range(n)]
    return perms if k <= 0 else perms[:k]


class Qwen3ChoiceModel(DecisionModel):
    def __init__(self, cfg: Qwen3ChoiceConfig | None = None):
        from transformers import AutoModelForCausalLM, AutoTokenizer

        self.cfg = cfg = cfg or Qwen3ChoiceConfig()
        self.tokenizer = AutoTokenizer.from_pretrained(cfg.model_name, padding_side="left")
        quant = None
        if cfg.load_in_4bit and cfg.device != "cpu":
            from transformers import BitsAndBytesConfig
            quant = BitsAndBytesConfig(
                load_in_4bit=True, bnb_4bit_quant_type="nf4",
                bnb_4bit_compute_dtype=torch.bfloat16, bnb_4bit_use_double_quant=True,
            )
        self.model = AutoModelForCausalLM.from_pretrained(
            cfg.model_name, dtype=torch.bfloat16, quantization_config=quant,
            device_map=cfg.device,
        )
        if cfg.adapter:
            from peft import PeftModel
            self.model = PeftModel.from_pretrained(self.model, str(Path(cfg.adapter)))
        self.model.eval()
        self._letter_ids: dict[int, list[int]] = {}

    def _letters(self, n: int) -> list[int]:
        if n not in self._letter_ids:
            ids = []
            for i in range(n):
                tok = self.tokenizer.encode(chr(ord("A") + i), add_special_tokens=False)
                assert len(tok) == 1, f"letter {chr(ord('A') + i)} is not a single token"
                ids.append(tok[0])
            self._letter_ids[n] = ids
        return self._letter_ids[n]

    @torch.no_grad()
    def score_options(self, inp: DecisionInput) -> list[float]:
        """Debiased log-probabilities (normalised over the options) per original option."""
        n = len(inp.options)
        perms = cyclic_permutations(n, self.cfg.n_permutations)
        prompts = [format_prompt(inp.context, inp.options, p, self.cfg.system_prompt)
                   for p in perms]
        enc = self.tokenizer(prompts, return_tensors="pt", padding=True).to(self.model.device)
        logits = self.model(**enc).logits[:, -1, :].float()           # [K, vocab]
        letter_lp = torch.log_softmax(logits[:, self._letters(n)], dim=-1)  # [K, n]

        acc = torch.zeros(n)
        for row, perm in zip(letter_lp.cpu(), perms):
            for pos, orig in enumerate(perm):
                acc[orig] += row[pos]
        avg = acc / len(perms)
        # renormalise the averaged log-probs into a distribution over options
        return torch.log_softmax(avg, dim=-1).tolist()

    def decide(self, inp: DecisionInput) -> DecisionOutput:
        scores = self.score_options(inp)
        chosen = max(range(len(scores)), key=scores.__getitem__)
        return DecisionOutput(chosen_index=chosen, scores=scores)

    @staticmethod
    def confidence(scores: list[float]) -> float:
        """Probability of the chosen option."""
        return math.exp(max(scores))
