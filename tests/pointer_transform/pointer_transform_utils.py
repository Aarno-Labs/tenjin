"""Shared driver for the pointer-transform tests.

Each fixture directory looks like:

    <case>/
      input/     # source files (.c / .h)
      expected/  # golden: after xj-prepare-pointertransform

The driver runs the tool over a scratch copy of input/ and checks:

  1. the output matches the stored golden (mismatches auto-rewrite the
     golden and fail, mirroring the repo's snapshot-test convention:
     inspect with git diff and keep or revert);
  2. the output still parses (clang -fsyntax-only);
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
    entries = [
        {
            "directory": workdir.as_posix(),
            "file": src.as_posix(),
            "arguments": ["clang", "-std=c11", "-c", src.as_posix()],
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
            golden_file.write_text(got, encoding="utf-8")
            stale.append(name)
    assert not stale, (
        f"{case_dir.name}: output changed for {stale} — golden files under "
        f"{golden_dir} were updated; inspect with git diff and keep or revert"
    )


def run_case(tmp_path: Path, case_dir: Path) -> None:
    builddir = hermetic.xj_prepare_pointertransform_build_dir(repo_root.localdir())
    # Keep in sync with `xj-prepare-pointertransform/CMakeLists.txt`
    ptr_tool = builddir / "xj-prepare-pointertransform"

    input_files = sorted(p.name for p in (case_dir / "input").iterdir())
    workdir = tmp_path / "codebase"
    workdir.mkdir()
    for name in input_files:
        shutil.copy(case_dir / "input" / name, workdir / name)
    sources = [workdir / n for n in input_files if n.endswith(".c")]
    _write_compdb(workdir, sources)

    baseline = _compile_and_run(workdir, sources, "orig")

    _run_tool(ptr_tool, workdir, sources, [])

    _check_syntax(sources)
    _compare_with_golden(case_dir, "expected", workdir, input_files)

    final = _compile_and_run(workdir, sources, "final")
    assert final == baseline, f"final output behaves differently: {final} vs {baseline}"
