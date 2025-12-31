import sys
import json
import base64
import zlib
import compression.zstd as zstd
import os
from pathlib import Path
from typing import TypedDict, Union, Any, Optional, Literal, cast
import hashlib
from typing_extensions import NotRequired
import tempfile
from subprocess import CompletedProcess

import hermetic
import repo_root

"""
Type definitions for the Coverage Set (covset) format.

A coverage set represents code coverage data for a C codebase. It stores
which lines of source code have been executed during testing, along with
configuration information about how files were compiled.

Format Overview
---------------
A covset is a JSON document with two top-level keys:

- "files": A mapping from file content hashes to file coverage information
- "configs": An array of unique configuration arrays (for deduplication)

The coverage data for each file is stored as a bitmap where each bit
corresponds to a source line (bit=1 means covered, bit=0 means not covered).
"""

type CompressionType = Literal["identity", "zlib", "zstd"]


class EncodedCoverage(TypedDict):
    """
    Encoded and optionally compressed coverage bitmap (for a single file).

    The bitmap represents line coverage where each bit corresponds to a source
    line.

    Attributes
    ----------
    b64 : str
        Base64-encoded binary data. After decoding and decompressing,
        this yields a bitmap where bit N indicates whether line N is covered.
    compression : CompressionType
        Compression algorithm: "identity", "zlib", or "zstd".
        Defaults to "identity" if not present.
    """

    b64: str
    compression: NotRequired[CompressionType]


class FilePath(TypedDict, total=False):
    """
    Exactly one of 'hex' or 'utf8' should be non-null.

    Attributes
    ----------
    hex : str | None
        Hex-encoded file path bytes (for non-UTF8 paths).
    utf8 : str | None
        UTF-8 encoded file path string (most common case).
    """

    hex: Optional[str]
    utf8: Optional[str]


ConfigReference = Union[int, list[str]]
"""
Reference to a configuration.

Can be either:
- int: An index into the top-level "configs" array (for deduplication)
- list[str]: An inline array of configuration strings

The configuration strings typically represent preprocessor definitions
and compiler options used when building the file.
"""


class FileInfo(TypedDict):
    """
    Coverage and metadata for a single source file.

    Attributes
    ----------
    filepath : FilePath
        Path to the source file, typically relative to the codebase root.

    expandedhash : str | None
        SHA-256 hash of the preprocessed/expanded file contents.
        Used to detect when the same source file produces different
        results under different configurations. May be null if not computed.

    encodedcoverage : EncodedCoverage
        The coverage bitmap for this file, encoded and compressed.

    encodedcoverable : EncodedCoverage
        Encoded and compressed bitmap indicating which lines are coverable
        (by traditional execution coverage metrics).

    misc : dict
        Optional arbitrary JSON value for extension data.
    """

    filepath: FilePath
    expandedhash: Optional[str]
    encodedcoverage: EncodedCoverage
    encodedcoverable: EncodedCoverage
    misc: dict


# Note: TypedDict doesn't support keys with hyphens directly in class syntax.
# The actual keys "config-inputs" and "config-outputs" must be accessed via
# subscript notation: file_info["config-inputs"]

type Sha256Hex = str
type FilesDict = dict[Sha256Hex, FileInfo]
"""
Mapping from file content hashes to file information.

Keys are lowercase hex-encoded SHA-256 hashes of the original source file
contents (before preprocessing). This allows identifying files by content
rather than by path, which is useful when the same file might exist at
different paths or when paths change.
"""


type ConfigsArray = list[list[str]]
"""
Array of unique configuration arrays.

Each inner array is a list of configuration strings (preprocessor definitions,
compiler flags, etc.). The outer array has no duplicate inner arrays.

This structure allows FileInfo entries to reference configurations by index
rather than repeating the full configuration array, saving space when many
files share the same configuration.

Example:
    [
        ["-DDEBUG", "-O0"],
        ["-DNDEBUG", "-O2"],
        ["-DTEST", "-DDEBUG"]
    ]
"""


