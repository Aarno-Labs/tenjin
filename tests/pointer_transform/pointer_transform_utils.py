"""Shared driver for the pointer-transform tests.

Each fixture directory looks like:

    <case>/
      input/                        # source files (.c / .h)
      expected_ptr/                 # golden: after xj-prepare-pointertransform alone
      expected/                     # golden: after chaining xj-prepare-slicetransform
      expected_metadata.json        # golden: side-file written by the pointer tool
                                    #   (per-pointer facts only; no slice records)

The driver mirrors the `pointertransform` preparation pass: it runs
xj-prepare-pointertransform (index rewriting, emitting the per-pointer
metadata side-file) and then xj-prepare-slicetransform (RustSlice
detection + signature reshaping, consuming that side-file) over a
scratch copy of input/ and checks, at each stage:

  1. the output matches the stored golden (mismatches auto-rewrite the
     golden and fail, mirroring the repo's snapshot-test convention:
     inspect with git diff and keep or revert);
  2. the output still parses (clang -fsyntax-only) — the slice pass
     re-parses the pointer pass's output, so the intermediate must
     always be valid C;
  3. the program still compiles and behaves identically (stdout + exit
     code) to the untransformed input.
"""

import json
import shutil
import subprocess
from pathlib import Path

import hermetic
import repo_root

EXTRA_ARGS = [
    "--extra-arg=-Wno-zero-length-array",
    "--extra-arg=-Wno-implicit-int-conversion",
    "--extra-arg=-Wno-unused-function",
]


def _write_compdb(workdir: Path, sources: list[Path]) -> None:
    clang = hermetic.xj_llvm_root(repo_root.localdir()) / "bin" / "clang"
    entries = [
        {
            "directory": workdir.as_posix(),
            "file": src.as_posix(),
            "arguments": [clang.as_posix(), "-std=c11", "-c", src.as_posix()],
        }
        for src in sources
    ]
    (workdir / "compile_commands.json").write_text(json.dumps(entries, indent=2), encoding="utf-8")


def _compile_and_run(workdir: Path, sources: list[Path], exe_name: str):
    exe = workdir / exe_name
    hermetic.run(
        ["clang", "-std=c11", "-o", exe, *sources],
        check=True,
        capture_output=True,
    )
    r = subprocess.run([exe.as_posix()], capture_output=True, text=True, timeout=60, check=False)
    return r.returncode, r.stdout


def _run_tool(tool: Path, workdir: Path, sources: list[Path], extra: list[str]):
    resource_dir = (
        hermetic.run(["clang", "-print-resource-dir"], capture_output=True, check=True)
        .stdout.decode()
        .strip()
    )
    hermetic.run(
        [
            tool,
            "--inplace",
            "-p",
            workdir,
            *EXTRA_ARGS,
            f"--extra-arg=-resource-dir={resource_dir}",
            *extra,
            *sources,
        ],
        check=True,
        capture_output=True,
        cwd=workdir,
    )


def _check_syntax(sources: list[Path]) -> None:
    for src in sources:
        hermetic.run(
            ["clang", "-std=c11", "-fsyntax-only", src],
            check=True,
            capture_output=True,
        )


def _compare_with_golden(case_dir: Path, golden_subdir: str, workdir: Path, filenames: list[str]):
    """Diff workdir outputs against the stored golden tree; on mismatch,
    rewrite the golden and fail (accept the new golden via git diff)."""
    golden_dir = case_dir / golden_subdir
    golden_dir.mkdir(exist_ok=True)
    stale = []
    for name in filenames:
        got = (workdir / name).read_text(encoding="utf-8")
        golden_file = golden_dir / name
        want = golden_file.read_text(encoding="utf-8") if golden_file.exists() else None
        if got != want:
            golden_file.parent.mkdir(parents=True, exist_ok=True)
            golden_file.write_text(got, encoding="utf-8")
            stale.append(name)
    assert not stale, (
        f"{case_dir.name}: output changed for {stale} — golden files under "
        f"{golden_dir} were updated; inspect with git diff and keep or revert"
    )


def _compare_metadata_golden(case_dir: Path, golden_name: str, got_path: Path) -> None:
    got = got_path.read_text(encoding="utf-8")
    golden = case_dir / golden_name
    if not golden.exists() or golden.read_text(encoding="utf-8") != got:
        golden.write_text(got, encoding="utf-8")
        raise AssertionError(
            f"{case_dir.name}: metadata changed — {golden} updated; "
            "inspect with git diff and keep or revert"
        )


def run_case(tmp_path: Path, case_dir: Path) -> None:
    # Keep in sync with the tools' CMakeLists.txt
    ptr_tool = (
        hermetic.xj_prepare_pointertransform_build_dir(repo_root.localdir())
        / "xj-prepare-pointertransform"
    )
    slice_tool = (
        hermetic.xj_prepare_slicetransform_build_dir(repo_root.localdir())
        / "xj-prepare-slicetransform"
    )

    # Paths are relative to input/ so a case can place same-named files
    # in different directories (distinct statics that share a name).
    input_root = case_dir / "input"
    input_files = sorted(
        p.relative_to(input_root).as_posix() for p in input_root.rglob("*") if p.is_file()
    )
    workdir = tmp_path / "codebase"
    workdir.mkdir()
    for name in input_files:
        dest = workdir / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(input_root / name, dest)
    sources = [workdir / n for n in input_files if n.endswith(".c")]
    _write_compdb(workdir, sources)

    baseline = _compile_and_run(workdir, sources, "orig")

    # Pass 1: pointer transform (plain index rewriting + metadata side-file).
    metadata_path = workdir / "metadata.json"
    _run_tool(ptr_tool, workdir, sources, [f"--metadata-out={metadata_path}"])

    _check_syntax(sources)
    intermediate = _compile_and_run(workdir, sources, "mid")
    assert intermediate == baseline, (
        f"intermediate output behaves differently: {intermediate} vs {baseline}"
    )
    _compare_with_golden(case_dir, "expected_ptr", workdir, input_files)
    _compare_metadata_golden(case_dir, "expected_metadata.json", metadata_path)

    # Pass 2: slice transform (RustSlice detection + signature reshaping).
    _run_tool(slice_tool, workdir, sources, [f"--metadata-in={metadata_path}"])

    _check_syntax(sources)
    _compare_with_golden(case_dir, "expected", workdir, input_files)

    final = _compile_and_run(workdir, sources, "final")
    assert final == baseline, f"final output behaves differently: {final} vs {baseline}"
