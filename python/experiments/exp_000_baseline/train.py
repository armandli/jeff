"""Experiment 000 — SFT baseline.

Run from repo root:
    cd python
    uv run experiments/exp_000_baseline/train.py
"""

import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).parent.parent.parent / "python"))

from jeff.data.loader import load_dataset_from_jsonl
from jeff.data.preprocessing import format_sft_example
from jeff.training.trainer import JeffTrainer, JeffTrainingConfig


def main():
    cfg_path = Path(__file__).parent / "config.yaml"
    with open(cfg_path) as f:
        cfg = yaml.safe_load(f)

    train_raw = load_dataset_from_jsonl(cfg["data"]["train"])
    eval_raw = load_dataset_from_jsonl(cfg["data"]["eval"])

    train_ds = [format_sft_example(ex) for ex in train_raw]
    eval_ds = [format_sft_example(ex) for ex in eval_raw]

    tcfg = JeffTrainingConfig(
        model_name=cfg["model"]["name"],
        output_dir=cfg["output"]["dir"],
        num_train_epochs=cfg["training"]["num_train_epochs"],
        per_device_train_batch_size=cfg["training"]["per_device_train_batch_size"],
    )

    trainer = JeffTrainer(tcfg)
    trainer.train(train_ds, eval_ds)
    print(f"Saved to {tcfg.output_dir}")


if __name__ == "__main__":
    main()