class CovSetDict(TypedDict):
    """
    Top-level coverage set document structure.

    A covset represents code coverage data for a codebase, storing:
    - Which source files were covered
    - Which lines in each file were executed
    - Configuration information for each file

    Attributes
    ----------
    files : FilesDict
        Mapping from file content SHA-256 hashes to coverage information.
        Each entry contains the coverage bitmap and metadata for one file.

    configs : ConfigsArray
        Array of unique configuration arrays. FileInfo entries can reference
        these by index to avoid duplicating configuration strings.

    Example
    -------
    {
        "files": {
            "abc123...": {
                "filepath": {"utf8": "src/main.c", "hex": null},
                "expandedhash": "def456...",
                "config-inputs": 0,
                "config-outputs": 0,
                "encodedcoverage": {
                    "b64": "eJxz...",
                    "compression": "zlib"
                }
            }
        },
        "configs": [
            ["-DDEBUG", "-I/usr/include"]
        ]
    }
    """

    files: FilesDict
    configs: ConfigsArray


type SetOperation = Literal["union", "intersection", "difference", "symmetric_diff", "negate"]

type MismatchPolicy = Literal["error", "warn", "ignore"]
"""
Policy for handling expanded hash mismatches during set operations.

When combining coverage from two sets, files may have different expanded
hashes (indicating different preprocessing results). This policy controls
the behavior:

- "error": Raise an exception (default, safest)
- "warn": Print a warning but continue
- "ignore": Silently continue
"""


class CovSet:
    """Represents a coverage set and provides methods for loading and saving."""

    def __init__(self, data: CovSetDict):
        """Initializes a CovSet object from a dictionary."""
        if "files" not in data or "configs" not in data:
            raise ValueError("Invalid covset format: missing 'files' or 'configs' key.")
        self.files = data["files"]
        self.configs = data["configs"]

    def to_json_dict(self) -> CovSetDict:
        """Returns the covset data as a dictionary."""
        return {"files": self.files, "configs": self.configs}

    @staticmethod
    def load(filepath: str) -> "CovSet":
        """Loads a covset from a JSON file."""
        with open(filepath, "r", encoding="utf-8") as f:
            return CovSet(json.load(f))

    def save(self, filepath: str):
        """Saves a covset to a JSON file."""
        with open(filepath, "w", encoding="utf-8") as f:
            json.dump(self.to_json_dict(), f, indent=2)


