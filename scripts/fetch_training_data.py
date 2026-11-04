"""Download freely available training data for jeff to $HOME/data/.

Saves two dataset families:
  $HOME/data/sft/   — labelled multiple-choice examples (context, options, correct_index)
  $HOME/data/dpo/   — preference pairs (prompt, chosen, rejected)

Run from repo root:
    uv run scripts/fetch_training_data.py [--datasets all] [--jobs 4]

Each dataset is skipped if its output file already exists, so the script is
safe to re-run after a partial download.
"""

import argparse
import hashlib
import json
import os
import random
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Callable, Iterator

# ---------------------------------------------------------------------------
# Output paths
# ---------------------------------------------------------------------------

HOME = Path(os.environ.get("HOME", Path.home()))
SFT_DIR = HOME / "data" / "sft"
DPO_DIR = HOME / "data" / "dpo"


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def _write_jsonl(path: Path, records: Iterator[dict], label: str) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    n = 0
    with open(tmp, "w") as f:
        for rec in records:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
            n += 1
    tmp.rename(path)
    print(f"  [ok] {label}: {n} examples → {path}")
    return n


def _skip(path: Path, label: str) -> bool:
    if path.exists():
        lines = sum(1 for _ in open(path))
        print(f"  [skip] {label}: already exists ({lines} lines) → {path}")
        return True
    return False


# ---------------------------------------------------------------------------
# SFT datasets  (multiple-choice decision tasks)
# ---------------------------------------------------------------------------

def fetch_mmlu() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "mmlu.jsonl"
    if _skip(out, "mmlu"): return

    def gen():
        for split in ("auxiliary_train", "validation", "test"):
            ds = load_dataset("cais/mmlu", "all", split=split, trust_remote_code=True)
            for row in ds:
                yield {
                    "context": row["question"],
                    "options": row["choices"],
                    "correct_index": int(row["answer"]),
                    "source": f"mmlu/{split}",
                }
    _write_jsonl(out, gen(), "mmlu")


def fetch_arc() -> None:
    from datasets import load_dataset
    for config in ("ARC-Easy", "ARC-Challenge"):
        tag = config.lower().replace("-", "_")
        out = SFT_DIR / f"{tag}.jsonl"
        if _skip(out, config): continue

        def gen(cfg=config):
            for split in ("train", "validation", "test"):
                ds = load_dataset("allenai/ai2_arc", cfg, split=split)
                for row in ds:
                    labels = row["choices"]["label"]
                    texts = row["choices"]["text"]
                    correct_label = row["answerKey"]
                    if correct_label not in labels:
                        continue
                    yield {
                        "context": row["question"],
                        "options": texts,
                        "correct_index": labels.index(correct_label),
                        "source": f"{cfg}/{split}",
                    }
        _write_jsonl(out, gen(), config)


def fetch_hellaswag() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "hellaswag.jsonl"
    if _skip(out, "hellaswag"): return

    def gen():
        for split in ("train", "validation"):
            ds = load_dataset("Rowan/hellaswag", split=split)
            for row in ds:
                ctx = row["activity_label"] + ": " + row["ctx"]
                yield {
                    "context": ctx,
                    "options": row["endings"],
                    "correct_index": int(row["label"]) if row["label"] != "" else -1,
                    "source": f"hellaswag/{split}",
                }
    _write_jsonl(out, (r for r in gen() if r["correct_index"] >= 0), "hellaswag")


def fetch_winogrande() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "winogrande.jsonl"
    if _skip(out, "winogrande"): return

    def gen():
        for split in ("train", "validation"):
            ds = load_dataset("allenai/winogrande", "winogrande_xl", split=split,
                              trust_remote_code=True)
            for row in ds:
                # sentence has a blank "_"; option1/option2 fill it
                yield {
                    "context": row["sentence"],
                    "options": [row["option1"], row["option2"]],
                    "correct_index": int(row["answer"]) - 1,  # "1"/"2" → 0/1
                    "source": f"winogrande/{split}",
                }
    _write_jsonl(out, gen(), "winogrande")


