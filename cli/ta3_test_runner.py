import concurrent.futures
import shutil
import subprocess
import sys
from pathlib import Path

import pprint

import toml  # type: ignore[import-untyped]
import click

import hermetic
import repo_root
import translation
from tenj_types import UserFacingError

_C_TEST_CASE_DIR = "test_case"
_TRANSLATED_RUST_DIR = "translated_rust"
_TRANSLATED_RUST_WORK_DIR = "resultsdir"


def run(test_corpus: Path, flags: list[str]) -> None:
    test_corpus = test_corpus.resolve()
    pyproject = test_corpus / "tools" / "test_runner" / "pyproject.toml"
    if not pyproject.exists():
        raise UserFacingError(f"Test runner not found: {pyproject}")

    if shutil.which("docker") is None:
        raise UserFacingError("docker not found in PATH")

    if shutil.which("nix") is None:
        raise UserFacingError("nix not found in PATH")

    if flags == ["--help"] or flags == ["-h"] or "--list" in flags or "--clean" in flags:
        _invoke_test_runner(test_corpus, flags)
        return

    failed_translations: list[str] = []

    if "--rust" in flags:
        # Discover C test cases (without --rust, so we find test_case/ dirs to translate)
        list_flags = [f for f in flags if f != "--rust"]
        list_result = _invoke_test_runner(test_corpus, [*list_flags, "--list"], capture_output=True)
        if list_result.returncode != 0:
            click.echo("Error: test runner --list failed", err=True)
            sys.exit(list_result.returncode)

        test_case_rel_paths = [
            line for line in list_result.stdout.decode().splitlines() if line.strip()
        ]
        assert len(test_case_rel_paths) >= 2
        assert test_case_rel_paths[0] == "Loading Nix image into Docker"
        assert test_case_rel_paths[1] == "Loaded image: exec_test_vector:latest"
        test_case_rel_paths = test_case_rel_paths[2:]  # Drop debug output lines

        click.echo(f"Discovered {len(test_case_rel_paths)} test case(s)", err=True)

        tenjin_root = repo_root.find_repo_root_dir_Path()
        max_workers = _parse_jobs(flags)

        with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures = {
                executor.submit(_translate_case, rel_path, test_corpus, tenjin_root): rel_path
                for rel_path in test_case_rel_paths
            }
            for future in concurrent.futures.as_completed(futures):
                if future.result() is not None:
                    failed_translations.append(futures[future])

        if failed_translations:
            click.echo(f"\n{len(failed_translations)} translation(s) failed:", err=True)
            for p in failed_translations:
                click.echo(f"  {p}", err=True)

    cp = _invoke_test_runner(test_corpus, flags)
    sys.exit(cp.returncode)


def _parse_jobs(flags: list[str]) -> int | None:
    """Return ThreadPoolExecutor max_workers from --jobs/-j. None means all cores, 1 means serial."""
    for i, f in enumerate(flags):
        if f in ("--jobs", "-j") and i + 1 < len(flags):
            n = int(flags[i + 1])
            return None if n == 0 else n
        if f.startswith("--jobs="):
            n = int(f.split("=", 1)[1])
            return None if n == 0 else n
    return 1


def _translate_case(rel_path: str, test_corpus: Path, tenjin_root: Path) -> Exception | None:
    test_case_root = test_corpus / rel_path
    translated_rust = test_case_root / _TRANSLATED_RUST_DIR
    candidate_name = test_case_root.name  # cando2 requires this naming convention for libs
    work_dir = test_case_root / _TRANSLATED_RUST_WORK_DIR / candidate_name
    c_source_dir = test_case_root / _C_TEST_CASE_DIR

    # Remove any existing translated_rust (symlink or directory)
    if translated_rust.is_symlink():
        translated_rust.unlink()
    elif translated_rust.is_dir():
        shutil.rmtree(translated_rust)

    # Remove any existing work directory for a clean translation
    if work_dir.exists():
        shutil.rmtree(work_dir)

    click.echo(f"Translating {rel_path} ...", err=True)
    try:
        translation.do_translate(tenjin_root, c_source_dir, work_dir, "tenjinized", "{}")
        _change_binary_name(work_dir / "final", new_name="driver")
        # The test runner expects Cargo.toml directly in translated_rust/.
        # do_translate places the final project in resultsdir/final/, so we
        # point translated_rust at that subdirectory via a symlink.
        translated_rust.symlink_to((work_dir / "final").resolve())
    except Exception as e:
        click.echo(f"  {rel_path}: failed: {e}", err=True)
        return e
    return None


def _invoke_test_runner(
    test_corpus: Path, flags: list[str], **kwargs
) -> subprocess.CompletedProcess:
    return hermetic.run(
        [
            "nix",
            "run",
            "./tools/test_runner",
            "--",
            *flags,
        ],
        cwd=test_corpus,
        check=False,
        with_tenjin_deps=True,
        **kwargs,
    )


def _change_binary_name(crate_root: Path, new_name: str) -> None:
    candidates = [crate_root / "driver", crate_root]
    for child in sorted(crate_root.iterdir()):
        if child.is_dir() and child != crate_root / "driver":
            candidates.append(child)

    pprint.pprint(candidates, compact=True)

    for candidate in candidates:
        cargo_toml_path = candidate / "Cargo.toml"
        if not cargo_toml_path.is_file():
            continue

        with open(cargo_toml_path, "r", encoding="utf-8") as f:
            cargo_toml = toml.load(f)

        if "bin" in cargo_toml:
            cargo_toml["bin"][0]["name"] = new_name

            with open(cargo_toml_path, "w", encoding="utf-8") as f:
                toml.dump(cargo_toml, f)
            break
