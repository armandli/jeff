"""Experiment 005 — score routers on val, generated test and the hand-labelled test.

Deployed system = cascade: if `bash -n` fails go straight to AGENT, otherwise RUN when
the model's P(RUN) >= threshold. The threshold is chosen on val as the lowest value
with english_run_rate <= eval.max_english_run_rate that also keeps every must-pass AGENT
row of critical_val below it, then used unchanged on all test sets.

The must-pass gate (critical_val, critical_test, critical_hand) is reported for the model
alone and for the model behind the rule guard is_critical_command, each with a threshold
picked on val. A gate passes only at 100%; every failure is listed. critical_hand is a
final check: never pick a threshold, checkpoint or template from it.

    python/.venv/bin/python python/experiments/exp_005_shell_router/evaluate.py --baselines-only
    python/.venv/bin/python python/experiments/exp_005_shell_router/evaluate.py --tag zeroshot
    python/.venv/bin/python python/experiments/exp_005_shell_router/evaluate.py \
        --adapter checkpoints/exp_005_shell_router/best --tag best
"""

import argparse
import collections
import json
import random
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.shell_router import (  # noqa: E402
    CRITICAL_AGENT_CATEGORIES,
    ENGLISH_CATEGORIES,
    RUN,
    bash_parses,
    first_word,
    is_critical_command,
)

SPLITS = ("val", "test", "hand")
GATES = ("critical_val", "critical_test", "critical_hand")


def load_rows(cfg: dict, split: str) -> list[dict]:
    d = cfg["data"]
    path = REPO_ROOT / d["out_dir"] / f"{d['prefix']}_{split}.jsonl"
    return [json.loads(line) for line in open(path)]


def compute_metrics(rows: list[dict], p_run: list[float], threshold: float) -> dict:
    run = [p >= threshold for p in p_run]
    valid = [r["correct_index"] == RUN for r in rows]
    english = [r["category"] in ENGLISH_CATEGORIES for r in rows]
    other = [not v and not e for v, e in zip(valid, english)]

    def rate(mask, hit):
        n = sum(mask)
        return sum(h for m, h in zip(mask, hit) if m) / n if n else None

    per_category = collections.defaultdict(lambda: [0, 0])
    for r, is_run, is_valid in zip(rows, run, valid):
        per_category[r["category"]][0] += is_run == is_valid
        per_category[r["category"]][1] += 1
    rare = [not r["tool_seen"] for r in rows]
    return {
        "n": len(rows),
        "threshold": threshold,
        "accuracy": rate([True] * len(rows), [a == b for a, b in zip(run, valid)]),
        "english_run_rate": rate(english, run),                # prose executed by bash
        "other_invalid_run_rate": rate(other, run),            # usually a harmless shell error
        "command_to_agent_rate": rate(valid, [not x for x in run]),   # slower, not unsafe
        "heldout_tool_accuracy": rate(rare, [a == b for a, b in zip(run, valid)]),
        "per_category": {k: {"accuracy": c / n, "n": n} for k, (c, n) in sorted(per_category.items())},
    }


def gate_metrics(rows: list[dict], p_run: list[float], threshold: float) -> dict:
    """All-or-nothing check. Scores near the threshold are listed as well as failures."""
    failures = [{"text": r["text"], "category": r["category"], "source": r["source"], "p_run": round(p, 6)}
                for r, p in zip(rows, p_run) if (p >= threshold) != (r["correct_index"] == RUN)]
    per_category = collections.Counter(r["category"] for r in rows)
    failed = collections.Counter(f["category"] for f in failures)
    return {
        "n": len(rows), "threshold": threshold, "failures_n": len(failures), "passed": not failures,
        "per_category": {c: {"n": n, "failed": failed[c]} for c, n in sorted(per_category.items())},
        "failures": sorted(failures, key=lambda f: f["category"]),
    }


def pick_gated_threshold(val_rows, val_p, gate_rows, gate_p, cap: float) -> dict:
    """Lowest threshold meeting the english cap on val with no critical AGENT row at or above it."""
    english_thr, cap_met = pick_threshold(val_rows, val_p, cap)
    must_agent = [p for r, p in zip(gate_rows, gate_p) if r["category"] in CRITICAL_AGENT_CATEGORIES]
    must_run = [p for r, p in zip(gate_rows, gate_p) if r["category"] not in CRITICAL_AGENT_CATEGORIES]
    threshold = max(english_thr, max(must_agent) + 1e-9)
    return {"threshold": threshold, "english_threshold": english_thr, "cap_met_on_val": cap_met,
            "gate_max_p_must_agent": max(must_agent), "gate_min_p_must_run": min(must_run),
            "gate_separable_on_val": max(must_agent) < min(must_run)}


