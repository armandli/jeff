"""Experiment 004 — LoRA fine-tune GLiNER2 on v2-teacher Doom states.

    python/.venv/bin/python python/experiments/exp_004_gliner2_doom/train.py
    python/.venv/bin/python python/experiments/exp_004_gliner2_doom/train.py --max-steps 20   # smoke

Writes PEFT adapters to <output.dir>/{best,final} (best = lowest eval_loss), then reports
teacher agreement on the full val set for the final adapter. In-game eval: see config.yaml.
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


def agreement(model, rows: list[dict]) -> dict:
    from jeff.models.gliner2_decision import TASK
    correct, t0 = 0, time.perf_counter()
    preds = []
    for r in rows:
        c = r["output"]["classifications"][0]
        out = model.classify_text(r["input"], {TASK: c["labels"]})[TASK]
        out = out["label"] if isinstance(out, dict) else out
        preds.append(out)
        correct += out == c["true_label"][0]
    n = len(rows)
    return {"n": n, "agreement": correct / n,
            "pred_dist": {k: preds.count(k) for k in sorted(set(preds))},
            "ms_per_decision": 1000 * (time.perf_counter() - t0) / n}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-steps", type=int, default=-1)
    ap.add_argument("--model", default=None)
    ap.add_argument("--output-dir", default=None)
    args = ap.parse_args()

    from gliner2 import AutoExtractor
    from gliner2.training.trainer import ExtractorTrainer, TrainingConfig

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    t, lora = cfg["training"], cfg["training"]["lora"]
    out_dir = REPO_ROOT / (args.output_dir or cfg["output"]["dir"])
    train_path, val_path = REPO_ROOT / cfg["data"]["train"], REPO_ROOT / cfg["data"]["val"]
    if not train_path.exists():
        sys.exit(f"missing {train_path}; run prepare_data.py first")

    model = AutoExtractor.from_pretrained(args.model or cfg["model"]["name"])
    tc = TrainingConfig(
        output_dir=str(out_dir),
        num_epochs=t["num_epochs"],
        max_steps=args.max_steps,
        batch_size=t["batch_size"],
        eval_batch_size=t["eval_batch_size"],
        gradient_accumulation_steps=t["gradient_accumulation_steps"],
        encoder_lr=t["encoder_lr"],
        task_lr=t["task_lr"],
        warmup_ratio=t["warmup_ratio"],
        eval_strategy="steps",
        eval_steps=t["eval_steps"],
        save_best=True,
        bf16=t["bf16"],
        fp16=False,
        seed=t["seed"],
        logging_steps=50,
        use_lora=lora["use"],
        lora_r=lora["r"],
        lora_alpha=lora["alpha"],
        lora_dropout=lora["dropout"],
        save_adapter_only=True,
    )
    trainer = ExtractorTrainer(model, tc)
    trainer.train(train_data=str(train_path), eval_data=str(val_path))

    # Teacher agreement on the full val set with the in-memory (final) weights.
    rows = [json.loads(l) for l in open(val_path)]
    m = trainer.model
    m.eval()
    infer = m.get_base_model() if hasattr(m, "get_base_model") else m
    res = {"tag": "final_val", "model": args.model or cfg["model"]["name"], **agreement(infer, rows)}
    res_dir = REPO_ROOT / cfg["output"]["results_dir"]
    res_dir.mkdir(parents=True, exist_ok=True)
    (res_dir / "final_val_agreement.json").write_text(json.dumps(res, indent=2) + "\n")
    print("EVAL " + json.dumps(res), flush=True)


if __name__ == "__main__":
    main()
