"""Experiment 004 — convert exp_002 DecisionExample JSONL to GLiNER2 classification records.

    python/.venv/bin/python python/experiments/exp_004_gliner2_doom/prepare_data.py

Each record: {"input": context, "output": {"classifications": [
    {"task": "action", "labels": [ATTACK, TURN_LEFT, TURN_RIGHT], "true_label": [<teacher>]}]}}
"""

import collections
import json
import sys
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.models.gliner2_decision import TASK, option_label  # noqa: E402


def convert(src: Path, dst: Path) -> None:
    n, counts = 0, collections.Counter()
    with open(src) as fi, open(dst, "w") as fo:
        for line in fi:
            d = json.loads(line)
            labels = [option_label(o) for o in d["options"]]
            gold = labels[d["correct_index"]]
            counts[gold] += 1
            fo.write(json.dumps({"input": d["context"], "output": {"classifications": [
                {"task": TASK, "labels": labels, "true_label": [gold]}]}}) + "\n")
            n += 1
    print(f"{dst.name}: {n}  labels {dict(counts)}")


def main():
    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))["data"]
    for s, d in (("src_train", "train"), ("src_val", "val")):
        dst = REPO_ROOT / cfg[d]
        dst.parent.mkdir(parents=True, exist_ok=True)
        convert(REPO_ROOT / cfg[s], dst)


if __name__ == "__main__":
    main()
