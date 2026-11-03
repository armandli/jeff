"""Experiment 001 — QLoRA on Qwen3-8B with MLP decision head.

Run from repo root:
    source python/.venv/bin/activate
    cd python
    python experiments/exp_001_qlora/train.py
"""

import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from jeff.data.loader import load_dataset_from_jsonl
from jeff.training.qlora_trainer import QLoRATrainer, QLoRATrainingConfig


def main():
    cfg_path = Path(__file__).parent / "config.yaml"
    with open(cfg_path) as f:
        cfg = yaml.safe_load(f)

    train_path = Path(cfg["data"]["train"]).expanduser()
    train_ds = load_dataset_from_jsonl(train_path)

    eval_ds = None
    if cfg["data"].get("eval"):
        eval_ds = load_dataset_from_jsonl(Path(cfg["data"]["eval"]).expanduser())

    tcfg = QLoRATrainingConfig(
        model_name=cfg["model"]["name"],
        load_in_4bit=cfg["model"]["load_in_4bit"],
        lora_r=cfg["model"]["lora_r"],
        lora_alpha=cfg["model"]["lora_alpha"],
        lora_dropout=cfg["model"]["lora_dropout"],
        lora_target_modules=cfg["model"]["lora_target_modules"],
        output_dir=cfg["output"]["dir"],
        num_epochs=cfg["training"]["num_epochs"],
        batch_size=cfg["training"]["batch_size"],
        gradient_accumulation_steps=cfg["training"]["gradient_accumulation_steps"],
        learning_rate=cfg["training"]["learning_rate"],
        head_learning_rate=cfg["training"]["head_learning_rate"],
        bf16=cfg["training"]["bf16"],
        wandb_project=cfg["wandb"]["project"],
        wandb_run=cfg["wandb"]["run_name"],
    )

    trainer = QLoRATrainer(tcfg)
    trainer.train_sft(train_ds, eval_ds)
    print(f"Done. Checkpoint at {tcfg.output_dir}/final")


if __name__ == "__main__":
    main()
