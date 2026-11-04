"""Experiment 001 — evaluate on MMLU val: zero-shot Qwen3-8B vs a trained checkpoint.

Zero-shot = the same scorer with no adapter and a zero head (identical to step 0).

    python/.venv/bin/python python/experiments/exp_001_qlora/evaluate.py \
        --checkpoint checkpoints/exp_001_qlora/best --tag best
"""

import argparse
import json
import sys
from pathlib import Path

import torch

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.loader import load_dataset_from_jsonl
from jeff.models.qwen3_decision import Qwen3Config, Qwen3DecisionModel
from jeff.training.qlora_trainer import QLoRATrainer, QLoRATrainingConfig


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--split", default="val", choices=["val", "test"])
    ap.add_argument("--skip-zeroshot", action="store_true")
    args = ap.parse_args()

    data = list(load_dataset_from_jsonl(REPO_ROOT / f"data/processed/mmlu_{args.split}.jsonl"))
    tr = QLoRATrainer(QLoRATrainingConfig())
    model = Qwen3DecisionModel(Qwen3Config())
    model._load_base(model.cfg.model_name)
    model.backbone.eval()
    out_dir = EXP_DIR / "results"
    out_dir.mkdir(exist_ok=True)

    if not args.skip_zeroshot:
        res = tr.evaluate(model, data, f"zeroshot_{args.split}")
        (out_dir / f"zeroshot_{args.split}.json").write_text(json.dumps(res, indent=2) + "\n")

    from peft import PeftModel
    ck = REPO_ROOT / args.checkpoint
    model.backbone = PeftModel.from_pretrained(model.backbone, str(ck))
    model.head.load_state_dict(torch.load(ck / "decision_head.pt"))
    model.backbone.eval()
    res = tr.evaluate(model, data, f"{args.tag}_{args.split}")
    res["checkpoint"] = args.checkpoint
    (out_dir / f"{args.tag}_{args.split}.json").write_text(json.dumps(res, indent=2) + "\n")


if __name__ == "__main__":
    main()
