"""Split $HOME/data/sft/mmlu.jsonl into train / val / test files for exp_000.

The fetch script concatenates MMLU's auxiliary_train, validation and test splits
into one file, tagged by the "source" field. This keeps them separate so that
validation/test never leak into training:

    data/processed/mmlu_train.jsonl  <- mmlu/auxiliary_train  (~100k)
    data/processed/mmlu_val.jsonl    <- mmlu/validation       (1,531)
    data/processed/mmlu_test.jsonl   <- mmlu/test             (14,042, held out)

Run from repo root:
    python/.venv/bin/python scripts/prepare_mmlu.py
"""

import argparse
import json
import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPLITS = {
    "mmlu/auxiliary_train": "mmlu_train.jsonl",
    "mmlu/validation": "mmlu_val.jsonl",
    "mmlu/test": "mmlu_test.jsonl",
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", default=Path(os.environ["HOME"]) / "data" / "sft" / "mmlu.jsonl",
                        type=Path)
    parser.add_argument("--out-dir", default=REPO_ROOT / "data" / "processed", type=Path)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    handles = {src: open(args.out_dir / name, "w") for src, name in SPLITS.items()}
    counts = dict.fromkeys(SPLITS, 0)
    with open(args.src) as f:
        for line in f:
            rec = json.loads(line)
            src = rec.get("source")
            if src in handles:
                handles[src].write(line)
                counts[src] += 1
    for h in handles.values():
        h.close()
    for src, name in SPLITS.items():
        print(f"{counts[src]:7d}  {src:22s} -> {args.out_dir / name}")


if __name__ == "__main__":
    main()
