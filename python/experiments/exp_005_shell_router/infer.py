"""Inference helper for exp_005 bash router.

Called by the bash_router C++ binary; writes one JSON line to stdout.

    python python/experiments/exp_005_shell_router/infer.py \
        --model checkpoints/exp_005_shell_router/best \
        --text "ls -la ~/Downloads"
"""

import argparse
import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.shell_router import OPTIONS, RUN, make_context  # noqa: E402
from jeff.models.base import DecisionInput  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--model", required=True, help="checkpoint directory")
    ap.add_argument("--text", required=True, help="the typed line to classify")
    args = ap.parse_args()

    from jeff.models.qwen3_decision import Qwen3DecisionModel  # noqa: E402
    model = Qwen3DecisionModel.from_pretrained(args.model)

    inp = DecisionInput(context=make_context(args.text), options=list(OPTIONS))
    # score_options_both returns log-probs with cyclic-permutation debiasing
    log_probs = model.score_options_both(inp)

    # Convert from log-space to probabilities
    probs = [math.exp(lp) for lp in log_probs]
    total = sum(probs)
    probs = [p / total for p in probs]

    result = {
        "decision": "RUN" if probs[RUN] >= 0.5 else "AGENT",
        "p_run": round(probs[RUN], 6),
        "p_agent": round(probs[1 - RUN], 6),
    }
    # Only JSON on stdout — warnings/logs go to stderr automatically.
    print(json.dumps(result))


if __name__ == "__main__":
    main()
