from dataclasses import dataclass
from dataclasses_json import dataclass_json


@dataclass_json
@dataclass
class PPOStatus:
    """Number of Proof Obgliations with each status"""

    stmt: int
    local: int
    api: int
    contract: int
    open: int
    violated: int


@dataclass_json
@dataclass
class PPOResults:
    """Proof Obligation results organized by PPO type"""

    ppos: dict[str, PPOStatus]


@dataclass_json
@dataclass
class CodehawkSummary:
    """Partial specification of Codehawk Analysis Summary Results

    tagresults: mapping from Proof Obligation type to status
    fileresults: mapping from source file to Proof Obligation status
    """

    tagresults: PPOResults
    fileresults: PPOResults