def llvm_profdata_to_CovSetDict(
    llvm_cov_export: dict[str, Any],
    *,
    codebase_path: Path,
    compression: CompressionType = "zstd",
    only_within: list[Path] = [],
) -> CovSetDict:
    """
    Convert `llvm-cov export -format=text` JSON output to `CovSet`.

    Notes:
    - This function marks a line as covered if any segment covering that
        line has `Count > 0`.
    - CovSet keys are SHA-256 hashes of the *original* source file bytes.

    Parameters
    ----------
    llvm_cov_export:
        Parsed JSON object from `llvm-cov export`.
    codebase_path:
        Relative paths will be resolved relative to this directory
    compression:
        Compression for the stored bitmap.
    codebase_only:
        If True, discard coverage for files outside `codebase_path`.
    """
    if not codebase_path.is_dir():
        if (
            codebase_path.exists()
            and codebase_path.suffix == ".c"
            and codebase_path.parent.is_dir()
        ):
            # Special case: single C file as codebase
            codebase_path = codebase_path.parent
        else:
            raise FileNotFoundError(f"Codebase path not found or not a directory: {codebase_path}")

    # The JSON schema varies somewhat across LLVM versions and flags.
    # We support the common shape:
    #   {"data": [{"files": [{"filename": "...", "segments": [...]}, ...]}]}
    data = llvm_cov_export.get("data")
    if not isinstance(data, list):
        raise TypeError("Invalid llvm-cov export JSON: missing/invalid 'data' array")

    files_out: FilesDict = {}
    # We don't currently reconstruct per-file compiler configurations from
    # llvm-cov export; keep a single empty config to satisfy schema.
    configs_out: ConfigsArray = [[]]

    for datum in data:
        if not isinstance(datum, dict):
            continue
        files = datum.get("files")
        if not isinstance(files, list):
            continue

        for f in files:
            if not isinstance(f, dict):
                continue
            filename = f.get("filename") or f.get("name")
            if not isinstance(filename, str) or not filename:
                continue

            filepath = Path(filename)
            if filepath.is_absolute():
                if only_within:
                    if not any(filepath.resolve().is_relative_to(p) for p in only_within):
                        continue
                source_path = filepath
            else:
                source_path = codebase_path / filepath

            # Hash original source bytes to produce CovSet file key.
            source_bytes = source_path.read_bytes()
            file_hash = hashlib.sha256(source_bytes).hexdigest()

            # Determine number of lines to size the bitmap.
            # Compute per-line lengths from a robust UTF-8 decode (with replacement)
            # and derive the line count from that.
            text = source_bytes.decode("utf-8", errors="replace")
            line_texts = text.splitlines()
            # Preserve previous semantics of counting a trailing newline as an
            # additional (empty) final line.
            if text.endswith("\n"):
                line_texts.append("")
            if not line_texts:
                line_texts = [""]
            line_lengths = [len(line) for line in line_texts]

            segments = f.get("segments")
            if segments is None:
                # Some versions nest under "segments": {"segments": ...}
                seg_obj = f.get("segments")
                segments = seg_obj.get("segments") if isinstance(seg_obj, dict) else None

            # If no segments are available, we still record the file with an
            # empty bitmap (useful for reporting).
            if isinstance(segments, list):
                covered, coverable = _covered_lines_from_llvm_segments(segments, line_lengths)
            else:
                covered = 0
                coverable = 0

            files_out[file_hash] = cast(
                FileInfo,
                {
                    "filepath": {"utf8": filename, "hex": None},
                    "expandedhash": None,
                    "encodedcoverage": encode_bitmap(covered, compression),
                    "encodedcoverable": encode_bitmap(coverable, compression),
                    "misc": None,
                },
            )

    return {"files": files_out, "configs": configs_out}


def _covered_lines_from_llvm_segments(
    segments: list[Any], line_lengths: list[int]
) -> tuple[int, int]:
    """Extract 1-based covered & coverable line numbers from an `llvm-cov export`
    segments array.

    LLVM segments represent points where coverage count changes. Each segment
    marks a (line, column) position and the count that applies from that point
    until the next segment.
    """
    covered: int = 0
    coverable: int = 0

    def ret():
        return (covered, coverable)

    if not segments:
        return ret()

    # Filter and parse valid segments
    # Segment format: [Line, Col, Count, HasCount, IsRegionEntry, IsGapRegion, ...]
    parsed_segments: list[tuple[int, int, int, bool]] = []  # (line, col, count, has_count)

    for seg in segments:
        if not isinstance(seg, (list, tuple)) or len(seg) < 3:
            continue
        line = seg[0]
        col = seg[1] if len(seg) > 1 else 1
        count = seg[2]
        # HasCount indicates whether count is meaningful; if false, treat as uncovered
        has_count = seg[3] if len(seg) > 3 else True

        if not isinstance(line, int) or line <= 0:
            continue
        if not isinstance(col, int):
            col = 1

        # If HasCount is false, treat as zero coverage regardless of count field
        if has_count is False or has_count == 0:
            parsed_segments.append((line, col, 0, False))
            continue

        # Count may be int or string depending on exporter; accept numeric strings.
        if isinstance(count, str):
            try:
                count_int = int(count)
            except ValueError:
                continue
        elif isinstance(count, int):
            count_int = count
        else:
            continue

        parsed_segments.append((line, col, count_int, True))

    if not parsed_segments:
        return ret()

    # Segments should already be in order, but sort to be safe
    parsed_segments.sort(key=lambda s: (s[0], s[1]))

    # Process consecutive segment pairs.
    # The count at segment[i] applies to all lines from segment[i] to segment[i+1].
    for i in range(len(parsed_segments) - 1):
        start_line, start_col, count, has_count = parsed_segments[i]
        end_line, end_col, _, _ = parsed_segments[i + 1]

        if start_col == line_lengths[start_line - 1] + 1:
            # Segment starts at column after end of line: move to next line
            start_line += 1

        # If the next segment starts at column 1, the previous segment ends on
        # the line before (exclusive). Otherwise it ends on the same line (inclusive).
        if end_col == 1:
            end_line = end_line - 1

        # Mark all lines from start to end (inclusive)
        for line_num in range(start_line, end_line + 1):
            covered |= (1 if count > 0 else 0) << (line_num - 1)
            coverable |= (1 if has_count else 0) << (line_num - 1)

    # Handle the last segment: if it has count > 0, at minimum that line is covered
    if parsed_segments:
        last_line, last_col, last_count, last_has_count = parsed_segments[-1]
        if last_col == line_lengths[last_line - 1] + 1:
            last_line += 1
        if last_count > 0:
            covered |= 1 << (last_line - 1)
            coverable |= (1 if last_has_count else 0) << (last_line - 1)
    return ret()


