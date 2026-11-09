"""Experiment 006 — train a fast encoder router on zsh RUN/AGENT data.

Supports three model candidates; select with --model-key (default: modernbert_base).
Each model is saved under checkpoints/exp_006_zsh_router/<model_key>/.

    # Train the fastest model (default):
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py

    # Train each candidate (can run in parallel on separate GPUs):
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py --model-key modernbert_base
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py --model-key modernbert_large
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py --model-key electra_base

    # Smoke test (2 steps, temp output):
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py --max-steps 2 --output-dir /tmp/x
"""

import argparse
import sys
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.loader import load_dataset_from_jsonl  # noqa: E402
from jeff.training.encoder_trainer import EncoderTrainer, EncoderTrainingConfig  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--model-key", default=None,
        help="key in config.yaml models section (default: first model = modernbert_base)",
    )
    ap.add_argument("--max-steps", type=int, default=None)
    ap.add_argument("--output-dir", default=None, help="override base output dir")
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    model_key = args.model_key or next(iter(cfg["models"]))
    if model_key not in cfg["models"]:
        raise SystemExit(f"Unknown --model-key {model_key!r}. "
                         f"Valid keys: {list(cfg['models'])}")
    m = cfg["models"][model_key]
    t, ev, d = cfg["training"], cfg["eval"], cfg["data"]
    processed = REPO_ROOT / d["out_dir"]

    train_ds = load_dataset_from_jsonl(processed / f"{d['prefix']}_train.jsonl")
    eval_ds = load_dataset_from_jsonl(processed / f"{d['prefix']}_val.jsonl")

    out_base = Path(args.output_dir) if args.output_dir else REPO_ROOT / cfg["output"]["dir"]
    out = out_base / model_key

    tcfg = EncoderTrainingConfig(
        model_name=m["name"],
        lora_r=m["lora_r"],
        lora_alpha=m["lora_alpha"],
        lora_dropout=m["lora_dropout"],
        lora_target_modules=m["lora_target_modules"],
        max_seq_len=m["max_seq_len"],
        output_dir=str(out),
        results_dir=str(out),   # per-model dir so eval_history.json doesn't collide
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
        eval_subset=ev["subset"],
        speed_eval_every=ev.get("speed_every", 0),
        wandb_project=cfg["wandb"]["project"],
        wandb_run=f"{cfg['wandb']['run_name']}_{model_key}",
    )
    EncoderTrainer(tcfg).train_sft(train_ds, eval_ds)
    print(f"Done. Checkpoint at {out}/final")


if __name__ == "__main__":
    main()
