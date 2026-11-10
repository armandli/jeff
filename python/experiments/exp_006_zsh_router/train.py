"""Experiment 006 — train a fast encoder router on zsh RUN/AGENT data.

Supports three model candidates; select with --model-key (default: modernbert_base).
Each model is saved under checkpoints/exp_006_zsh_router/<model_key>/.

    # Train the fastest model (default):
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py

    # Train each candidate. Run them one at a time on a single GPU: parallel runs share
    # the GPU and distort the latency numbers measured during training.
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py --model-key modernbert_base
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py --model-key modernbert_large
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py --model-key roberta_base

    # Smoke test (2 steps, temp output):
    python/.venv/bin/python python/experiments/exp_006_zsh_router/train.py --max-steps 2 --output-dir /tmp/x
"""

import argparse
import json
import sys
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.loader import load_dataset_from_jsonl  # noqa: E402
from jeff.data.zsh_router import ENGLISH_CATEGORIES, RUN, pick_threshold, zsh_parses  # noqa: E402
from jeff.training.encoder_trainer import EncoderTrainer, EncoderTrainingConfig  # noqa: E402


def make_rank_fn(val_path: Path, cap: float):
    """Rank checkpoints the way the router is deployed: `zsh -n` failures go to AGENT,
    the RUN threshold is the lowest one keeping english_run_rate <= cap on the eval
    subset, and the score is accuracy at that threshold. Plain argmax accuracy rewards
    checkpoints that run more English, which is the costly error."""
    meta = {}
    for line in open(val_path):
        r = json.loads(line)
        meta[r["context"]] = r
    parse_cache: dict[str, bool] = {}

    def rank_fn(examples, probs) -> dict:
        rows = [meta[ex.context] for ex in examples]
        cascade = []
        for r, p in zip(rows, probs):
            if r["text"] not in parse_cache:
                parse_cache[r["text"]] = zsh_parses(r["text"])
            cascade.append(p[RUN] if parse_cache[r["text"]] else 0.0)
        threshold, _ = pick_threshold(rows, cascade, cap)
        run = [p >= threshold for p in cascade]
        valid = [r["correct_index"] == RUN for r in rows]
        english = [r["category"] in ENGLISH_CATEGORIES for r in rows]
        n_eng = sum(english)
        return {
            "rank_score": sum(a == b for a, b in zip(run, valid)) / len(rows),
            "cascade_threshold": threshold,
            "cascade_english_run_rate":
                sum(x for x, e in zip(run, english) if e) / n_eng if n_eng else 0.0,
            "cascade_command_to_agent_rate":
                sum(not x for x, v in zip(run, valid) if v) / max(1, sum(valid)),
        }

    return rank_fn


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
    rank_fn = make_rank_fn(processed / f"{d['prefix']}_val.jsonl", ev["max_english_run_rate"])
    EncoderTrainer(tcfg, rank_fn=rank_fn).train_sft(train_ds, eval_ds)
    print(f"Done. Checkpoint at {out}/final")


if __name__ == "__main__":
    main()