def decode_bitmap(encoded_data: EncodedCoverage) -> int:
    """Decodes and decompresses bitmap data from the covset format."""
    compression = encoded_data.get("compression", "identity")
    b64_data = encoded_data["b64"]

    binary_data = base64.b64decode(b64_data)

    if compression == "identity":
        return _bytes_to_bits(binary_data)
    elif compression == "zstd":
        return _bytes_to_bits(zstd.decompress(binary_data))
    elif compression == "zlib":
        return _bytes_to_bits(zlib.decompress(binary_data))
    else:
        raise ValueError(f"Unknown compression type: {compression}")


def encode_bitmap(bitmap: int, compression: CompressionType) -> EncodedCoverage:
    """Compresses and encodes bitmap data into the covset format."""
    binary_data = _bits_to_bytes(bitmap)
    if compression == "identity":
        compressed_data = binary_data
    elif compression == "zstd":
        compressed_data = zstd.compress(binary_data)
    elif compression == "zlib":
        compressed_data = zlib.compress(binary_data)
    else:
        raise ValueError(f"Unknown compression type: {compression}")

    b64_data = base64.b64encode(compressed_data).decode("ascii")
    return {"b64": b64_data, "compression": compression}


type Sexpr = Union[str, list["Sexpr"]]


def parse_sexp(s: str) -> Sexpr:
    """
    Parses a simple s-expression string into a nested list structure.
    Handles quoted strings as atoms.
    """
    s = s.strip()
    if not s.startswith("(") or not s.endswith(")"):
        # It's an atom
        return s.strip('"')

    # It's an expression
    s = s[1:-1].strip()

    parts = []
    balance = 0
    current_part = ""
    in_string = False

    for char in s:
        if char == '"':
            in_string = not in_string

        if char == "(" and not in_string:
            balance += 1
        elif char == ")" and not in_string:
            balance -= 1

        if char.isspace() and balance == 0 and not in_string:
            if current_part:
                parts.append(parse_sexp(current_part))
                current_part = ""
        else:
            current_part += char

    if current_part:
        parts.append(parse_sexp(current_part))

    return parts


def _merge_configs(
    configs1: ConfigsArray, configs2: ConfigsArray
) -> tuple[ConfigsArray, dict[int, int], dict[int, int]]:
    """
    Merges two config lists, creating a unified list and mappings
    from the old config indices to the new ones.
    """
    conf1_tuples = [tuple(c) for c in configs1]
    conf2_tuples = [tuple(c) for c in configs2]

    # Create a sorted list of unique configs
    new_configs_tuples = sorted(list(set(conf1_tuples) | set(conf2_tuples)))

    new_configs = [list(c) for c in new_configs_tuples]

    # Create mappings from old indices to new indices
    map1 = {i: new_configs_tuples.index(conf) for i, conf in enumerate(conf1_tuples)}
    map2 = {i: new_configs_tuples.index(conf) for i, conf in enumerate(conf2_tuples)}

    return new_configs, map1, map2


