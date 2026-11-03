import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

from jeff.models.base import DecisionInput, DecisionModel, DecisionOutput


class QwenDecisionModel(DecisionModel):
    """Qwen 2.5 fine-tuned for constrained option selection."""

    def __init__(self, model_name_or_path: str, device: str = "cuda"):
        self.device = device
        self.tokenizer = AutoTokenizer.from_pretrained(model_name_or_path)
        self.model = AutoModelForCausalLM.from_pretrained(
            model_name_or_path,
            torch_dtype=torch.bfloat16,
            device_map=device,
        )
        self.model.eval()

    def score_options(self, inp: DecisionInput) -> list[float]:
        """Score each option via next-token log-prob after the context."""
        prompt = self._format_prompt(inp)
        enc = self.tokenizer(prompt, return_tensors="pt").to(self.device)

        scores = []
        with torch.no_grad():
            logits = self.model(**enc).logits[0, -1]  # last position
            log_probs = torch.log_softmax(logits, dim=-1)

        for option in inp.options:
            option_ids = self.tokenizer.encode(option, add_special_tokens=False)
            # Score by first token of each option (fast System 1 heuristic)
            scores.append(log_probs[option_ids[0]].item())

        return scores

    def decide(self, inp: DecisionInput) -> DecisionOutput:
        scores = self.score_options(inp)
        chosen = int(torch.tensor(scores).argmax().item())
        return DecisionOutput(chosen_index=chosen, scores=scores)

    def _format_prompt(self, inp: DecisionInput) -> str:
        opts = "\n".join(f"{i}. {o}" for i, o in enumerate(inp.options))
        return f"{inp.context}\n\nOptions:\n{opts}\n\nAnswer:"

    def export_torchscript(self, path: str) -> None:
        """Export for LibTorch C++ inference."""
        scripted = torch.jit.trace(
            self.model,
            example_inputs=(torch.zeros(1, 1, dtype=torch.long, device=self.device),),
        )
        torch.jit.save(scripted, path)
