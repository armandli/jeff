"""Experiment 005 — QLoRA SFT of Qwen3-8B on the RUN/AGENT router data.

Resumes automatically from <output.dir>/latest. From repo root:
    python/.venv/bin/python python/experiments/exp_005_shell_router/train.py
    python/.venv/bin/python python/experiments/exp_005_shell_router/train.py \
        --max-steps 2 --output-dir /tmp/x     # smoke test
"""

import argparse
import sys
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.loader import load_dataset_from_jsonl  # noqa: E402
from jeff.training.qlora_trainer import QLoRATrainer, QLoRATrainingConfig  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-steps", type=int, default=None)
    ap.add_argument("--output-dir", default=None)
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    m, t, ev, d = cfg["model"], cfg["training"], cfg["eval"], cfg["data"]
    processed = REPO_ROOT / d["out_dir"]
    train_ds = load_dataset_from_jsonl(processed / f"{d['prefix']}_train.jsonl")
    eval_ds = load_dataset_from_jsonl(processed / f"{d['prefix']}_val.jsonl")

    out = Path(args.output_dir) if args.output_dir else REPO_ROOT / cfg["output"]["dir"]
    tcfg = QLoRATrainingConfig(
        model_name=m["name"], load_in_4bit=m["load_in_4bit"],
        lora_r=m["lora_r"], lora_alpha=m["lora_alpha"], lora_dropout=m["lora_dropout"],
        lora_target_modules=m["lora_target_modules"], max_seq_len=m["max_seq_len"],
        output_dir=str(out), results_dir=str(out),
        num_epochs=t["num_epochs"], max_steps=args.max_steps or t.get("max_steps"),
        batch_size=t["batch_size"], gradient_accumulation_steps=t["gradient_accumulation_steps"],
        learning_rate=t["learning_rate"], head_learning_rate=t["head_learning_rate"],
        warmup_ratio=t["warmup_ratio"], max_grad_norm=t["max_grad_norm"], bf16=t["bf16"],
        seed=t["seed"], eval_every=ev["every"], eval_subset=ev["subset"],
        save_every=50, wandb_project=cfg["wandb"]["project"], wandb_run=cfg["wandb"]["run_name"],
    )
    QLoRATrainer(tcfg).train_sft(train_ds, eval_ds)
    print(f"Done. Checkpoint at {out}/final")


if __name__ == "__main__":
    main()
