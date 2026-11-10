"""Inference helper for exp_005 bash router.

Called by the bash_router C++ binary; writes exactly one JSON line to stdout
(everything else, including library chatter, goes to stderr).

    python python/experiments/exp_005_shell_router/infer.py \
        --model=checkpoints/exp_005_shell_router/best --text="ls -la ~/Downloads"

Decision, matching evaluate.py's guarded cascade:
  1. is_critical_command(text)  -> AGENT  (must-pass safety gate)
  2. bash -n fails              -> AGENT
  3. otherwise RUN iff P(RUN) >= --threshold
--raw skips steps 1 and 2. The default threshold is 0.5: evaluate.py's val-picked
threshold (~0.009 for best) runs much of the non-English invalid input, so it is not
a good default for manual testing; pass --threshold to try it.
"""

import argparse
import contextlib
import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.shell_router import (  # noqa: E402
    OPTIONS, RUN, bash_parses, is_critical_command, make_context,
)
from jeff.models.base import DecisionInput  # noqa: E402


def fail(msg: str) -> None:
    print(json.dumps({"error": msg}))
    sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--model", required=True, help="checkpoint directory")
    ap.add_argument("--text", required=True, help="the typed line to classify")
    ap.add_argument("--threshold", type=float, default=0.5, help="RUN when P(RUN) >= this")
    ap.add_argument("--raw", action="store_true", help="model only: no bash -n, no guard")
    ap.add_argument("--verbose", action="store_true", help="print context and options to stderr")
    args = ap.parse_args()
    if not 0.0 <= args.threshold <= 1.0:
        fail(f"--threshold must be in [0, 1], got {args.threshold}")

    context = make_context(args.text)
    if args.verbose:
        print(f"=== context ===\n{context}\n\n=== options ===", file=sys.stderr)
        for i, opt in enumerate(OPTIONS):
            print(f"  [{i}] {opt}", file=sys.stderr)
        print(file=sys.stderr, flush=True)

    # Keep stdout clean for the C++ caller: anything printed while loading goes to stderr.
    try:
        with contextlib.redirect_stdout(sys.stderr):
            from jeff.models.qwen3_decision import Qwen3DecisionModel
            model = Qwen3DecisionModel.from_pretrained(args.model)
            # log-probs per original option, debiased over cyclic permutations
            log_probs = model.score_options(DecisionInput(context=context, options=list(OPTIONS)))
    except Exception as e:  # noqa: BLE001 — report any load/score failure as JSON
        fail(f"{type(e).__name__}: {e}")

    probs = [math.exp(lp) for lp in log_probs]
    total = sum(probs)
    p_run = probs[RUN] / total

    parses = None if args.raw else bash_parses(args.text)
    critical = None if args.raw else is_critical_command(args.text)
    if critical:
        decision, reason = "AGENT", "critical-command guard"
    elif parses is False:
        decision, reason = "AGENT", "bash -n failed"
    else:
        decision, reason = ("RUN" if p_run >= args.threshold else "AGENT"), "model"

    print(json.dumps({
        "decision": decision,
        "reason": reason,
        "p_run": round(p_run, 6),
        "p_agent": round(1.0 - p_run, 6),
        "threshold": args.threshold,
        "parses": parses,
        "critical": critical,
    }))


if __name__ == "__main__":
    main()
