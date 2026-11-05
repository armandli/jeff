"""Experiment 003 — evaluation with accuracy AND speed metrics.

Runs two passes:
  1. Zero-shot (no checkpoint): encoder + zero-init head
  2. Fine-tuned checkpoint (--checkpoint)

Outputs per pass:
  - accuracy, NLL, pred_dist  (same as exp_001)
  - ms_per_decision (sequential, single example at a time)
  - decisions/sec in sequential and batched modes
  - speedup ratio vs exp_001 Qwen3-8B QLoRA baseline

Usage:
    python/.venv/bin/python python/experiments/exp_003_fast_encoder/evaluate.py \\
        --checkpoint checkpoints/exp_003_fast_encoder/best --tag best

    # Zero-shot only:
    python/.venv/bin/python python/experiments/exp_003_fast_encoder/evaluate.py \\
        --skip-finetuned

    # Speed benchmark only (no accuracy eval):
    python/.venv/bin/python python/experiments/exp_003_fast_encoder/evaluate.py \\
        --speed-only --checkpoint checkpoints/exp_003_fast_encoder/best
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
from jeff.models.modernbert_decision import ModernBERTConfig, ModernBERTDecisionModel
from jeff.training.encoder_trainer import EncoderTrainer, EncoderTrainingConfig

# exp_001 reference number for speedup calculation
EXP001_MS_PER_DECISION = 202.2   # Qwen3-8B QLoRA, sequential, MMLU val


def build_zeroshot(model_name: str) -> ModernBERTDecisionModel:
    model = ModernBERTDecisionModel(ModernBERTConfig(model_name=model_name))
    model._load_base(model_name)
    model.backbone.eval()
    return model


def load_finetuned(checkpoint: str, model_name: str) -> ModernBERTDecisionModel:
    model = ModernBERTDecisionModel(ModernBERTConfig(model_name=model_name))
    model._load_base(model_name)
    from peft import PeftModel
    ck = Path(checkpoint)
    model.backbone = PeftModel.from_pretrained(model.backbone, str(ck))
    model.head.load_state_dict(torch.load(ck / "decision_head.pt"))
    model.backbone.eval()
    return model


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", default=None)
    ap.add_argument("--tag", default="finetuned")
    ap.add_argument("--split", default="val", choices=["val", "test"])
    ap.add_argument("--model", default="answerdotai/ModernBERT-large")
    ap.add_argument("--skip-zeroshot", action="store_true")
    ap.add_argument("--skip-finetuned", action="store_true")
    ap.add_argument("--speed-only", action="store_true",
                    help="skip accuracy eval, only run speed benchmark")
    ap.add_argument("--speed-batch-size", type=int, default=32)
    args = ap.parse_args()

    data = list(load_dataset_from_jsonl(
        REPO_ROOT / f"data/processed/mmlu_{args.split}.jsonl"
    ))
    out_dir = EXP_DIR / "results"
    out_dir.mkdir(exist_ok=True)

    tr = EncoderTrainer(EncoderTrainingConfig(model_name=args.model))

    def run(model, tag: str):
        if args.speed_only:
            speed = tr.speed_benchmark(model, data, batch_size=args.speed_batch_size)
            speed["speedup_vs_exp001"] = round(EXP001_MS_PER_DECISION / speed["sequential_ms_p50"], 1)
            speed["tag"] = tag
            print("SPEED " + json.dumps(speed))
            (out_dir / f"speed_{tag}_{args.split}.json").write_text(
                json.dumps(speed, indent=2) + "\n"
            )
            return

        res = tr.evaluate(model, data, f"{tag}_{args.split}")
        speed = tr.speed_benchmark(model, data, batch_size=args.speed_batch_size)
        speed["speedup_vs_exp001"] = round(EXP001_MS_PER_DECISION / speed["sequential_ms_p50"], 1)
        full = {**res, "speed": speed}
        path = out_dir / f"{tag}_{args.split}.json"
        path.write_text(json.dumps(full, indent=2) + "\n")
        print(f"Wrote {path}")
        print(f"  accuracy     : {res['accuracy']:.4f}")
        print(f"  seq ms/dec   : {speed['sequential_ms_p50']:.1f}")
        print(f"  dec/sec batch: {speed['batch_decisions_per_sec']:.0f}")
        print(f"  speedup vs exp001: {speed['speedup_vs_exp001']:.1f}×")

    if not args.skip_zeroshot:
        model = build_zeroshot(args.model)
        run(model, "zeroshot")

    if not args.skip_finetuned and not args.speed_only:
        if not args.checkpoint:
            print("Pass --checkpoint <path> to evaluate a fine-tuned model.")
            return
        model = load_finetuned(args.checkpoint, args.model)
        run(model, args.tag)
    elif args.speed_only:
        if not args.checkpoint:
            model = build_zeroshot(args.model)
            run(model, "zeroshot")
        else:
            model = load_finetuned(args.checkpoint, args.model)
            run(model, args.tag)


if __name__ == "__main__":
    main()
