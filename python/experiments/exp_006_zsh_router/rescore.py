"""Experiment 006 — recompute cascade metrics offline from the per-row scores that
evaluate.py saves (row_scores / row_parses), without the GPU. Thresholds are picked on val
and applied unchanged to test and hand.

    python/.venv/bin/python python/experiments/exp_006_zsh_router/rescore.py [results/*.json]
"""

import glob
import json
import sys
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(EXP_DIR))
sys.path.insert(0, str(EXP_DIR.parents[1]))

import evaluate as E  # noqa: E402
from jeff.data.zsh_router import pick_threshold, pick_threshold_accuracy  # noqa: E402


def rescore(res: dict, data: dict, vocab: set, cap: float) -> dict:
    casc = {s: [p if ok else 0.0 for p, ok in zip(res["row_scores"][s], res["row_parses"][s])]
            for s in E.SPLITS}
    known = {s: [p if E.known_or_no_command(r["text"], vocab) else 0.0
                 for p, r in zip(casc[s], data[s])] for s in E.SPLITS}
    t_low, _ = pick_threshold(data["val"], casc["val"], cap)
    variants = {
        "cascade@lowest": (casc, t_low),
        "cascade@acc": (casc, pick_threshold_accuracy(data["val"], casc["val"], cap)),
        "known@cascade_lowest": (known, t_low),
        "known@acc": (known, pick_threshold_accuracy(data["val"], known["val"], cap)),
    }
    return {name: {"threshold": t, **{s: E.compute_metrics(data[s], sc[s], t) for s in E.SPLITS}}
            for name, (sc, t) in variants.items()}


def main():
    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    files = sys.argv[1:] or sorted(glob.glob(str(EXP_DIR / "results" / "*.json")))
    data = {s: E.load_rows(cfg, s) for s in E.SPLITS}
    vocab = E.load_vocab(cfg)
    cap = cfg["eval"]["max_english_run_rate"]
    table = {}
    print(f"{'result':36} {'variant':21} {'thr':>5}  val acc/eng/c2a     test acc/eng/c2a    hand acc/eng/c2a")
    for f in files:
        res = json.load(open(f))
        if "row_scores" not in res:
            continue
        name = Path(f).stem
        table[name] = rescore(res, data, vocab, cap)
        for v, m in table[name].items():
            cols = "  ".join(f"{m[s]['accuracy']:.3f}/{m[s]['english_run_rate']:.3f}/"
                             f"{m[s]['command_to_agent_rate']:.3f}" for s in E.SPLITS)
            print(f"{name:36} {v:21} {m['threshold']:5.3f}  {cols}")
    (EXP_DIR / "results" / "rescore.json").write_text(json.dumps(table, indent=1) + "\n")


if __name__ == "__main__":
    main()