def _remap_file_info_configs(file_info, mapping):
    """Remaps config indices in a file_info dict using a provided mapping."""
    new_info = file_info.copy()
    for key in ["config-inputs", "config-outputs"]:
        if key in new_info and isinstance(new_info[key], int):
            new_info[key] = mapping[new_info[key]]
    return new_info


def _bitwise_op(int1: int, int2: int, op: SetOperation) -> int:
    """
    Performs a bitwise operation (union, intersection, etc.) on two bitmaps.
    """
    if op == "union":
        result_int = int1 | int2
    elif op == "intersection":
        result_int = int1 & int2
    elif op == "difference":
        result_int = int1 & ~int2
    elif op == "symmetric_diff":
        result_int = int1 ^ int2
    else:
        raise ValueError(f"Unknown bitwise operation: {op}")

    return result_int


def op_negate(covset: CovSet, compression: CompressionType) -> CovSet:
    """Performs a bitwise NOT on all file bitmaps in a covset."""
    new_files = {}
    for fhash, info in covset.files.items():
        new_info = info.copy()
        bitmap = decode_bitmap(info["encodedcoverage"])

        if not bitmap:
            new_info["encodedcoverage"] = encode_bitmap(0, compression)
            new_files[fhash] = new_info
            continue

        bit_length = bitmap.bit_length()
        mask = (1 << bit_length) - 1

        negated_int = ~bitmap & mask

        new_info["encodedcoverage"] = encode_bitmap(negated_int, compression)
        new_files[fhash] = new_info

    return CovSet({"files": new_files, "configs": covset.configs})


def set_operation(
    op: SetOperation,
    set1: CovSet,
    set2: CovSet,
    mismatch_policy: MismatchPolicy,
    compression: CompressionType,
) -> CovSet:
    """
    Performs a binary set operation on two CovSets.
    """
    new_configs, map1, map2 = _merge_configs(set1.configs, set2.configs)
    new_files = {}

    all_file_hashes = set(set1.files.keys()) | set(set2.files.keys())

    for fhash in all_file_hashes:
        in1 = fhash in set1.files
        in2 = fhash in set2.files

        info1 = set1.files.get(fhash)
        info2 = set2.files.get(fhash)

        if in1 and in2:
            assert info1 is not None and info2 is not None
            exp_hash1 = info1.get("expandedhash")
            exp_hash2 = info2.get("expandedhash")
            if exp_hash1 and exp_hash2 and exp_hash1 != exp_hash2:
                msg = f"Expanded hash mismatch for file hash {fhash[:10]}..."
                if mismatch_policy == "error":
                    raise ValueError(msg)
                elif mismatch_policy == "warn":
                    print(f"Warning: {msg}", file=sys.stderr)

        base_info = info1 if in1 else info2
        new_info = _remap_file_info_configs(base_info, map1 if in1 else map2)

        # Perform bitmap operation based on the set operation type
        if op in ("union", "symmetric_diff"):
            if in1 and in2:
                assert info1 is not None and info2 is not None
                bitmap1 = decode_bitmap(info1["encodedcoverage"])
                bitmap2 = decode_bitmap(info2["encodedcoverage"])
                new_bitmap = _bitwise_op(bitmap1, bitmap2, op)
                new_info["encodedcoverage"] = encode_bitmap(new_bitmap, compression)
            elif in1:
                assert info1 is not None
                new_info["encodedcoverage"] = info1["encodedcoverage"]
            else:  # in2
                assert info2 is not None
                new_info["encodedcoverage"] = info2["encodedcoverage"]
            new_files[fhash] = new_info

        elif op == "intersection":
            if in1 and in2:
                assert info1 is not None and info2 is not None
                bitmap1 = decode_bitmap(info1["encodedcoverage"])
                bitmap2 = decode_bitmap(info2["encodedcoverage"])
                new_bitmap = _bitwise_op(bitmap1, bitmap2, "intersection")
                new_info["encodedcoverage"] = encode_bitmap(new_bitmap, compression)
                new_files[fhash] = new_info

        elif op == "difference":
            if in1 and not in2:
                assert info1 is not None
                new_info["encodedcoverage"] = info1["encodedcoverage"]
                new_files[fhash] = new_info
            elif in1 and in2:
                assert info1 is not None and info2 is not None
                bitmap1 = decode_bitmap(info1["encodedcoverage"])
                bitmap2 = decode_bitmap(info2["encodedcoverage"])
                new_bitmap = _bitwise_op(bitmap1, bitmap2, "difference")
                new_info["encodedcoverage"] = encode_bitmap(new_bitmap, compression)
                new_files[fhash] = new_info

    return CovSet({"files": new_files, "configs": new_configs})