def pick_threshold(rows: list[dict], p_run: list[float], cap: float) -> tuple[float, bool]:
    """Lowest threshold meeting the english_run_rate cap on these rows."""
    english = sorted(p for r, p in zip(rows, p_run) if r["category"] in ENGLISH_CATEGORIES)
    allowed = int(cap * len(english))         # english rows allowed at or above the threshold
    if allowed >= len(english):
        return 0.0, True
    threshold = english[-allowed - 1] + 1e-9
    return threshold, threshold <= 1.0


def rule_baselines(cfg: dict) -> dict:
    d = cfg["data"]
    vocab = set(json.load(open(REPO_ROOT / d["out_dir"] / f"{d['prefix']}_vocab.json")))
    results = {}
    for split in (*SPLITS, *GATES):
        rows = load_rows(cfg, split)
        with ThreadPoolExecutor(8) as ex:
            parses = list(ex.map(bash_parses, (r["text"] for r in rows)))

        def known(text):
            word = first_word(text)
            return word in vocab or word.startswith(("/", "./", "../", "~/", "."))

        rules = {
            "always_run": [1.0] * len(rows),
            "bash_n": [float(p) for p in parses],
            "bash_n_known_command": [float(p and known(r["text"])) for p, r in zip(parses, rows)],
            "bash_n_known_command_guard": [float(p and known(r["text"]) and not is_critical_command(r["text"]))
                                           for p, r in zip(parses, rows)],
        }
        score = gate_metrics if split in GATES else compute_metrics
        results[split] = {name: score(rows, p_run, 0.5) for name, p_run in rules.items()}
    return results


def load_model(adapter: str | None):
    from jeff.models.qwen3_decision import Qwen3Config, Qwen3DecisionModel
    if adapter:
        return Qwen3DecisionModel.from_pretrained(str(REPO_ROOT / adapter))
    model = Qwen3DecisionModel(Qwen3Config())
    model._load_base(model.cfg.model_name)
    model.backbone.eval()
    return model


def score_batched(model, rows: list[dict], batch_size: int) -> list[float]:
    """P(RUN) per row: same scorer as score_options_both, batched for throughput."""
    import torch

    from jeff.models.qwen3_choice import cyclic_permutations
    from jeff.models.qwen3_decision import encode_example

    perms = cyclic_permutations(2)
    items = []
    for i, r in enumerate(rows):
        for perm in perms:
            it = encode_example(model.tokenizer, r["context"], r["options"], perm)
            items.append({**it, "row": i, "perm": perm})
    items.sort(key=lambda it: len(it["input_ids"]))     # similar lengths pad less

    device = model.head.proj.weight.device
    acc = torch.zeros(len(rows), 2)
    with torch.no_grad():
        for start in range(0, len(items), batch_size):
            chunk = items[start:start + batch_size]
            batch = model.collate(chunk)
            scores, _ = model.forward_for_training(**{k: v.to(device) for k, v in batch.items()})
            for it, row in zip(chunk, scores.float().cpu()):
                for pos, orig in enumerate(it["perm"]):
                    acc[it["row"], orig] += row[pos]
    probs = torch.softmax(acc / len(perms), dim=-1)
    return probs[:, RUN].tolist()


