"""Inference helper for exp_006 zsh router.

Called by the zsh_router C++ binary; writes exactly one JSON line to stdout
(everything else, including library chatter, goes to stderr).

    python python/experiments/exp_006_zsh_router/infer.py \
        --model=checkpoints/exp_006_zsh_router/modernbert_base/best \
        --model-key=modernbert_base --text="ls **/*.py"

Decision, matching evaluate.py's cascade: `zsh -n` fails -> AGENT, otherwise RUN iff
P(RUN) >= --threshold (default 0.5). --raw skips the zsh -n check. If zsh is not
installed the check is skipped and reported as null.
"""

import argparse
import contextlib
import json
import math
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.zsh_router import OPTIONS, RUN, make_context, zsh_parses  # noqa: E402
from jeff.models.base import DecisionInput  # noqa: E402


def fail(msg: str) -> None:
    print(json.dumps({"error": msg}))
    sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--model", required=True, help="checkpoint directory")
    ap.add_argument("--model-key", default="modernbert_base",
                    help="model key in exp_006 config (default: modernbert_base)")
    ap.add_argument("--text", required=True, help="the typed line to classify")
    ap.add_argument("--threshold", type=float, default=0.5, help="RUN when P(RUN) >= this")
    ap.add_argument("--raw", action="store_true", help="model only: no zsh -n check")
    ap.add_argument("--verbose", action="store_true", help="print context and options to stderr")
    args = ap.parse_args()
    if not 0.0 <= args.threshold <= 1.0:
        fail(f"--threshold must be in [0, 1], got {args.threshold}")

    import yaml
    cfg_path = REPO_ROOT / "python/experiments/exp_006_zsh_router/config.yaml"
    models_cfg = yaml.safe_load(cfg_path.read_text())["models"]
    if args.model_key not in models_cfg:
        fail(f"unknown model-key {args.model_key!r}; choices: {', '.join(models_cfg)}")
    m = models_cfg[args.model_key]

    context = make_context(args.text)
    if args.verbose:
        print(f"=== context ===\n{context}\n\n=== options ===", file=sys.stderr)
        for i, opt in enumerate(OPTIONS):
            print(f"  [{i}] {opt}", file=sys.stderr)
        print(file=sys.stderr, flush=True)

    try:
        with contextlib.redirect_stdout(sys.stderr):
            from jeff.models.modernbert_decision import ModernBERTConfig, ModernBERTDecisionModel
            mcfg = ModernBERTConfig(
                model_name=m["name"],
                lora_target_modules=m["lora_target_modules"],
                max_seq_len=m["max_seq_len"],
            )
            model = ModernBERTDecisionModel.from_pretrained(args.model, cfg=mcfg)
            # log-probs per original option, debiased over cyclic permutations
            log_probs = model.score_options(DecisionInput(context=context, options=list(OPTIONS)))
    except Exception as e:  # noqa: BLE001 — report any load/score failure as JSON
        fail(f"{type(e).__name__}: {e}")

    probs = [math.exp(lp) for lp in log_probs]
    total = sum(probs)
    p_run = probs[RUN] / total

    parses = None
    if not args.raw:
        if shutil.which("zsh"):
            parses = zsh_parses(args.text)
        else:
            print("warning: zsh not found on PATH; skipping the zsh -n check", file=sys.stderr)
    if parses is False:
        decision, reason = "AGENT", "zsh -n failed"
    else:
        decision, reason = ("RUN" if p_run >= args.threshold else "AGENT"), "model"

    print(json.dumps({
        "decision": decision,
        "reason": reason,
        "p_run": round(p_run, 6),
        "p_agent": round(1.0 - p_run, 6),
        "threshold": args.threshold,
        "parses": parses,
        "model_key": args.model_key,
        "model_name": m["name"],
    }))


if __name__ == "__main__":
    main()