def evaluate_exp(
    exp: Sexpr, mismatch_policy: MismatchPolicy, compression: CompressionType
) -> CovSet:
    """Recursively evaluates a parsed s-expression."""
    if not isinstance(exp, list):
        # Atom: a filepath
        return CovSet.load(exp)

    assert isinstance(exp, list) and len(exp) >= 1 and isinstance(exp[0], str)
    op = exp[0].lower()

    # Unary operations
    if op == "negate":
        if len(exp) != 2:
            raise ValueError(f"Operation '{op}' requires 1 argument, got {len(exp) - 1}")
        covset = evaluate_exp(exp[1], mismatch_policy, compression)
        return op_negate(covset, compression)

    if op == "cat":
        if len(exp) != 2:
            raise ValueError(f"Operation '{op}' requires 1 argument, got {len(exp) - 1}")
        covset = evaluate_exp(exp[1], mismatch_policy, compression)
        return op_cat(covset)

    if op == "show":
        if len(exp) != 2:
            raise ValueError(f"Operation '{op}' requires 1 argument, got {len(exp) - 1}")
        covset = evaluate_exp(exp[1], mismatch_policy, compression)
        return op_show(covset)

    # Binary operations
    binary_ops = {"union", "intersection", "difference", "symmetric_diff"}
    if op in binary_ops:
        if len(exp) != 3:
            raise ValueError(f"Operation '{op}' requires 2 arguments, got {len(exp) - 1}")

        set1 = evaluate_exp(exp[1], mismatch_policy, compression)
        set2 = evaluate_exp(exp[2], mismatch_policy, compression)

        op = cast(SetOperation, op)
        return set_operation(op, set1, set2, mismatch_policy, compression)

    raise ValueError(f"Unknown operation: {op}")


def op_cat(covset: CovSet) -> CovSet:
    """Emits the coverage data in JSON format to stdout"""
    cat(covset.to_json_dict())
    return covset


def cat(covset_data: CovSetDict):
    """
    Prints the covset JSON data to stdout.
    """
    print(json.dumps(covset_data, indent=2))


def op_show(covset: CovSet) -> CovSet:
    """Shows coverage for all files in the covset."""
    show(covset.to_json_dict(), Path("."))
    return covset


