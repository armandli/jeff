"""Experiment 002 step 3 — turn Monte-Carlo Q labels (label_q.py) into DPO pairs.

A pair (chosen=a, rejected=b) is kept when the paired return difference over the
K common-random-number rollouts is large and clearly non-zero:
    mean(R_a - R_b) >= min_gap  and  mean > z * se
Episodes (seeds) are split, so eval pairs come from unseen games.

    python/.venv/bin/python python/experiments/exp_002_doom/build_prefs.py
"""

import argparse
import collections
import json
import statistics as st
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inp", default="~/data/doom/q_train.jsonl")
    ap.add_argument("--min-gap", type=float, default=0.5)
    ap.add_argument("--z", type=float, default=2.5)
    ap.add_argument("--se-floor", type=float, default=0.1, help="CRN can give se=0")
    ap.add_argument("--prefix", default="doom_prefs")
    ap.add_argument("--eval-frac", type=float, default=0.1)
    ap.add_argument("--out-dir", default=str(REPO_ROOT / "data/processed"))
    args = ap.parse_args()

    rows = [json.loads(l) for l in open(Path(args.inp).expanduser())]
    seeds = sorted({r["seed"] for r in rows})
    eval_seeds = set(seeds[int(len(seeds) * (1 - args.eval_frac)):])
    splits = {"train": [], "eval": []}
    kinds = collections.Counter()
    for r in rows:
        R = r["q_returns"]
        for a in range(3):
            for b in range(3):
                if a == b:
                    continue
                d = [x - y for x, y in zip(R[a], R[b])]
                m, se = st.mean(d), max(st.stdev(d) / len(d) ** 0.5, args.se_floor)
                if m >= args.min_gap and m > args.z * se:
                    split = "eval" if r["seed"] in eval_seeds else "train"
                    splits[split].append({"context": r["context"], "options": r["options"],
                                          "chosen": a, "rejected": b, "gap": m, "se": se,
                                          "teacher_index": r["teacher_index"], "seed": r["seed"]})
                    kinds[("teacher" if a == r["teacher_index"] else
                           "beats_teacher" if b == r["teacher_index"] else "other")] += 1
    out = Path(args.out_dir)
    for k, v in splits.items():
        with open(out / f"{args.prefix}_{k}.jsonl", "w") as f:
            for p in v:
                f.write(json.dumps(p) + "\n")
        print(f"{k}: {len(v)} pairs  chosen {dict(collections.Counter(p['chosen'] for p in v))}")
    print(f"states {len(rows)}, pair kinds {dict(kinds)}")


if __name__ == "__main__":
    main()
