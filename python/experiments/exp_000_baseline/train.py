"""Experiment 000 — SFT baseline (LoRA on Qwen2.5-3B-Instruct, MMLU auxiliary_train).

Prereqs (from repo root):
    python/.venv/bin/python scripts/fetch_training_data.py --datasets mmlu
    python/.venv/bin/python scripts/prepare_mmlu.py

Run from anywhere:
    python/.venv/bin/python python/experiments/exp_000_baseline/train.py [--max-steps 20]
"""

import argparse
from pathlib import Path

import yaml
from transformers import AutoTokenizer

from jeff.data.loader import load_dataset_from_jsonl
from jeff.data.preprocessing import format_sft_example
from jeff.training.trainer import JeffTrainer, JeffTrainingConfig

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent


def load_config(path: Path) -> dict:
    """Load a YAML config, deep-merging it over the file named by its `_base` key."""
    with open(path) as f:
        cfg = yaml.safe_load(f)
    base_rel = cfg.pop("_base", None)
    if base_rel is None:
        return cfg
    base = load_config((path.parent / base_rel).resolve())

    def merge(a: dict, b: dict) -> dict:
        out = dict(a)
        for k, v in b.items():
            out[k] = merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
        return out

    return merge(base, cfg)


def build_split(path: Path, tokenizer, max_length: int) -> list[dict]:
    rows = [format_sft_example(ex) for ex in load_dataset_from_jsonl(path)]
    # Drop examples whose prompt would be truncated: right-truncation would cut
    # off the answer token and leave nothing to learn from.
    keep = [r for r in rows if len(tokenizer(r["prompt"])["input_ids"]) < max_length - 4]
    print(f"{path.name}: {len(keep)}/{len(rows)} examples fit in {max_length} tokens")
    return keep


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--max-steps", type=int, default=-1, help="smoke-test override")
    parser.add_argument("--output-dir", default=None)
    args = parser.parse_args()

    cfg = load_config(EXP_DIR / "config.yaml")
    t, m = cfg["training"], cfg["model"]
    lora = cfg.get("lora", {})
    max_length = t.get("max_length", 2048)

    tokenizer = AutoTokenizer.from_pretrained(m["name"])
    train_ds = build_split(REPO_ROOT / cfg["data"]["train"], tokenizer, max_length)
    eval_ds = build_split(REPO_ROOT / cfg["data"]["eval"], tokenizer, max_length)

    tcfg = JeffTrainingConfig(
        model_name=m["name"],
        output_dir=args.output_dir or str(REPO_ROOT / cfg["output"]["dir"]),
        num_train_epochs=t["num_train_epochs"],
        max_steps=args.max_steps,
        per_device_train_batch_size=t["per_device_train_batch_size"],
        gradient_accumulation_steps=t["gradient_accumulation_steps"],
        learning_rate=float(t["learning_rate"]),
        warmup_ratio=t["warmup_ratio"],
        bf16=t["bf16"],
        max_length=max_length,
        logging_steps=t["logging_steps"],
        save_steps=t["save_steps"],
        eval_steps=t["eval_steps"],
        lora_r=lora.get("r", 64),
        lora_alpha=lora.get("alpha", 128),
        lora_target_modules=lora.get("target_modules", ["q_proj", "v_proj"]),
        report_to=t.get("report_to", "none"),
    )

    JeffTrainer(tcfg).train(train_ds, eval_ds)
    print(f"Saved to {tcfg.output_dir}")


if __name__ == "__main__":
    main()