def show(covset_data: CovSetDict, codebase_path: Path):
    """
    Prints each source file with lines prefixed by '+' (covered) or '-' (not covered).
    """
    files_to_visualize = covset_data.get("files", {})

    if not codebase_path.is_dir():
        raise FileNotFoundError(f"Codebase path not found or not a directory: {codebase_path}")

    for fhash, file_info in files_to_visualize.items():
        filepath_info = file_info.get("filepath")
        if not filepath_info:
            print(f"Warning: Missing filepath for hash {fhash[:10]}...", file=sys.stderr)
            continue

        relative_path = filepath_info.get("utf8")
        if not relative_path:
            print(f"Warning: No utf8 filepath for hash {fhash[:10]}...", file=sys.stderr)
            continue

        source_file_path = codebase_path / relative_path

        if not source_file_path.is_file():
            print(f"Warning: Source file not found: {source_file_path}", file=sys.stderr)
            continue

        print("-" * 40)
        print(f"File: {relative_path}")
        print("-" * 40)

        try:
            covered = decode_bitmap(file_info["encodedcoverage"])
            coverable = decode_bitmap(file_info["encodedcoverable"])

            with open(source_file_path, "r", encoding="utf-8") as f:
                for i, line in enumerate(f):
                    is_covered = (covered & (1 << i)) != 0
                    is_coverable = (coverable & (1 << i)) != 0
                    if is_coverable:
                        prefix = "+" if is_covered else "-"
                    else:
                        prefix = " "
                    print(f"{prefix} {line.rstrip()}")

        except Exception as e:
            print(f"Error processing file {relative_path}: {e}", file=sys.stderr)

        print()

    covered_lines = 0
    coverable_lines = 0
    for fhash, file_info in files_to_visualize.items():
        covered = decode_bitmap(file_info["encodedcoverage"])
        coverable = decode_bitmap(file_info["encodedcoverable"])
        covered_lines += covered.bit_count()
        coverable_lines += coverable.bit_count()
    print("=" * 40)
    coverage_percentage = (covered_lines / coverable_lines) if coverable_lines > 0 else 0
    print(f"Total covered lines: {covered_lines} / {coverable_lines} = {coverage_percentage:.2%}")


def _bytes_to_bits(b: bytes) -> int:
    return int.from_bytes(b, "little")


def _bits_to_bytes(bits: int) -> bytes:
    bytes_needed = (bits.bit_length() + 7) // 8
    return bits.to_bytes(bytes_needed, "little")


