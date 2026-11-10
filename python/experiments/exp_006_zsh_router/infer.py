"""Inference helper for exp_006 zsh router.

Called by the zsh_router C++ binary; writes one JSON line to stdout.

    python python/experiments/exp_006_zsh_router/infer.py \
        --model checkpoints/exp_006_zsh_router/modernbert_base/best \
        --model-key modernbert_base \
        --text "ls **/*.py"
"""

import argparse
import json
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.zsh_router import OPTIONS, RUN, make_context  # noqa: E402
from jeff.models.base import DecisionInput  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--model", required=True, help="checkpoint directory")
    ap.add_argument("--model-key", default="modernbert_base",
                    help="model key in exp_006 config (default: modernbert_base)")
    ap.add_argument("--text", required=True, help="the typed line to classify")
    args = ap.parse_args()

    import yaml
    cfg_path = REPO_ROOT / "python/experiments/exp_006_zsh_router/config.yaml"
    models_cfg = yaml.safe_load(cfg_path.read_text())["models"]
    if args.model_key not in models_cfg:
        print(json.dumps({"error": f"unknown model-key {args.model_key!r}"}))
        sys.exit(1)
    m = models_cfg[args.model_key]

    from jeff.models.modernbert_decision import ModernBERTConfig, ModernBERTDecisionModel  # noqa: E402
    mcfg = ModernBERTConfig(
        model_name=m["name"],
        lora_target_modules=m["lora_target_modules"],
        max_seq_len=m["max_seq_len"],
    )
    model = ModernBERTDecisionModel.from_pretrained(args.model, cfg=mcfg)

    inp = DecisionInput(context=make_context(args.text), options=list(OPTIONS))
    # score_options returns log-probs with cyclic-permutation debiasing
    log_probs = model.score_options(inp)

    probs = [math.exp(lp) for lp in log_probs]
    total = sum(probs)
    probs = [p / total for p in probs]

    result = {
        "decision": "RUN" if probs[RUN] >= 0.5 else "AGENT",
        "p_run": round(probs[RUN], 6),
        "p_agent": round(probs[1 - RUN], 6),
        "model_key": args.model_key,
        "model_name": m["name"],
    }
    print(json.dumps(result))


if __name__ == "__main__":
    main()
