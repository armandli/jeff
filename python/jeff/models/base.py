from abc import ABC, abstractmethod
from dataclasses import dataclass


@dataclass
class DecisionInput:
    context: str
    options: list[str]


@dataclass
class DecisionOutput:
    chosen_index: int
    scores: list[float]


class DecisionModel(ABC):
    @abstractmethod
    def decide(self, inp: DecisionInput) -> DecisionOutput: ...

    @abstractmethod
    def score_options(self, inp: DecisionInput) -> list[float]:
        """Return raw log-probabilities for each option."""
        ...