def fetch_commonsenseqa() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "commonsenseqa.jsonl"
    if _skip(out, "commonsenseqa"): return

    def gen():
        for split in ("train", "validation"):
            ds = load_dataset("tau/commonsense_qa", split=split)
            for row in ds:
                labels = row["choices"]["label"]
                texts = row["choices"]["text"]
                ans = row["answerKey"]
                if ans not in labels:
                    continue
                yield {
                    "context": row["question"],
                    "options": texts,
                    "correct_index": labels.index(ans),
                    "source": f"commonsenseqa/{split}",
                }
    _write_jsonl(out, gen(), "commonsenseqa")


def fetch_truthfulqa() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "truthfulqa.jsonl"
    if _skip(out, "truthfulqa"): return

    def gen():
        ds = load_dataset("truthfulqa/truthful_qa", "multiple_choice", split="validation")
        for row in ds:
            choices = row["mc1_targets"]["choices"]
            labels = row["mc1_targets"]["labels"]
            if 1 not in labels:
                continue
            # mc1 always lists the correct answer first; shuffle deterministically.
            seed = int(hashlib.sha256(row["question"].encode()).hexdigest()[:8], 16)
            order = list(range(len(choices)))
            random.Random(seed).shuffle(order)
            yield {
                "context": row["question"],
                "options": [choices[i] for i in order],
                "correct_index": order.index(labels.index(1)),
                "source": "truthfulqa/validation",
            }
    _write_jsonl(out, gen(), "truthfulqa")


def fetch_openbookqa() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "openbookqa.jsonl"
    if _skip(out, "openbookqa"): return

    def gen():
        for split in ("train", "validation", "test"):
            ds = load_dataset("allenai/openbookqa", "main", split=split)
            for row in ds:
                labels = row["choices"]["label"]
                texts = row["choices"]["text"]
                ans = row["answerKey"]
                if ans not in labels:
                    continue
                yield {
                    "context": row["question_stem"],
                    "options": texts,
                    "correct_index": labels.index(ans),
                    "source": f"openbookqa/{split}",
                }
    _write_jsonl(out, gen(), "openbookqa")


def fetch_piqa() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "piqa.jsonl"
    if _skip(out, "piqa"): return

    def gen():
        for split in ("train", "validation"):
            ds = load_dataset("ybisk/piqa", split=split, revision="refs/convert/parquet")
            for row in ds:
                yield {
                    "context": row["goal"],
                    "options": [row["sol1"], row["sol2"]],
                    "correct_index": int(row["label"]),
                    "source": f"piqa/{split}",
                }
    _write_jsonl(out, gen(), "piqa")


def fetch_siqa() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "siqa.jsonl"
    if _skip(out, "siqa"): return

    def gen():
        for split in ("train", "validation"):
            ds = load_dataset("allenai/social_i_qa", split=split, revision="refs/convert/parquet")
            for row in ds:
                yield {
                    "context": row["context"] + " " + row["question"],
                    "options": [row["answerA"], row["answerB"], row["answerC"]],
                    "correct_index": int(row["label"]) - 1,  # "1"/"2"/"3" → 0/1/2
                    "source": f"siqa/{split}",
                }
    _write_jsonl(out, gen(), "siqa")


def fetch_boolq() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "boolq.jsonl"
    if _skip(out, "boolq"): return

    def gen():
        for split in ("train", "validation"):
            ds = load_dataset("google/boolq", split=split)
            for row in ds:
                yield {
                    "context": row["passage"] + "\n\nQuestion: " + row["question"],
                    "options": ["no", "yes"],
                    "correct_index": int(row["answer"]),  # False=0, True=1
                    "source": f"boolq/{split}",
                }
    _write_jsonl(out, gen(), "boolq")


