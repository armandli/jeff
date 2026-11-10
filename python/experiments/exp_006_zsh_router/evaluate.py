"""Experiment 006 — score and compare zsh routers on val, generated test, and hand-labelled test.

Deployed system: cascade: if `zsh -n` fails go straight to AGENT, otherwise RUN when
the model's P(RUN) >= threshold. The threshold is chosen on val to cap english_run_rate,
then applied unchanged to both tests.

Single model:
    python/.venv/bin/python python/experiments/exp_006_zsh_router/evaluate.py --baselines-only
    python/.venv/bin/python python/experiments/exp_006_zsh_router/evaluate.py \
        --adapter checkpoints/exp_006_zsh_router/modernbert_base/best \
        --model-key modernbert_base --tag best_base

Compare all trained models side-by-side (accuracy + latency Pareto):
    python/.venv/bin/python python/experiments/exp_006_zsh_router/evaluate.py --compare-all
"""

import argparse
import collections
import json
import statistics
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.zsh_router import (  # noqa: E402
    ENGLISH_CATEGORIES,
    RUN,
    first_word,
    zsh_parses,
)

SPLITS = ("val", "test", "hand")


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
        "english_run_rate": rate(english, run),
        "other_invalid_run_rate": rate(other, run),
        "command_to_agent_rate": rate(valid, [not x for x in run]),
        "heldout_tool_accuracy": rate(rare, [a == b for a, b in zip(run, valid)]),
        "per_category": {k: {"accuracy": c / n, "n": n} for k, (c, n) in sorted(per_category.items())},
    }


def pick_threshold(rows: list[dict], p_run: list[float], cap: float) -> tuple[float, bool]:
    english = sorted(p for r, p in zip(rows, p_run) if r["category"] in ENGLISH_CATEGORIES)
    allowed = int(cap * len(english))
    if allowed >= len(english):
        return 0.0, True
    threshold = english[-allowed - 1] + 1e-9
    return threshold, threshold <= 1.0


def rule_baselines(cfg: dict) -> dict:
    d = cfg["data"]
    vocab = set(json.load(open(REPO_ROOT / d["out_dir"] / f"{d['prefix']}_vocab.json")))
    results = {}
    for split in SPLITS:
        rows = load_rows(cfg, split)
        with ThreadPoolExecutor(8) as ex:
            parses = list(ex.map(zsh_parses, (r["text"] for r in rows)))

        def known(text):
            word = first_word(text)
            return word in vocab or word.startswith(("/", "./", "../", "~/", "."))

        results[split] = {
            "always_run": compute_metrics(rows, [1.0] * len(rows), 0.5),
            "zsh_n": compute_metrics(rows, [float(p) for p in parses], 0.5),
            "zsh_n_known_command": compute_metrics(
                rows, [float(p and known(r["text"])) for p, r in zip(parses, rows)], 0.5),
        }
    return results


def load_model(model_key: str, adapter: str | None, cfg: dict):
    from jeff.models.modernbert_decision import ModernBERTConfig, ModernBERTDecisionModel
    m = cfg["models"][model_key]
    mcfg = ModernBERTConfig(model_name=m["name"], lora_target_modules=m["lora_target_modules"],
                             max_seq_len=m["max_seq_len"])
    if adapter:
        return ModernBERTDecisionModel.from_pretrained(str(REPO_ROOT / adapter), cfg=mcfg)
    model = ModernBERTDecisionModel(mcfg)
    model._load_base(m["name"])
    model.backbone.eval()
    return model


def score_batched(model, rows: list[dict], batch_size: int) -> list[float]:
    """P(RUN) per row via batched cyclic-permutation scoring."""
    import torch

    from jeff.models.modernbert_decision import _cyclic_permutations

    n_opts = len(rows[0]["options"])
    perms = _cyclic_permutations(n_opts)
    items = []
    for i, r in enumerate(rows):
        for perm in perms:
            it = model.encode_example(r["context"], r["options"], perm)
            items.append({**it, "row": i, "perm": perm})
    items.sort(key=lambda it: len(it["input_ids"][0]))

    device = model.head.proj.weight.device
    acc = torch.zeros(len(rows), n_opts)
    with torch.no_grad():
        for start in range(0, len(items), batch_size):
            chunk = items[start:start + batch_size]
            batch = model.collate(chunk)
            scores = model.forward_for_training(
                **{k: v.to(device) for k, v in batch.items()
                   if k not in ("labels", "n_options", "counts")},
                n_options=batch["n_options"],
            )
            for it, row in zip(chunk, scores.float().cpu()):
                for pos, orig in enumerate(it["perm"]):
                    acc[it["row"], orig] += row[pos]
    probs = torch.softmax(acc / len(perms), dim=-1)
    return probs[:, RUN].tolist()


def latency_benchmark(model, rows: list[dict], n: int = 200) -> dict:
    """Wall-time for a single-line decision (sequential, as a shell would call it)."""
    import torch

    from jeff.models.base import DecisionInput
    times = []
    for r in rows[:n]:
        t0 = time.perf_counter()
        model.score_options(DecisionInput(r["context"], r["options"]))
        if hasattr(torch, "cuda") and torch.cuda.is_available():
            torch.cuda.synchronize()
        times.append(1000 * (time.perf_counter() - t0))
    times = sorted(times[5:])
    n_valid = len(times)
    return {
        "p50_ms": round(statistics.median(times), 2),
        "p95_ms": round(times[int(0.95 * n_valid)], 2),
        "decisions_per_sec": round(1000 / statistics.median(times), 1),
        "n": n_valid,
    }


