import uuid
from dataclasses import dataclass

from dataclasses_json import dataclass_json, DataClassJsonMixin


type PreprocessorDefinition = tuple[str, str | None]
type PerFilePreprocessorDefinitions = dict[str, list[PreprocessorDefinition]]


@dataclass_json
@dataclass
class TransformationRecord:
    name: str
    results_path: str
    start_unix_timestamp: int
    elapsed_ms: int
    exit_code: int
    stderr_lines: list[str] | None
    stdout_lines: list[str] | None


@dataclass_json
@dataclass
class IngestedCodebase:
    git_repo_url: str
    git_commit: str
    relative_path: str


@dataclass_json
@dataclass
class TranslationInputs:
    codebase: IngestedCodebase | None
    host_platform: str
    per_file_preprocessor_definitions: PerFilePreprocessorDefinitions
    tenjin_git_repo_url: str
    tenjin_git_commit: str
    c2rust_baseline_version: str
    guidance: dict


@dataclass_json
@dataclass
class TranslationResults:
    translation_start_unix_timestamp: int
    translation_elapsed_ms: int
    static_measurement_elapsed_ms: int
    transformations: list[TransformationRecord]
    c2rust_baseline: dict[str, int | float] | None
    tenjin_initial: dict[str, int | float] | None
    tenjin_final: dict[str, int | float] | None


@dataclass
class TranslationRecord(DataClassJsonMixin):  # mixin for better type inference
    translation_uuid: uuid.UUID
    inputs: TranslationInputs
    results: TranslationResults


@dataclass_json
@dataclass
class SubdirectoryFileSnapshot:
    path: str  # relative to the path of the SubdirectorySnapshot
    lines: list[str]
    sha256: str


@dataclass_json
@dataclass
class SubdirectorySnapshot:
    path: str
    files: list[SubdirectoryFileSnapshot]


@dataclass
class TranslationResultsSnapshot(DataClassJsonMixin):  # mixin for better type inference
    for_translation: uuid.UUID

    c_versions: list[SubdirectorySnapshot]
    rust_versions: list[SubdirectorySnapshot]


@dataclass_json
@dataclass
class EndpointEvaluationResults:
    """A given codebase can have multiple endpoints (binaries; functions within libraries;
    binaries wrapping a library function, etc.) that we want to evaluate.

    Each endpoint will have its own input corpus of interest.

    We are primarily concerned with the behavior of a single method within that endpoint,
    """

    target_method: str
    target_file: str

    input_corpus: SubdirectorySnapshot
    input_paths_with_changed_output: list[str]
    input_paths_with_changed_exit_code: list[str]
    inputs_diverged_count: int  # = sum of lengths of above two lists
    inputs_converged_count: int  # = len(input_corpus) - inputs_diverged_count


@dataclass_json
@dataclass
class EvaluationCoverageResult:
    for_translation: uuid.UUID

    fuzzing_time_s: int  # how long did it take to generate the input corpus being used?
    exec_time_s: int  # how long did it take to run the target endpoint against the corpus?
    coverage_bitmap: str | None  # base64-encoded bitmap
    # If we take the files in the coverage snapshot and concatenate them in order,
    # the resulting list of lines should be the same length as the (decoded) bitmap.
    coverage_snapshot: SubdirectorySnapshot | None  # files for which coverage is being measured
    coverage_summary_configuration_achieved_lines_count: int
    coverage_summary_configuration_universal_lines_count: int
    coverage_summary_configuration_preprocessor_induced_lines_count: int


@dataclass_json
@dataclass
class EvaluationResults:
    for_translation: uuid.UUID
    # It may be of interest to measure coverage both of the Rust and C versions.
    coverage_results: list[EvaluationCoverageResult]
    endpoint_evaluations: list[EndpointEvaluationResults]
