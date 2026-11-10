"""Experiment 006 — single-decision latency for trained routers, measured on an idle machine.

Latencies logged during training and evaluate.py swing by 30-50% because other jobs share the
GPU. This script measures every checkpoint back to back with nothing else running, in three
deployment variants:
  gpu        bf16 on GPU, LoRA adapter unmerged (what evaluate.py measures)
  gpu_merged bf16 on GPU, LoRA merged into the base weights (no adapter matmuls)
  cpu_merged fp32 on CPU, LoRA merged, torch threads = --cpu-threads

    python/.venv/bin/python python/experiments/exp_006_zsh_router/bench_latency.py \\
        modernbert_base:checkpoints/exp_006_zsh_router/modernbert_base/best ...
    python/.venv/bin/python python/experiments/exp_006_zsh_router/bench_latency.py --all
"""

import argparse
import json
import statistics
import sys
import time
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.zsh_router import apply_context_mode, read_context_mode  # noqa: E402


def time_decisions(model, rows: list[dict], n: int, sync_cuda: bool) -> dict:
    import torch

    from jeff.models.base import DecisionInput
    times = []
    for r in (rows * (1 + n // len(rows)))[:n + 10]:
        t0 = time.perf_counter()
        model.score_options(DecisionInput(r["context"], r["options"]))
        if sync_cuda:
            torch.cuda.synchronize()
        times.append(1000 * (time.perf_counter() - t0))
    times = sorted(times[10:])   # drop warm-up
    return {"p50_ms": round(statistics.median(times), 2),
            "p95_ms": round(times[int(0.95 * len(times))], 2),
            "mean_ms": round(statistics.mean(times), 2), "n": len(times)}


def bench_one(model_key: str, adapter: str, cfg: dict, args) -> dict:
    import torch
    from peft import PeftModel

    from jeff.models.modernbert_decision import ModernBERTConfig, ModernBERTDecisionModel

    m = cfg["models"][model_key]
    mode = read_context_mode(REPO_ROOT / adapter)
    d = cfg["data"]
    rows = [json.loads(line) for line in open(REPO_ROOT / d["out_dir"] / f"{d['prefix']}_val.jsonl")]
    rows = [apply_context_mode(r, mode) for r in rows[:args.n_rows]]
    out = {"model_key": model_key, "adapter": adapter, "context_mode": mode}

    def load(device: str):
        mcfg = ModernBERTConfig(model_name=m["name"], lora_target_modules=m["lora_target_modules"],
                                max_seq_len=m["max_seq_len"], device=device)
        return ModernBERTDecisionModel.from_pretrained(str(REPO_ROOT / adapter), cfg=mcfg)

    model = load("cuda")
    out["gpu"] = time_decisions(model, rows, args.n, sync_cuda=True)
    if isinstance(model.backbone, PeftModel):
        model.backbone = model.backbone.merge_and_unload()
    out["gpu_merged"] = time_decisions(model, rows, args.n, sync_cuda=True)
    del model
    torch.cuda.empty_cache()

    if args.cpu_threads > 0:
        torch.set_num_threads(args.cpu_threads)
        model = load("cpu")
        if isinstance(model.backbone, PeftModel):
            model.backbone = model.backbone.merge_and_unload()
        model.backbone.float()
        model.head.float()
        out["cpu_merged"] = time_decisions(model, rows, args.n_cpu, sync_cuda=False)
        out["cpu_threads"] = args.cpu_threads
        del model
    print(json.dumps(out), flush=True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="*", help="model_key:checkpoint_path pairs")
    ap.add_argument("--all", action="store_true",
                    help="every checkpoints/exp_006_zsh_router/<run>/best with a known model key")
    ap.add_argument("--n", type=int, default=300, help="timed GPU decisions per variant")
    ap.add_argument("--n-cpu", type=int, default=100)
    ap.add_argument("--n-rows", type=int, default=300)
    ap.add_argument("--cpu-threads", type=int, default=8, help="0 skips the CPU variant")
    ap.add_argument("--out", default=str(EXP_DIR / "results" / "latency_bench.json"))
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    runs = [tuple(r.split(":", 1)) for r in args.runs]
    if args.all:
        base = REPO_ROOT / cfg["output"]["dir"]
        keys = sorted(cfg["models"], key=len, reverse=True)   # roberta_base_full before roberta_base
        for run_dir in sorted(base.iterdir()):
            if not (run_dir / "best" / "decision_head.pt").exists():
                continue
            key = next((k for k in keys if run_dir.name == k or run_dir.name.startswith(k + "_")), None)
            if key:
                runs.append((key, str((run_dir / "best").relative_to(REPO_ROOT))))
    results = [bench_one(k, a, cfg, args) for k, a in runs]
    Path(args.out).write_text(json.dumps(results, indent=2) + "\n")
    print(f"\n{'run':40} {'mode':6} {'gpu p50':>8} {'merged':>8} {'cpu p50':>8}")
    for r in results:
        cpu = r.get("cpu_merged", {}).get("p50_ms", float("nan"))
        print(f"{Path(r['adapter']).parent.name:40} {r['context_mode']:6} "
              f"{r['gpu']['p50_ms']:8.1f} {r['gpu_merged']['p50_ms']:8.1f} {cpu:8.1f}")
    print(f"→ {args.out}")


if __name__ == "__main__":
    main()