def fetch_gpqa() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "gpqa.jsonl"
    if _skip(out, "gpqa"): return


    def gen():
        # GPQA is gated: accept the terms on the HF page and run `hf auth login`
        # first. Errors propagate so we never write an empty file that a re-run
        # would then skip.
        for config in ("gpqa_main", "gpqa_diamond", "gpqa_extended"):
            ds = load_dataset("Idavidrein/gpqa", config, split="train")
            for row in ds:
                options = [
                    row["Correct Answer"],
                    row["Incorrect Answer 1"],
                    row["Incorrect Answer 2"],
                    row["Incorrect Answer 3"],
                ]
                # Deterministic shuffle so the correct answer is not always index 0.
                seed = int(hashlib.sha256(row["Question"].encode()).hexdigest()[:8], 16)
                order = list(range(4))
                random.Random(seed).shuffle(order)
                yield {
                    "context": row["Question"],
                    "options": [options[i] for i in order],
                    "correct_index": order.index(0),
                    "source": f"gpqa/{config}",
                }
    _write_jsonl(out, gen(), "gpqa")


def fetch_race() -> None:
    from datasets import load_dataset
    out = SFT_DIR / "race.jsonl"
    if _skip(out, "race"): return

    label_map = {"A": 0, "B": 1, "C": 2, "D": 3}

    def gen():
        for level in ("middle", "high"):
            for split in ("train", "validation", "test"):
                ds = load_dataset("ehovy/race", level, split=split)
                for row in ds:
                    ans = row["answer"]
                    if ans not in label_map:
                        continue
                    yield {
                        "context": row["article"] + "\n\nQuestion: " + row["question"],
                        "options": row["options"],
                        "correct_index": label_map[ans],
                        "source": f"race/{level}/{split}",
                    }
    _write_jsonl(out, gen(), "race")


# ---------------------------------------------------------------------------
# DPO datasets  (preference pairs for RLCD training)
# ---------------------------------------------------------------------------

def fetch_ultrafeedback() -> None:
    from datasets import load_dataset
    out = DPO_DIR / "ultrafeedback.jsonl"
    if _skip(out, "ultrafeedback"): return

    # Use the binarized version — already split into chosen/rejected
    def gen():
        for split in ("train_prefs", "test_prefs"):
            ds = load_dataset("HuggingFaceH4/ultrafeedback_binarized", split=split)
            for row in ds:
                chosen_msgs = row["chosen"]
                rejected_msgs = row["rejected"]
                # Extract assistant turns
                chosen_text = next(
                    (m["content"] for m in reversed(chosen_msgs) if m["role"] == "assistant"), ""
                )
                rejected_text = next(
                    (m["content"] for m in reversed(rejected_msgs) if m["role"] == "assistant"), ""
                )
                prompt = next(
                    (m["content"] for m in chosen_msgs if m["role"] == "user"), ""
                )
                if not (prompt and chosen_text and rejected_text):
                    continue
                yield {
                    "prompt": prompt,
                    "chosen": chosen_text,
                    "rejected": rejected_text,
                    "source": f"ultrafeedback/{split}",
                }
    _write_jsonl(out, gen(), "ultrafeedback")


def fetch_hh_rlhf() -> None:
    from datasets import load_dataset
    out = DPO_DIR / "hh_rlhf.jsonl"
    if _skip(out, "hh_rlhf"): return

    def gen():
        for config in ("harmless-base", "helpful-base", "helpful-online",
                       "helpful-rejection-sampled"):
            try:
                ds = load_dataset("Anthropic/hh-rlhf", data_dir=config, split="train")
            except Exception:
                continue
            for row in ds:
                chosen = row["chosen"].strip()
                rejected = row["rejected"].strip()
                # Last Human turn as prompt
                parts = chosen.split("\n\nHuman: ")
                if len(parts) < 2:
                    continue
                prompt = parts[-1].split("\n\nAssistant:")[0].strip()
                chosen_ans = chosen.rsplit("\n\nAssistant:", 1)[-1].strip()
                rejected_ans = rejected.rsplit("\n\nAssistant:", 1)[-1].strip()
                yield {
                    "prompt": prompt,
                    "chosen": chosen_ans,
                    "rejected": rejected_ans,
                    "source": f"hh_rlhf/{config}",
                }
    _write_jsonl(out, gen(), "hh_rlhf")


