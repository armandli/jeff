import json
from dataclasses import dataclass
from pathlib import Path
from torch.utils.data import Dataset


@dataclass
class DecisionExample:
    context: str
    options: list[str]
    correct_index: int


class DecisionDataset(Dataset):
    def __init__(self, examples: list[DecisionExample]):
        self.examples = examples

    def __len__(self) -> int:
        return len(self.examples)

    def __getitem__(self, idx: int) -> DecisionExample:
        return self.examples[idx]


def load_dataset_from_jsonl(path: str | Path) -> DecisionDataset:
    """Load decision examples from a JSONL file.

    Each line: {"context": "...", "options": [...], "correct_index": N}
    """
    examples = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            examples.append(DecisionExample(**d))
    return DecisionDataset(examples)
