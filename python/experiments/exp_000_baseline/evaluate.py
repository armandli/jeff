"""Experiment 000 — evaluate a model on MMLU val/test as a constrained decision task.

Reports accuracy, ECE, and the distribution of predicted indices (a position-bias
check: a well-behaved model's predictions should roughly track the label spread).

    # zero-shot baseline (before fine-tuning)
    python/.venv/bin/python python/experiments/exp_000_baseline/evaluate.py \
        --model Qwen/Qwen2.5-3B-Instruct --tag zeroshot
    # after training
    python/.venv/bin/python python/experiments/exp_000_baseline/evaluate.py \
        --model checkpoints/exp_000_baseline --tag sft
"""

import argparse
import json
from collections import Counter
from pathlib import Path

from tqdm import tqdm

from jeff.data.loader import load_dataset_from_jsonl
from jeff.eval.metrics import accuracy, ece
from jeff.models.base import DecisionInput
from jeff.models.qwen_decision import QwenDecisionModel

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True, help="HF id or checkpoint dir")
    parser.add_argument("--split", default="val", choices=["val", "test"])
    parser.add_argument("--limit", type=int, default=None)
    parser.add_argument("--tag", required=True, help="name for the results file")
    args = parser.parse_args()

    model_path = args.model
    if Path(REPO_ROOT / model_path).exists():
        model_path = str(REPO_ROOT / model_path)

    data = load_dataset_from_jsonl(REPO_ROOT / f"data/processed/mmlu_{args.split}.jsonl")
    examples = list(data)[: args.limit]
    model = QwenDecisionModel(model_path)

    preds, labels, scores = [], [], []
    for ex in tqdm(examples, desc=f"{args.tag}/{args.split}"):
        out = model.decide(DecisionInput(context=ex.context, options=ex.options))
        preds.append(out.chosen_index)
        labels.append(ex.correct_index)
        scores.append(out.scores)

    result = {
        "model": args.model,
        "split": args.split,
        "n": len(examples),
        "accuracy": accuracy(preds, labels),
        "ece": ece(scores, labels),
        "pred_dist": dict(sorted(Counter(preds).items())),
        "label_dist": dict(sorted(Counter(labels).items())),
    }
    out_path = EXP_DIR / "results" / f"{args.tag}_{args.split}.json"
    out_path.parent.mkdir(exist_ok=True)
    out_path.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    print(f"-> {out_path}")


if __name__ == "__main__":
    main()
