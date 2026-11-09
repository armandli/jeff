"""Experiment 006 — live training monitor.

Watches checkpoints/exp_006_zsh_router/<model_key>/ for each configured model and
prints a live table of accuracy + latency whenever a checkpoint changes on disk.

Run this alongside one or more train.py processes:

    python/.venv/bin/python python/experiments/exp_006_zsh_router/monitor.py
    python/.venv/bin/python python/experiments/exp_006_zsh_router/monitor.py --poll 30
    python/.venv/bin/python python/experiments/exp_006_zsh_router/monitor.py \
        --model-keys modernbert_base electra_base

The monitor reads eval_history.json (written by EncoderTrainer after each eval step)
and decision_head.pt mtime to detect new checkpoints. For each newly arrived checkpoint
it also runs a quick latency benchmark on a small val subset.
"""

import argparse
import json
import sys
import time
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))


def load_val_subset(cfg: dict, n: int = 150) -> list[dict]:
    d = cfg["data"]
    path = REPO_ROOT / d["out_dir"] / f"{d['prefix']}_val.jsonl"
    rows = [json.loads(line) for line in open(path)]
    return rows[:n]


def quick_latency(model_key: str, adapter_dir: Path, cfg: dict, rows: list[dict]) -> dict | None:
    """Load checkpoint and measure p50 latency. Returns None on failure."""
    try:
        import torch
        from jeff.models.base import DecisionInput
        from jeff.models.modernbert_decision import ModernBERTConfig, ModernBERTDecisionModel
        m = cfg["models"][model_key]
        mcfg = ModernBERTConfig(model_name=m["name"],
                                lora_target_modules=m["lora_target_modules"],
                                max_seq_len=m["max_seq_len"])
        model = ModernBERTDecisionModel.from_pretrained(str(adapter_dir), cfg=mcfg)
        model.backbone.eval()

        # Warmup
        for r in rows[:5]:
            model.score_options(DecisionInput(r["context"], r["options"]))

        times = []
        for r in rows:
            t0 = time.perf_counter()
            model.score_options(DecisionInput(r["context"], r["options"]))
            if hasattr(torch, "cuda") and torch.cuda.is_available():
                torch.cuda.synchronize()
            times.append((time.perf_counter() - t0) * 1000)
        times = sorted(times[5:])
        return {
            "p50_ms": round(sorted(times)[len(times) // 2], 2),
            "p95_ms": round(times[int(0.95 * len(times))], 2),
        }
    except Exception as e:
        return {"error": str(e)}


def read_latest_eval(ckpt_dir: Path) -> dict | None:
    """Read the most recent entry from eval_history.json if present."""
    p = ckpt_dir / "eval_history.json"
    if not p.exists():
        return None
    try:
        history = json.loads(p.read_text())
        step_entries = [h for h in history if h.get("tag", "").startswith("step")]
        return step_entries[-1] if step_entries else (history[-1] if history else None)
    except (json.JSONDecodeError, IndexError):
        return None


def checkpoint_mtime(ckpt_dir: Path) -> float:
    """Return mtime of the head weights file as a proxy for checkpoint freshness."""
    p = ckpt_dir / "latest" / "decision_head.pt"
    return p.stat().st_mtime if p.exists() else 0.0


def print_header():
    print(f"\n{'model':22} {'step':>6} {'acc':>7} {'p50_ms':>8} {'p95_ms':>8}  status")
    print("-" * 70)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--poll", type=int, default=60, help="poll interval in seconds (default: 60)")
    ap.add_argument("--model-keys", nargs="*", default=None,
                    help="subset of models to watch (default: all in config)")
    ap.add_argument("--latency-n", type=int, default=100,
                    help="val rows used for quick latency benchmark (default: 100)")
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    model_keys = args.model_keys or list(cfg["models"].keys())
    out_base = REPO_ROOT / cfg["output"]["dir"]

    print(f"Monitoring {model_keys}  |  poll every {args.poll}s  |  Ctrl-C to stop")
    rows = load_val_subset(cfg, args.latency_n)
    print(f"Loaded {len(rows)} val rows for latency benchmark", flush=True)

    # Track last-seen mtime per model so we only re-benchmark on change.
    last_mtime: dict[str, float] = {k: 0.0 for k in model_keys}
    last_lat: dict[str, dict] = {}

    while True:
        print_header()
        for model_key in model_keys:
            ckpt_dir = out_base / model_key
            latest_dir = ckpt_dir / "latest"
            best_eval = read_latest_eval(ckpt_dir)
            mtime = checkpoint_mtime(ckpt_dir)

            if mtime == 0.0:
                print(f"{model_key:22} {'—':>6} {'—':>7} {'—':>8} {'—':>8}  waiting for first checkpoint")
                continue

            # Only re-run latency benchmark if checkpoint changed.
            if mtime != last_mtime[model_key] and latest_dir.exists():
                lat = quick_latency(model_key, latest_dir, cfg, rows)
                last_lat[model_key] = lat
                last_mtime[model_key] = mtime
            else:
                lat = last_lat.get(model_key, {})

            acc_str = f"{best_eval['accuracy']:.3f}" if best_eval else "—"
            step_str = str(best_eval.get("step", "?")) if best_eval else "—"
            p50_str = f"{lat.get('p50_ms', '—')}" if lat else "—"
            p95_str = f"{lat.get('p95_ms', '—')}" if lat else "—"
            status = "error: " + lat["error"] if (lat and "error" in lat) else "ok"
            print(f"{model_key:22} {step_str:>6} {acc_str:>7} {p50_str:>8} {p95_str:>8}  {status}")

        # Also print final results if training is done
        for model_key in model_keys:
            final_json = out_base / model_key / "final" / "eval.json"
            if final_json.exists():
                try:
                    d = json.loads(final_json.read_text())
                    spd = d.get("speed", {})
                    print(f"  [{model_key}] FINAL: acc={d.get('accuracy','?'):.3f}  "
                          f"p50={spd.get('sequential_ms_p50','?')}ms  "
                          f"dec/s={spd.get('sequential_decisions_per_sec','?')}")
                except Exception:
                    pass

        print(f"\n[{time.strftime('%H:%M:%S')}] next poll in {args.poll}s …", flush=True)
        time.sleep(args.poll)


if __name__ == "__main__":
    main()
