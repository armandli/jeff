"""Experiment 002 step 2 — build the SFT set from recorded, teacher-labelled play.

Merges clean-teacher and epsilon-noisy-teacher recordings (noisy runs reach states
the clean teacher never visits, e.g. far off-target), drops exact duplicate
contexts, and samples a fixed-size training set.

    python/.venv/bin/python python/experiments/exp_002_doom/prepare_sft.py
"""

import argparse
import collections
import json
import random
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inputs", nargs="+", default=["~/data/doom/teacher_train.jsonl",
                                                     "~/data/doom/noisy_train.jsonl"])
    ap.add_argument("--val", default="~/data/doom/teacher_val.jsonl")
    ap.add_argument("--n-train", type=int, default=24000)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out-dir", default=str(REPO_ROOT / "data/processed"))
    ap.add_argument("--prefix", default="doom_sft", help="output file prefix")
    args = ap.parse_args()

    seen, rows = set(), []
    for p in args.inputs:
        for line in open(Path(p).expanduser()):
            d = json.loads(line)
            if d["context"] in seen:
                continue
            seen.add(d["context"])
            rows.append({"context": d["context"], "options": d["options"],
                         "correct_index": d["correct_index"]})
    print(f"unique train states: {len(rows)}")
    random.Random(args.seed).shuffle(rows)
    rows = rows[: args.n_train]

    val = []
    for line in open(Path(args.val).expanduser()):
        d = json.loads(line)
        if d["context"] not in seen:          # no train/val overlap
            val.append({"context": d["context"], "options": d["options"],
                        "correct_index": d["correct_index"]})
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for name, data in ((f"{args.prefix}_train.jsonl", rows), (f"{args.prefix}_val.jsonl", val)):
        with open(out / name, "w") as f:
            for r in data:
                f.write(json.dumps(r) + "\n")
        print(f"{name}: {len(data)}  labels {dict(collections.Counter(r['correct_index'] for r in data))}")


if __name__ == "__main__":
    main()
