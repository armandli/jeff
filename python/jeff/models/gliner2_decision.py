"""GLiNER2 schema-classification decision model (exp_004).

A small DeBERTa-v3 encoder (fastino GLiNER2 family) that classifies a text
observation into one of a fixed set of action labels in a single forward pass.
Suits tasks with a small, fixed action set (ViZDoom), not open multiple choice.

    model = GLiNER2DecisionModel(GLiNER2Config(adapter="checkpoints/exp_004_gliner2_doom/best"))
    out = model.decide(DecisionInput(context="...", options=state.option_texts()))

Options must be "NAME: description" strings (or bare names); the NAME part is
the classification label, matching how prepare_data.py builds training records.
"""

from dataclasses import dataclass

from jeff.models.base import DecisionInput, DecisionModel, DecisionOutput

DEFAULT_MODEL = "fastino/gliner2.5-base-v1"
TASK = "action"


def option_label(option: str) -> str:
    return option.split(":", 1)[0].strip()


@dataclass
class GLiNER2Config:
    model_name: str = DEFAULT_MODEL
    adapter: str | None = None      # PEFT LoRA dir from train.py (None = pretrained only)
    device: str = "cuda"


class GLiNER2DecisionModel(DecisionModel):

    def __init__(self, cfg: GLiNER2Config | None = None):
        from gliner2 import AutoExtractor

        self.cfg = cfg or GLiNER2Config()
        model = AutoExtractor.from_pretrained(self.cfg.model_name)
        if self.cfg.adapter:
            from peft import PeftModel
            model = PeftModel.from_pretrained(model, self.cfg.adapter).merge_and_unload()
        self.model = model.to(self.cfg.device).eval()

    def _classify(self, inp: DecisionInput) -> tuple[int, float]:
        labels = [option_label(o) for o in inp.options]
        res = self.model.classify_text(inp.context, {TASK: labels}, include_confidence=True)
        pred = res[TASK]
        label, conf = (pred["label"], pred["confidence"]) if isinstance(pred, dict) else (pred, 1.0)
        return labels.index(label), float(conf)

    def score_options(self, inp: DecisionInput) -> list[float]:
        """One-hot-ish scores: the library only exposes the winning label's confidence."""
        idx, conf = self._classify(inp)
        return [conf if i == idx else 0.0 for i in range(len(inp.options))]

    def decide(self, inp: DecisionInput) -> DecisionOutput:
        idx, conf = self._classify(inp)
        return DecisionOutput(chosen_index=idx,
                              scores=[conf if i == idx else 0.0 for i in range(len(inp.options))])
