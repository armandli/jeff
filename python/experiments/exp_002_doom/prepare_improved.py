"""Experiment 002 step 3b — SFT labels from one step of Monte-Carlo policy improvement.

For each state in label_q.py output, the label is the action whose paired return
beats the teacher's by the build_prefs.py rule (largest such gap), else the
teacher's action. Imitating this is imitating the MC lookahead policy measured
by mc_policy_eval.py (+0.86 +- 0.66 kills/ep over the teacher), and, unlike DPO
on the pairs alone, it anchors the ~96% of states without evidence to the teacher.

    python/.venv/bin/python python/experiments/exp_002_doom/prepare_improved.py
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
    ap.add_argument("--prefix", default="doom_improved")
    ap.add_argument("--eval-frac", type=float, default=0.1)
    ap.add_argument("--out-dir", default=str(REPO_ROOT / "data/processed"))
    args = ap.parse_args()

    rows = [json.loads(l) for l in open(Path(args.inp).expanduser())]
    seeds = sorted({r["seed"] for r in rows})
    eval_seeds = set(seeds[int(len(seeds) * (1 - args.eval_frac)):])
    splits = {"train": [], "val": []}
    changed = collections.Counter()
    for r in rows:
        t, R = r["teacher_index"], r["q_returns"]
        label, best = t, 0.0
        for i in range(3):
            if i == t:
                continue
            d = [x - y for x, y in zip(R[i], R[t])]
            m, se = st.mean(d), max(st.stdev(d) / len(d) ** 0.5, args.se_floor)
            if m >= args.min_gap and m > args.z * se and m > best:
                label, best = i, m
        if label != t:
            changed[(t, label)] += 1
        splits["val" if r["seed"] in eval_seeds else "train"].append(
            {"context": r["context"], "options": r["options"], "correct_index": label})
    for k, v in splits.items():
        with open(Path(args.out_dir) / f"{args.prefix}_{k}.jsonl", "w") as f:
            for x in v:
                f.write(json.dumps(x) + "\n")
        print(f"{k}: {len(v)}")
    n = sum(changed.values())
    print(f"relabelled {n}/{len(rows)} ({n / len(rows):.3f}); (teacher, new): {dict(changed)}")


if __name__ == "__main__":
    main()