def fetch_glaive_function_calling() -> None:
    """Tool-selection decisions — directly relevant to JEV's tool-picking capability."""
    from datasets import load_dataset
    out = SFT_DIR / "glaive_function_calling.jsonl"
    if _skip(out, "glaive_function_calling"): return

    def gen():
        ds = load_dataset("glaiveai/glaive-function-calling-v2", split="train",
                          trust_remote_code=True)
        for row in ds:
            # Each row has a system prompt listing available functions and a chat
            # We store as-is for fine-tuning; convert to decision format where possible.
            system = row.get("system", "")
            chat = row.get("chat", "")
            if not chat:
                continue
            yield {
                "system": system,
                "chat": chat,
                "source": "glaive_function_calling/train",
            }
    _write_jsonl(out, gen(), "glaive_function_calling")


# ---------------------------------------------------------------------------
# Registry + CLI
# ---------------------------------------------------------------------------

SFT_FETCHERS: dict[str, Callable] = {
    "mmlu": fetch_mmlu,
    "arc": fetch_arc,
    "hellaswag": fetch_hellaswag,
    "winogrande": fetch_winogrande,
    "commonsenseqa": fetch_commonsenseqa,
    "truthfulqa": fetch_truthfulqa,
    "openbookqa": fetch_openbookqa,
    "piqa": fetch_piqa,
    "siqa": fetch_siqa,
    "boolq": fetch_boolq,
    "gpqa": fetch_gpqa,
    "race": fetch_race,
    "glaive_function_calling": fetch_glaive_function_calling,
}

DPO_FETCHERS: dict[str, Callable] = {
    "ultrafeedback": fetch_ultrafeedback,
    "hh_rlhf": fetch_hh_rlhf,
}

ALL_FETCHERS = {**SFT_FETCHERS, **DPO_FETCHERS}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--datasets", nargs="+", default=["all"],
        help=f"Which datasets to fetch. 'all' fetches everything. "
             f"Available: {', '.join(sorted(ALL_FETCHERS))}",
    )
    parser.add_argument(
        "--jobs", type=int, default=1,
        help="Number of datasets to download in parallel (default: 1). "
             "Increase carefully — HuggingFace rate-limits aggressive parallel requests.",
    )
    args = parser.parse_args()

    targets: list[str]
    if args.datasets == ["all"]:
        targets = list(ALL_FETCHERS)
    else:
        unknown = set(args.datasets) - ALL_FETCHERS.keys()
        if unknown:
            print(f"Unknown datasets: {unknown}", file=sys.stderr)
            sys.exit(1)
        targets = args.datasets

    print(f"Saving SFT data to : {SFT_DIR}")
    print(f"Saving DPO data to : {DPO_DIR}")
    print(f"Fetching {len(targets)} dataset(s) with {args.jobs} worker(s)\n")

    errors: list[tuple[str, Exception]] = []

    def run(name: str) -> tuple[str, Exception | None]:
        print(f"  [start] {name}")
        try:
            ALL_FETCHERS[name]()
            return name, None
        except Exception as e:
            return name, e

    if args.jobs == 1:
        for name in targets:
            _, err = run(name)
            if err:
                print(f"  [error] {name}: {err}", file=sys.stderr)
                errors.append((name, err))
    else:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {pool.submit(run, name): name for name in targets}
            for fut in as_completed(futures):
                name, err = fut.result()
                if err:
                    print(f"  [error] {name}: {err}", file=sys.stderr)
                    errors.append((name, err))

    print(f"\nDone. {len(targets) - len(errors)}/{len(targets)} datasets succeeded.")
    if errors:
        print("Failed:", [n for n, _ in errors], file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
