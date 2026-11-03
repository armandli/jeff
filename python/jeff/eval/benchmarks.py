"""Evaluation against JEV-style decision benchmarks.

Supported benchmarks (mirrors the public JEV suite):
  - mmlu        : Massive Multitask Language Understanding
  - gpqa        : Graduate-level Google-Proof Q&A
  - bfcl        : Berkeley Function Calling Leaderboard (tool selection)
  - custom      : Any JSONL file in data/benchmarks/
"""

from dataclasses import dataclass

from datasets import load_dataset

from jeff.data.loader import DecisionDataset, DecisionExample
from jeff.eval.metrics import accuracy, ece
from jeff.models.base import DecisionInput, DecisionModel


@dataclass
class BenchmarkResult:
    name: str
    accuracy: float
    ece: float
    n_examples: int


def run_benchmark(model: DecisionModel, name: str, split: str = "test") -> BenchmarkResult:
    dataset = _load_benchmark(name, split)
    predictions, labels, scores_list = [], [], []

    for ex in dataset:
        inp = DecisionInput(context=ex.context, options=ex.options)
        out = model.decide(inp)
        predictions.append(out.chosen_index)
        labels.append(ex.correct_index)
        scores_list.append(out.scores)

    acc = accuracy(predictions, labels)
    cal = ece(scores_list, labels)
    return BenchmarkResult(name=name, accuracy=acc, ece=cal, n_examples=len(dataset))


def _load_benchmark(name: str, split: str) -> DecisionDataset:
    if name == "mmlu":
        return _load_mmlu(split)
    raise NotImplementedError(f"Benchmark '{name}' not yet wired up")


def _load_mmlu(split: str) -> DecisionDataset:
    hf = load_dataset("cais/mmlu", "all", split=split, trust_remote_code=True)
    examples = []
    for row in hf:
        examples.append(DecisionExample(
            context=row["question"],
            options=row["choices"],
            correct_index=row["answer"],
        ))
    return DecisionDataset(examples)
