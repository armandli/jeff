"""Experiment 003 — Fast encoder decision model training.

Run from repo root (resumes from checkpoints/exp_003_fast_encoder/latest):
    python/.venv/bin/python python/experiments/exp_003_fast_encoder/train.py
    python/.venv/bin/python python/experiments/exp_003_fast_encoder/train.py --max-steps 5

Options:
  --max-steps N     Hard cap on optimizer steps (useful for smoke tests)
  --train-limit N   Use only the first N training examples
  --eval-subset N   Override the per-step eval subset size
  --model NAME      Override model.name from config (e.g. answerdotai/ModernBERT-base)
  --output-dir DIR  Override output directory
"""

import argparse
import sys
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.loader import load_dataset_from_jsonl
from jeff.training.encoder_trainer import EncoderTrainer, EncoderTrainingConfig


def resolve(p: str) -> Path:
    p = Path(p).expanduser()
    return p if p.is_absolute() else REPO_ROOT / p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-steps", type=int, default=None)
    ap.add_argument("--train-limit", type=int, default=None)
    ap.add_argument("--eval-subset", type=int, default=None)
    ap.add_argument("--model", default=None, help="override model.name")
    ap.add_argument("--output-dir", default=None)
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    m, t, ev = cfg["model"], cfg["training"], cfg["eval"]

    train_ds = load_dataset_from_jsonl(resolve(cfg["data"]["train"]))
    if args.train_limit:
        train_ds.examples = train_ds.examples[:args.train_limit]
    eval_ds = (load_dataset_from_jsonl(resolve(cfg["data"]["eval"]))
               if cfg["data"].get("eval") else None)

    tcfg = EncoderTrainingConfig(
        model_name=args.model or m["name"],
        full_finetune=m["full_finetune"],
        lora_r=m["lora_r"],
        lora_alpha=m["lora_alpha"],
        lora_dropout=m["lora_dropout"],
        lora_target_modules=m["lora_target_modules"],
        max_seq_len=m["max_seq_len"],
        n_permutations=m["n_permutations"],
        output_dir=str(resolve(args.output_dir or cfg["output"]["dir"])),
        results_dir=str(resolve(cfg["output"]["results_dir"])) if not args.output_dir else None,
        num_epochs=t["num_epochs"],
        max_steps=args.max_steps or t.get("max_steps"),
        batch_size=t["batch_size"],
        gradient_accumulation_steps=t["gradient_accumulation_steps"],
        learning_rate=t["learning_rate"],
        head_learning_rate=t["head_learning_rate"],
        warmup_ratio=t["warmup_ratio"],
        max_grad_norm=t["max_grad_norm"],
        bf16=t["bf16"],
        seed=t["seed"],
        eval_every=ev["every"],
        eval_subset=args.eval_subset if args.eval_subset is not None else ev["subset"],
        wandb_project=cfg["wandb"]["project"],
        wandb_run=cfg["wandb"]["run_name"],
    )

    EncoderTrainer(tcfg).train_sft(train_ds, eval_ds)
    print(f"Done. Checkpoint at {tcfg.output_dir}/final")


if __name__ == "__main__":
    main()
