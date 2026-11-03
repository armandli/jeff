"""Download and prepare training data from HuggingFace datasets.

Usage:
    uv run scripts/download_data.py --benchmark mmlu --split train --out data/processed/mmlu_train.jsonl
"""

import argparse
import json
from pathlib import Path


def download_mmlu(split: str, out: Path) -> None:
    from datasets import load_dataset
    ds = load_dataset("cais/mmlu", "all", split=split, trust_remote_code=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        for row in ds:
            record = {
                "context": row["question"],
                "options": row["choices"],
                "correct_index": row["answer"],
            }
            f.write(json.dumps(record) + "\n")
    print(f"Wrote {len(ds)} examples to {out}")


def download_gpqa(split: str, out: Path) -> None:
    from datasets import load_dataset
    ds = load_dataset("Idavidrein/gpqa", "gpqa_main", split=split, trust_remote_code=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        for row in ds:
            options = [
                row["Correct Answer"],
                row["Incorrect Answer 1"],
                row["Incorrect Answer 2"],
                row["Incorrect Answer 3"],
            ]
            record = {
                "context": row["Question"],
                "options": options,
                "correct_index": 0,  # correct answer is always first before shuffle
            }
            f.write(json.dumps(record) + "\n")
    print(f"Wrote {len(ds)} examples to {out}")


BENCHMARKS = {
    "mmlu": download_mmlu,
    "gpqa": download_gpqa,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", choices=list(BENCHMARKS), required=True)
    parser.add_argument("--split", default="train")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    BENCHMARKS[args.benchmark](args.split, Path(args.out))


if __name__ == "__main__":
    main()