def latency_ms(model, rows: list[dict], n: int = 200) -> dict:
    """Wall time of a single-line decision, as a shell would call it."""
    import torch

    from jeff.models.base import DecisionInput
    times = []
    for r in rows[:n]:
        t0 = time.perf_counter()
        model.score_options_both(DecisionInput(r["context"], r["options"]))
        torch.cuda.synchronize()
        times.append(1000 * (time.perf_counter() - t0))
    times = sorted(times[5:])                           # skip warm-up
    return {"p50": statistics.median(times), "p95": times[int(0.95 * len(times))]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter", default=None, help="repo-relative checkpoint; omit for zero-shot")
    ap.add_argument("--tag", default="zeroshot")
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--baselines-only", action="store_true")
    ap.add_argument("--limit", type=int, default=None, help="rows per split, for smoke tests")
    ap.add_argument("--results-dir", default=None, help="override, for smoke tests")
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    results_dir = Path(args.results_dir) if args.results_dir else REPO_ROOT / cfg["output"]["results_dir"]
    results_dir.mkdir(parents=True, exist_ok=True)

    if args.baselines_only:
        res = rule_baselines(cfg)
        (results_dir / "baselines.json").write_text(json.dumps(res, indent=2) + "\n")
        for split, by_rule in res.items():
            for name, m in by_rule.items():
                if split in GATES:
                    print(f"{split:13} {name:26} failures {m['failures_n']}/{m['n']}  {m['per_category']}")
                    continue
                print(f"{split:5} {name:26} acc {m['accuracy']:.3f}  english_run {m['english_run_rate']:.3f}  "
                      f"other_invalid_run {m['other_invalid_run_rate']:.3f}  "
                      f"cmd_to_agent {m['command_to_agent_rate']:.3f}")
        return

    model = load_model(args.adapter)
    data = {s: load_rows(cfg, s) for s in (*SPLITS, *GATES)}
    if args.limit:      # the gate files are sorted, so a prefix would miss whole classes
        data = {s: random.Random(0).sample(rows, min(args.limit, len(rows))) for s, rows in data.items()}
    scores = {}
    for split in (*SPLITS, *GATES):
        t0 = time.time()
        scores[split] = score_batched(model, data[split], args.batch_size)
        print(f"scored {split}: {len(data[split])} rows in {time.time() - t0:.0f}s", flush=True)
    parses = {s: [bash_parses(r["text"]) for r in data[s]] for s in scores}
    cascade = {s: [p if ok else 0.0 for p, ok in zip(scores[s], parses[s])] for s in scores}
    guarded = {s: [0.0 if is_critical_command(r["text"]) else p for r, p in zip(data[s], cascade[s])]
               for s in scores}

    cap = cfg["eval"]["max_english_run_rate"]
    pick = pick_gated_threshold(data["val"], cascade["val"], data["critical_val"], cascade["critical_val"], cap)
    pick_g = pick_gated_threshold(data["val"], guarded["val"], data["critical_val"], guarded["critical_val"], cap)
    threshold, threshold_g = pick["threshold"], pick_g["threshold"]
    res = {"tag": args.tag, "adapter": args.adapter, "threshold": threshold,
           "threshold_cap": cap, "threshold_pick": pick, "guarded_threshold_pick": pick_g,
           "latency_ms": latency_ms(model, data["val"]), "splits": {}, "gates": {}}
    for split in SPLITS:
        res["splits"][split] = {
            "model_at_0.5": compute_metrics(data[split], scores[split], 0.5),
            "cascade_at_threshold": compute_metrics(data[split], cascade[split], threshold),
            "guarded_at_threshold": compute_metrics(data[split], guarded[split], threshold_g),
            "cascade_at_0.5": compute_metrics(data[split], cascade[split], 0.5),
            "guarded_at_0.5": compute_metrics(data[split], guarded[split], 0.5),
        }
        m = res["splits"][split]["cascade_at_threshold"]
        print(f"{split:5} cascade acc {m['accuracy']:.3f}  english_run {m['english_run_rate']:.3f}  "
              f"other_invalid_run {m['other_invalid_run_rate']:.3f}  "
              f"cmd_to_agent {m['command_to_agent_rate']:.3f}", flush=True)

    for gate in GATES:
        res["gates"][gate] = {
            "model_at_0.5": gate_metrics(data[gate], scores[gate], 0.5),
            "cascade_at_threshold": gate_metrics(data[gate], cascade[gate], threshold),
            "guarded_at_threshold": gate_metrics(data[gate], guarded[gate], threshold_g),
            "cascade_at_0.5": gate_metrics(data[gate], cascade[gate], 0.5),
            "guarded_at_0.5": gate_metrics(data[gate], guarded[gate], 0.5),
        }
        for name in ("cascade_at_threshold", "guarded_at_threshold", "guarded_at_0.5"):
            m = res["gates"][gate][name]
            print(f"{gate:13} {name:21} {'PASS' if m['passed'] else 'FAIL'} "
                  f"failures {m['failures_n']}/{m['n']}  {m['per_category']}", flush=True)
    for name in ("guarded_at_threshold", "guarded_at_0.5"):
        m = res["splits"]["test"][name]
        print(f"test  {name:21} acc {m['accuracy']:.3f}  english_run {m['english_run_rate']:.3f}  "
              f"other_invalid_run {m['other_invalid_run_rate']:.3f}  "
              f"cmd_to_agent {m['command_to_agent_rate']:.3f}", flush=True)

    # Every hand-test mistake, since that set is small enough to read.
    res["hand_errors"] = [
        {"text": r["text"], "category": r["category"], "p_run": round(p, 4)}
        for r, p in zip(data["hand"], cascade["hand"])
        if (p >= threshold) != (r["correct_index"] == RUN)
    ]
    (results_dir / f"{args.tag}.json").write_text(json.dumps(res, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({k: res[k] for k in ("threshold_pick", "guarded_threshold_pick", "latency_ms")}))


if __name__ == "__main__":
    main()
