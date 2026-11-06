"""Experiment 002 step 3 — RLCD: option-level DPO from game-outcome preferences.

Starts from the step-2 SFT adapter. Resumes from <output_dir>/latest. From repo root:
    python/.venv/bin/python python/experiments/exp_002_doom/train_dpo.py
"""

import argparse
import sys
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.loader import load_dataset_from_jsonl
from jeff.training.option_dpo import OptionDPOConfig, OptionDPOTrainer, load_pairs


def resolve(p: str) -> Path:
    p = Path(p).expanduser()
    return p if p.is_absolute() else REPO_ROOT / p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-steps", type=int, default=None)
    ap.add_argument("--output-dir", default=None)
    args = ap.parse_args()
    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    s, d = cfg["sft"], cfg["dpo"]
    out = resolve(args.output_dir or d["output_dir"])
    tcfg = OptionDPOConfig(
        model_name=cfg["model"]["name"], load_in_4bit=cfg["model"]["load_in_4bit"],
        lora_r=s["lora_r"], lora_alpha=s["lora_alpha"], lora_dropout=s["lora_dropout"],
        lora_target_modules=s["lora_target_modules"], max_seq_len=s["max_seq_len"],
        output_dir=str(out), results_dir=str(out),
        num_epochs=d["num_epochs"], max_steps=args.max_steps or d.get("max_steps"),
        batch_size=d["batch_size"], gradient_accumulation_steps=d["gradient_accumulation_steps"],
        learning_rate=d["learning_rate"], head_learning_rate=d["head_learning_rate"],
        warmup_ratio=d["warmup_ratio"], max_grad_norm=1.0, bf16=True, seed=0,
        eval_every=d["eval_every"], eval_subset=d["state_eval_subset"], save_every=d["eval_every"],
        log_every=5, wandb_project="jeff", wandb_run=d["wandb_run"],
        init_adapter=str(resolve(d["init_adapter"])), beta=d["beta"], nll_weight=d["nll_weight"],
    )
    OptionDPOTrainer(tcfg).train_dpo(
        load_pairs(resolve(d["train"])), load_pairs(resolve(d["eval"])),
        load_dataset_from_jsonl(resolve(s["eval"])))
    print(f"Done. Checkpoints in {out}")


if __name__ == "__main__":
    main()