def evaluate_one(model_key: str, adapter: str | None, cfg: dict, args) -> dict:
    model = load_model(model_key, adapter, cfg)
    data = {s: load_rows(cfg, s)[:args.limit] for s in SPLITS}
    scores = {}
    for split in SPLITS:
        t0 = time.time()
        scores[split] = score_batched(model, data[split], args.batch_size)
        print(f"[{model_key}] scored {split}: {len(data[split])} rows in {time.time()-t0:.0f}s",
              flush=True)

    parses = {s: [zsh_parses(r["text"]) for r in data[s]] for s in SPLITS}
    cascade = {s: [p if ok else 0.0 for p, ok in zip(scores[s], parses[s])] for s in SPLITS}

    cap = cfg["eval"]["max_english_run_rate"]
    threshold, cap_met = pick_threshold(data["val"], cascade["val"], cap)
    lat = latency_benchmark(model, data["val"])
    res = {
        "model_key": model_key,
        "adapter": adapter,
        "threshold": threshold,
        "threshold_cap": cap,
        "threshold_cap_met_on_val": cap_met,
        "latency_ms": lat,
        "splits": {},
    }
    for split in SPLITS:
        res["splits"][split] = {
            "model_at_0.5": compute_metrics(data[split], scores[split], 0.5),
            "cascade_at_threshold": compute_metrics(data[split], cascade[split], threshold),
        }
        m = res["splits"][split]["cascade_at_threshold"]
        print(f"[{model_key}] {split:5} cascade acc {m['accuracy']:.3f}  "
              f"english_run {m['english_run_rate']:.3f}  "
              f"other_invalid_run {m['other_invalid_run_rate']:.3f}  "
              f"p50_ms {lat['p50_ms']}", flush=True)
    res["hand_errors"] = [
        {"text": r["text"], "category": r["category"], "p_run": round(p, 4)}
        for r, p in zip(data["hand"], cascade["hand"])
        if (p >= threshold) != (r["correct_index"] == RUN)
    ]
    return res


def compare_all(cfg: dict, args) -> None:
    """Train all three models' best checkpoints and print an accuracy-vs-latency table."""
    results_dir = REPO_ROOT / cfg["output"]["results_dir"]
    all_res = {}
    for model_key in cfg["models"]:
        adapter_path = REPO_ROOT / cfg["output"]["dir"] / model_key / "best"
        if not adapter_path.exists():
            print(f"[{model_key}] no checkpoint at {adapter_path}, skipping", flush=True)
            continue
        res = evaluate_one(model_key, str(adapter_path.relative_to(REPO_ROOT)), cfg, args)
        all_res[model_key] = res
        (results_dir / f"compare_{model_key}.json").write_text(
            json.dumps(res, indent=2, ensure_ascii=False) + "\n")

    if not all_res:
        print("No trained checkpoints found. Run train.py for each model first.")
        return

    # Print Pareto summary
    print("\n=== Accuracy vs Latency (Pareto) ===")
    print(f"{'model':20} {'val_acc':>8} {'test_acc':>9} {'p50_ms':>8} {'p95_ms':>8} {'dec/s':>7}")
    for key, res in sorted(all_res.items(), key=lambda kv: kv[1]["latency_ms"]["p50_ms"]):
        val_acc = res["splits"]["val"]["cascade_at_threshold"]["accuracy"]
        test_acc = res["splits"]["test"]["cascade_at_threshold"]["accuracy"]
        lat = res["latency_ms"]
        print(f"{key:20} {val_acc:8.3f} {test_acc:9.3f} {lat['p50_ms']:8.1f} "
              f"{lat['p95_ms']:8.1f} {lat['decisions_per_sec']:7.0f}")

    (results_dir / "compare_all.json").write_text(json.dumps(all_res, indent=2, ensure_ascii=False) + "\n")
    print(f"\nFull results → {results_dir}/compare_all.json")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter", default=None, help="repo-relative checkpoint path")
    ap.add_argument("--model-key", default=None, help="model key (required unless --compare-all or --baselines-only)")
    ap.add_argument("--tag", default="zeroshot")
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--baselines-only", action="store_true")
    ap.add_argument("--compare-all", action="store_true", help="benchmark all trained model checkpoints")
    ap.add_argument("--limit", type=int, default=None, help="rows per split (smoke tests)")
    ap.add_argument("--results-dir", default=None)
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    results_dir = Path(args.results_dir) if args.results_dir else REPO_ROOT / cfg["output"]["results_dir"]
    results_dir.mkdir(parents=True, exist_ok=True)

    if args.baselines_only:
        res = rule_baselines(cfg)
        (results_dir / "baselines.json").write_text(json.dumps(res, indent=2) + "\n")
        for split, by_rule in res.items():
            for name, m in by_rule.items():
                print(f"{split:5} {name:25} acc {m['accuracy']:.3f}  "
                      f"english_run {m['english_run_rate']:.3f}  "
                      f"other_invalid_run {m['other_invalid_run_rate']:.3f}  "
                      f"cmd_to_agent {m['command_to_agent_rate']:.3f}")
        return

    if args.compare_all:
        compare_all(cfg, args)
        return

    model_key = args.model_key or next(iter(cfg["models"]))
    res = evaluate_one(model_key, args.adapter, cfg, args)
    (results_dir / f"{args.tag}.json").write_text(
        json.dumps(res, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({k: res[k] for k in ("threshold", "threshold_cap_met_on_val", "latency_ms")}))


if __name__ == "__main__":
    main()
