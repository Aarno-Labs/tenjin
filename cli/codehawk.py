import json
from dataclasses import dataclass
from typing import Self


@dataclass
class PPOStatus:
    """Number of Proof Obgliations with each status"""

    stmt: int
    local: int
    api: int
    contract: int
    open: int
    violated: int


@dataclass
class PPOResults:
    """Proof Obligation results organized by PPO type"""

    ppos: dict[str, PPOStatus]

    @classmethod
    def from_dict(cls, data: dict) -> Self:
        return cls(
            ppos={
                name: PPOStatus(
                    stmt=status["stmt"],
                    local=status["local"],
                    api=status["api"],
                    contract=status["contract"],
                    open=status["open"],
                    violated=status["violated"],
                )
                for name, status in data["ppos"].items()
            }
        )


@dataclass
class CodehawkSummary:
    """Partial specification of Codehawk Analysis Summary Results

    tagresults: mapping from Proof Obligation type to status
    fileresults: mapping from source file to Proof Obligation status
    """

    tagresults: PPOResults
    fileresults: PPOResults

    @classmethod
    def from_json(cls, text: str) -> Self:
        data = json.loads(text)
        return cls(
            tagresults=PPOResults.from_dict(data["tagresults"]),
            fileresults=PPOResults.from_dict(data["fileresults"]),
        )
