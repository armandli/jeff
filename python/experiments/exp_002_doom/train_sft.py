"""Experiment 002 step 2 — QLoRA SFT of Qwen3-8B on teacher-labelled Doom states.

Resumes automatically from <output_dir>/latest. From repo root:
    python/.venv/bin/python python/experiments/exp_002_doom/train_sft.py
    python/.venv/bin/python python/experiments/exp_002_doom/train_sft.py --max-steps 2 --output-dir /tmp/x
Then play with the adapter (letter + head scorer):
    python/.venv/bin/python python/experiments/exp_002_doom/play.py --brain qwen3 \
        --adapter checkpoints/exp_002_doom_sft/best --tag qwen3_sft
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
    ap.add_argument("--output-dir", default=None)
    ap.add_argument("--section", default="sft",
                    help="config section; 'sft_improved' overrides data/output of 'sft'")
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    s = {**cfg["sft"], **cfg.get(args.section, {})}
    train_ds = load_dataset_from_jsonl(resolve(s["train"]))
    eval_ds = load_dataset_from_jsonl(resolve(s["eval"]))
    out = resolve(args.output_dir or s["output_dir"])
    tcfg = QLoRATrainingConfig(
        model_name=cfg["model"]["name"], load_in_4bit=cfg["model"]["load_in_4bit"],
        lora_r=s["lora_r"], lora_alpha=s["lora_alpha"], lora_dropout=s["lora_dropout"],
        lora_target_modules=s["lora_target_modules"], max_seq_len=s["max_seq_len"],
        output_dir=str(out), results_dir=str(out),
        num_epochs=s["num_epochs"], max_steps=args.max_steps or s.get("max_steps"),
        batch_size=s["batch_size"], gradient_accumulation_steps=s["gradient_accumulation_steps"],
        learning_rate=s["learning_rate"], head_learning_rate=s["head_learning_rate"],
        warmup_ratio=s["warmup_ratio"], max_grad_norm=s["max_grad_norm"], bf16=s["bf16"],
        seed=s["seed"], eval_every=s["eval_every"], eval_subset=s["eval_subset"],
        save_every=50, wandb_project="jeff", wandb_run=s["wandb_run"],
    )
    QLoRATrainer(tcfg).train_sft(train_ds, eval_ds)
    print(f"Done. Checkpoint at {out}/final")


if __name__ == "__main__":
    main()
