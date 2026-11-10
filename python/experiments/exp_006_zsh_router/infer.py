"""Inference helper for exp_006 zsh router.

Called by the zsh_router C++ binary; writes exactly one JSON line to stdout
(everything else, including library chatter, goes to stderr).

    python python/experiments/exp_006_zsh_router/infer.py \
        --model=checkpoints/exp_006_zsh_router/modernbert_base/best \
        --model-key=modernbert_base --text="ls **/*.py"

Decision, matching evaluate.py's cascade: `zsh -n` fails -> AGENT, otherwise RUN iff
P(RUN) >= --threshold (default 0.5). --raw skips the zsh -n check. If zsh is not
installed the check is skipped and reported as null.

--known-command adds the check evaluate.py reports as cascade_known_*: a line whose command
word zsh cannot resolve (`whence`) goes to AGENT; lines with no command word (assignments,
(( )) arithmetic) are left to the model. `zsh -f` is used, so aliases and functions defined in
~/.zshrc count as unknown; evaluation used the training vocabulary as a stand-in for whence.

The checkpoint's context mode (context_mode.txt beside it, written by train.py) and pooling
(pooling.txt) are read automatically. LoRA weights are merged before scoring, which roughly
halves GPU latency (bench_latency.py).
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

import subprocess  # noqa: E402

from jeff.data.zsh_router import (  # noqa: E402
    OPTIONS,
    RUN,
    apply_context_mode,
    command_word,
    make_context,
    read_context_mode,
    zsh_parses,
)
from jeff.models.base import DecisionInput  # noqa: E402


def command_known(text: str) -> bool | None:
    """True if zsh resolves the command word, None if there is no command word."""
    word = command_word(text)
    if not word:
        return None
    r = subprocess.run(["zsh", "-fc", 'whence -- "$1" >/dev/null', "_", word],
                       capture_output=True, timeout=5)
    return r.returncode == 0


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
    ap.add_argument("--known-command", action="store_true",
                    help="also send lines whose command word zsh cannot resolve to AGENT")
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

    mode = read_context_mode(args.model)
    row = apply_context_mode({"text": args.text, "context": make_context(args.text),
                              "options": list(OPTIONS)}, mode)
    context, options = row["context"], row["options"]
    if args.verbose:
        print(f"=== context ({mode}) ===\n{context}\n\n=== options ===", file=sys.stderr)
        for i, opt in enumerate(options):
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
            if hasattr(model.backbone, "merge_and_unload"):
                model.backbone = model.backbone.merge_and_unload()
            log_probs = model.score_options(DecisionInput(context=context, options=options))
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
    known = None
    if args.known_command and parses is not False and shutil.which("zsh"):
        known = command_known(args.text)
    if parses is False:
        decision, reason = "AGENT", "zsh -n failed"
    elif known is False:
        decision, reason = "AGENT", "unknown command word"
    else:
        decision, reason = ("RUN" if p_run >= args.threshold else "AGENT"), "model"

    print(json.dumps({
        "decision": decision,
        "reason": reason,
        "p_run": round(p_run, 6),
        "p_agent": round(1.0 - p_run, 6),
        "threshold": args.threshold,
        "parses": parses,
        "command_known": known,
        "context_mode": mode,
        "model_key": args.model_key,
        "model_name": m["name"],
    }))


if __name__ == "__main__":
    main()