def do_eval(
    output: Optional[str],
    expression: str,
    compression: CompressionType,
    on_mismatch: MismatchPolicy = "error",
) -> None:
    try:
        parsed_exp = parse_sexp(expression)
        result_covset = evaluate_exp(parsed_exp, on_mismatch, compression)

        output_json = json.dumps(result_covset.to_json_dict(), indent=2)

        if output:
            with open(output, "w", encoding="utf-8") as f:
                f.write(output_json)
        else:
            if expression.strip().startswith("(cat ") or expression.strip().startswith("(show "):
                return
            print(output_json)

    except (ValueError, FileNotFoundError, json.JSONDecodeError, TypeError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


def _binaries_within(dir: Path) -> list[Path]:
    return [p for p in dir.iterdir() if p.is_file() and os.access(p, os.X_OK)]


def generate_via(
    target: str | None,
    codebase: Path,
    resultsdir: Path,
    output: Path,
    html: bool,
    rust: bool,
    rest: list[str],
) -> CompletedProcess:
    assert resultsdir.is_dir(), f"Results directory not found: {resultsdir}"
    # Resolve relative paths before changing working directory
    resultsdir = resultsdir.absolute()

    if rust:
        finaldir = resultsdir / "final"
        # Rust code emits profile data tied to its own version of llvm-profdata,
        # so we must use it instead of Tenjin's hermetic copy.
        # Note: this assumes that the final codebase is using a *nightly*
        # version of Rust, since
        #   (A) the coverage runtime is only available on nightly, and
        #   (B) we don't install the llvm-tools component, which is what
        #       provides `llvm-profdata` etc, on non-nightly toolchains.
        toolchain_path_cp = hermetic.run(
            ["rustc", "--print", "sysroot"], check=True, cwd=finaldir, capture_output=True
        )
        toolchain_path = Path(toolchain_path_cp.stdout.decode("utf-8").strip())
        llvm_profdata_paths = list(toolchain_path.glob("**/llvm-profdata"))
        assert len(llvm_profdata_paths) == 1, (
            f"Could not find unique llvm-profdata in Rust toolchain {toolchain_path}; had "
            + " & ".join(str(p) for p in llvm_profdata_paths)
        )

        # Clear non-instrumented build artifacts
        # hermetic.run_cargo_on_translated_code(["clean"], cwd=finaldir, check=True)
        hermetic.run_cargo_on_translated_code(
            ["build"],
            cwd=finaldir,
            check=True,
            env_ext={"RUSTFLAGS": "-C instrument-coverage -Awarnings"},
        )
        target_binary_parent = finaldir / "target" / "debug"
        codebase_path = finaldir
        llvm_tools_path = llvm_profdata_paths[0].parent
    else:
        builtcov = resultsdir / "_built_cov"
        assert builtcov.is_dir(), f"Built coverage directory not found: {builtcov}"

        target_binary_parent = builtcov
        codebase_path = codebase
        llvm_tools_path = hermetic.xj_llvm_root(repo_root.localdir()) / "bin"

    binaries_to_consider = _binaries_within(target_binary_parent)
    binaries_matching_target = [
        b for b in binaries_to_consider if target is None or b.name == target
    ]
    if len(binaries_matching_target) == 0:
        raise ValueError(
            f"No target binary matching '{target}' found in {target_binary_parent}. Candidates: "
            + str([b.relative_to(target_binary_parent).as_posix() for b in binaries_to_consider])
        )
    if len(binaries_matching_target) > 1:
        raise ValueError(
            f"Expected exactly one target binary matching '{target}' in {target_binary_parent}, "
            f" found {len(binaries_matching_target)}: "
            + str([
                b.relative_to(target_binary_parent).as_posix() for b in binaries_matching_target
            ])
        )

    target_binary = binaries_matching_target[0].as_posix()

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        # Use LLVM_PROFILE_FILE to direct coverage output and ensure
        # shared libraries do not collide in their output files.
        #    %p = process ID
        #    %m = module name
        cp = hermetic.run(
            [target_binary, *rest],
            env_ext={"LLVM_PROFILE_FILE": (tmp_path / "xj-%p-%m.profraw").as_posix()},
            check=False,
        )

        raws = [p.as_posix() for p in tmp_path.glob("xj-*.profraw")]
        hermetic.run(
            [
                llvm_tools_path / "llvm-profdata",
                "merge",
                "-sparse",
                *raws,
                "-o",
                str(tmp_path / "merged.profdata"),
            ],
            check=True,
        )

        # --compilation-dir
        covex = hermetic.run(
            [
                llvm_tools_path / "llvm-cov",
                "export",
                target_binary,
                "-instr-profile",
                str(tmp_path / "merged.profdata"),
                "--skip-branches",
                "--skip-expansions",
                "--check-binary-ids",
            ],
            check=True,
            capture_output=True,
        )

        if html:
            covex_html = hermetic.run(
                [
                    llvm_tools_path / "llvm-cov",
                    "show",
                    target_binary,
                    "-instr-profile",
                    str(tmp_path / "merged.profdata"),
                    "-show-line-counts-or-regions",
                    "--format=html",
                ],
                check=True,
                capture_output=True,
            ).stdout
        else:
            covex_html = None

    # TODO parse target info from serialized JSON to reconstruct
    # the `-object` flags needed to get coverage for shared libraries

    llvm_cov_json = json.loads(covex.stdout.decode("utf-8"))
    covset_dict = llvm_profdata_to_CovSetDict(
        llvm_cov_json,
        codebase_path=codebase_path,
        compression="zstd",
        only_within=[codebase_path, resultsdir],
    )
    with open(output, "w", encoding="utf-8") as f:
        json.dump(covset_dict, f, indent=2)

    # output.with_suffix(".llvm.json").write_bytes(covex.stdout)
    if covex_html:
        output.with_suffix(".llvm.html").write_bytes(covex_html)

    return cp
