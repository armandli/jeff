"""Experiment 001 — QLoRA on Qwen3-8B with a linear decision head.

Run from repo root (resumes automatically from checkpoints/exp_001_qlora/latest):
    python/.venv/bin/python python/experiments/exp_001_qlora/train.py
    python/.venv/bin/python python/experiments/exp_001_qlora/train.py --max-steps 2   # smoke test
"""

import argparse
import sys
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.loader import load_dataset_from_jsonl
from jeff.training.qlora_trainer import QLoRATrainer, QLoRATrainingConfig


def resolve(p: str) -> Path:
    p = Path(p).expanduser()
    return p if p.is_absolute() else REPO_ROOT / p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-steps", type=int, default=None)
    ap.add_argument("--train-limit", type=int, default=None, help="use only the first N train examples")
    ap.add_argument("--eval-subset", type=int, default=None)
    ap.add_argument("--no-full-eval", action="store_true")
    ap.add_argument("--output-dir", default=None)
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    m, t, ev = cfg["model"], cfg["training"], cfg["eval"]

    train_ds = load_dataset_from_jsonl(resolve(cfg["data"]["train"]))
    if args.train_limit:
        train_ds.examples = train_ds.examples[: args.train_limit]
    eval_ds = load_dataset_from_jsonl(resolve(cfg["data"]["eval"])) if cfg["data"].get("eval") else None

    tcfg = QLoRATrainingConfig(
        model_name=m["name"], load_in_4bit=m["load_in_4bit"],
        lora_r=m["lora_r"], lora_alpha=m["lora_alpha"], lora_dropout=m["lora_dropout"],
        lora_target_modules=m["lora_target_modules"], max_seq_len=m["max_seq_len"],
        output_dir=str(resolve(args.output_dir or cfg["output"]["dir"])),
        results_dir=str(resolve(cfg["output"]["results_dir"])) if not args.output_dir else None,
        num_epochs=t["num_epochs"], max_steps=args.max_steps or t.get("max_steps"),
        batch_size=t["batch_size"], gradient_accumulation_steps=t["gradient_accumulation_steps"],
        learning_rate=t["learning_rate"], head_learning_rate=t["head_learning_rate"],
        warmup_ratio=t["warmup_ratio"], max_grad_norm=t["max_grad_norm"], bf16=t["bf16"],
        seed=t["seed"], eval_every=ev["every"],
        eval_subset=args.eval_subset if args.eval_subset is not None else ev["subset"],
        wandb_project=cfg["wandb"]["project"], wandb_run=cfg["wandb"]["run_name"],
    )
    if args.no_full_eval and eval_ds is not None:
        # keep the subset for periodic eval, skip the full-val pass at the end
        from jeff.data.loader import DecisionDataset
        import random
        eval_ds = DecisionDataset(random.Random(1).sample(list(eval_ds), min(tcfg.eval_subset, len(eval_ds))))

    QLoRATrainer(tcfg).train_sft(train_ds, eval_ds)
    print(f"Done. Checkpoint at {tcfg.output_dir}/final")


if __name__ == "__main__":
    main()
