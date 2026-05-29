import json
from pathlib import Path
from subprocess import CompletedProcess, TimeoutExpired
import tempfile
import re
from difflib import unified_diff
from dataclasses import dataclass

import filelock
import pytest
import pytest_html.extras

import hermetic


@dataclass
class TenjinFixtures:
    root: Path
    request: pytest.FixtureRequest
    tmp_codebase: Path
    tmp_resultsdir: Path
    extras: list
    monkeypatch: pytest.MonkeyPatch


def clean_up_resultsdir(resultsdir: Path):
    """Remove all files in the results directory, to save disk space."""
    # This can add up to dozens of gigabytes across all tests if left in place.
    for subdir in resultsdir.iterdir():
        if (subdir / "target").is_dir():
            run_cargo_on_final(subdir, [hermetic.tenjin_cargo_toolchain_specifier(), "clean"])

    for item in resultsdir.rglob("*.refoldmap.json"):
        if "expand_preprocessor" not in item.as_posix():
            item.unlink()


def assert_final_had_no_unsafe_fns(resultsdir: Path):
    with open(resultsdir / "translation_metadata.json", "r", encoding="utf-8") as f:
        translate_meta = json.load(f)
        unsafe_count = translate_meta["results"]["tenjin_final"]["total_unsafe_fns_count"]
        assert unsafe_count == 0, f"Expected no unsafe functions, got {unsafe_count}"


def annotate_pytest_request_with_translation_notes(fixtures: TenjinFixtures):
    tmp_resultsdir = fixtures.tmp_resultsdir
    if (tmp_resultsdir / "translation_metadata.json").exists():
        metadata_text = (tmp_resultsdir / "translation_metadata.json").read_text(encoding="utf-8")
        metadata_json = json.loads(metadata_text)

        # This would attach a "file" (as a data: URI) -- not what we want!
        # extras.append(pytest_html.extras.text(metadata_text))

        # This puts the metadata text directly in the report, preceding the
        # test's captured CLI output.
        fixtures.extras.append(pytest_html.extras.html("<pre>" + metadata_text + "</pre>"))

        before = (
            metadata_json.get("results", {})
            .get("c2rust_baseline", {})
            .get("total_unsafe_fns_count")
        )
        after = (
            metadata_json.get("results", {}).get("tenjin_final", {}).get("total_unsafe_fns_count")
        )
        fixtures.request.node.summary_html = f"unsafe: {before}->{after}"


def run_cargo_on_final(cwd: Path, args: list[str], capture_output: bool = False, input=None):
    return hermetic.run_cargo_on_translated_code(
        args,
        cwd=cwd,
        check=True,
        capture_output=capture_output,
        input=input,
    )


def cached_git_clone_at_commit(repo_url: str, commit: str) -> Path:
    """
    Clone the given repo at the given commit, and cache in Python's temporary directory
    so that subsequent calls with the same repo and commit are fast, even across
    multiple test runs.
    """

    cache_dir = Path(tempfile.gettempdir()) / "tenjin_pytest_repo_cache"
    repo_cache_path = cache_dir / f"{repo_url.replace('/', '_')}_{commit}"
    done_sentinel = repo_cache_path / ".clone_complete"

    with filelock.FileLock(str(repo_cache_path) + ".lock"):
        # The lock is held: either we're the first in, or we
        # waited and someone else already finished.
        if not done_sentinel.exists():
            # We're the first worker. Do the clone + checkout.
            if repo_cache_path.exists():
                # If the cache directory exists but the sentinel doesn't, it means
                # a previous clone attempt failed, or was interrupted, or we have
                # an older checkout from before using file locks.
                # In any case: Remove the invalid cache before retrying.
                hermetic.run(["rm", "-rf", str(repo_cache_path)], check=True)

            try:
                hermetic.run(
                    ["git", "clone", "--no-checkout", repo_url, str(repo_cache_path)], check=True
                )
                hermetic.run(
                    ["git", "checkout", "--detach", commit], cwd=repo_cache_path, check=True
                )
                done_sentinel.touch()
            except Exception:
                # If something goes wrong, remove the cache directory to avoid leaving a broken clone around
                if repo_cache_path.exists():
                    hermetic.run(["rm", "-rf", str(repo_cache_path)], check=True)
                raise

    return repo_cache_path


@dataclass
class TestOutcome:
    """Result of running a single test vector."""

    skipped: bool
    ok: bool
    name: str
    message: str


def regex_match(pattern: str, text: str) -> bool:
    """Return True if the pattern fully matches the input text."""
    compiled = re.compile(pattern, flags=re.MULTILINE)
    return bool(compiled.fullmatch(text))


def difference(label: str, expected: str, actual: str, n: int = 1500) -> str:
    """Generate a unified diff between expected and actual output."""
    diff = "".join(
        unified_diff(
            expected.splitlines(keepends=True),
            actual.splitlines(keepends=True),
            fromfile=f"expected {label}",
            tofile=f"actual {label}",
        )
    )
    return diff[:n] + ("... [truncated]\n" if len(diff) > n else "")


def _load_test_vector_spec(test_name: str, test_vector: Path) -> tuple[dict, TestOutcome | None]:
    spec = json.loads(test_vector.read_text(encoding="utf-8"))

    if "has_ub" in spec:
        return spec, TestOutcome(
            skipped=True,
            ok=True,
            name=test_name,
            message=f"[test] {test_name}: Skipped (has_ub: {spec['has_ub']})",
        )

    return spec, None


def _run_test_command(
    cmd: list[str],
    test_name: str,
    cwd: Path | None = None,
    stdin: str | None = None,
    timeout: int = 42,
) -> tuple[CompletedProcess | None, TestOutcome | None]:
    try:
        return (
            hermetic.run(
                cmd,
                cwd=cwd,
                input=stdin,
                text=True,
                capture_output=True,
                timeout=timeout,
            ),
            None,
        )
    except TimeoutExpired:
        return None, TestOutcome(
            skipped=False,
            ok=False,
            name=test_name,
            message=f"{test_name}: Command timed out after {timeout} seconds",
        )
    except Exception as e:
        return None, TestOutcome(
            skipped=False,
            ok=False,
            name=test_name,
            message=f"{test_name}: Failed to execute: {e}",
        )


def _test_outcome_from_direct_result(
    *,
    test_name: str,
    spec: dict,
    result: CompletedProcess,
    verbose: bool,
) -> TestOutcome:
    """Convert stdout/stderr/returncode into a TestOutcome."""
    test_stdout = result.stdout or ""
    test_stderr = result.stderr or ""

    expected_rc = spec.get("rc", 0)
    exp_out = spec.get("stdout", {"pattern": "", "is_regex": False})
    exp_err = spec.get("stderr", {"pattern": "", "is_regex": False})

    rc_ok = result.returncode == expected_rc
    out_ok = (
        regex_match(exp_out["pattern"], test_stdout)
        if exp_out.get("is_regex", False)
        else (test_stdout == exp_out["pattern"])
    )
    err_ok = (
        regex_match(exp_err["pattern"], test_stderr)
        if exp_err.get("is_regex", False)
        else (test_stderr == exp_err["pattern"])
    )

    if rc_ok and out_ok and err_ok:
        return TestOutcome(
            skipped=False,
            ok=True,
            name=test_name,
            message=f"[test] {test_name}: Passed",
        )

    reasons = []
    if not out_ok:
        reasons.append("stdout mismatch")
    if not err_ok:
        reasons.append("stderr mismatch")
    if not rc_ok:
        reasons.append("return code mismatch")
    msg = f"{test_name}: " + ", ".join(reasons)

    if verbose:
        msg += "\n" + difference("stdout", exp_out["pattern"], test_stdout)
        msg += "\n" + difference("stderr", exp_err["pattern"], test_stderr)
        msg += f"\nexpected rc={expected_rc}, actual rc={result.returncode}\n"

    return TestOutcome(
        skipped=False,
        ok=False,
        name=test_name,
        message=msg,
    )


def _test_outcome_from_cando2_v2_result(
    *,
    test_name: str,
    test_vector: Path,
    result: CompletedProcess,
) -> TestOutcome:
    """Convert a `cando2` runner JSON report into a TestOutcome.

    This is the newer path for corpus library runners where `cando2` performs
    the comparison internally and emits a per-vector JSON status report.
    """
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError as e:
        return TestOutcome(
            skipped=False,
            ok=False,
            name=test_name,
            message=(
                f"{test_name}: cando2 produced an invalid JSON report: {e}\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            ),
        )

    outcome = report.get(str(test_vector))
    if outcome is None:
        return TestOutcome(
            skipped=False,
            ok=False,
            name=test_name,
            message=(
                f"{test_name}: cando2 report did not include {test_vector}.\n"
                f"report keys: {sorted(report)}\n"
                f"stderr:\n{result.stderr}"
            ),
        )

    outcome_result = outcome.get("result", "<missing>")
    if outcome_result in {"Pass", "Skip"}:
        return TestOutcome(
            skipped=outcome_result == "Skip",
            ok=True,
            name=test_name,
            message=f"[test] {test_name}: {outcome_result}",
        )

    details = [f"{test_name}: cando2 reported {outcome_result}"]
    if "diff" in outcome:
        details.append(outcome["diff"])
    output = outcome.get("output", {})
    if output.get("stdout"):
        details.append(f"stdout:\n{output['stdout']}")
    if output.get("stderr"):
        details.append(f"stderr:\n{output['stderr']}")
    if result.stderr:
        details.append(f"runner stderr:\n{result.stderr}")
    details.append(f"exit code: {result.returncode}")

    return TestOutcome(
        skipped=False,
        ok=False,
        name=test_name,
        message="\n".join(details),
    )


def tractor_case_released(case_path: str) -> bool:
    if "Examples/" in case_path:
        return False
    if "Public-Tests/B02_" in case_path:
        return False
    return True


def run_tractor_test_vector(
    binary: Path,
    test_name: str,
    test_vector: Path,
    verbose: bool = True,
    cwd: Path | None = None,
    cando2_new_interface: bool = False,
) -> TestOutcome:
    """
    Execute a single test vector against the binary.

    Parameters
    ----------
    binary : Path
        Path to the binary to test
    test_name : str
        Name of the test (for reporting)
    spec_path : Path
        Path to the test vector JSON file
    verbose : bool
        If True, print verbose output including diffs
    cwd : Path, optional
        Working directory for running the binary

    Returns
    -------
    TestOutcome
        Result of the test execution
    """

    spec, early_outcome = _load_test_vector_spec(test_name, test_vector)
    if early_outcome is not None:
        return early_outcome

    is_lib_test = "lib_state_in" in spec

    if is_lib_test:
        if cando2_new_interface:
            cmd = [str(binary), "-l", "quiet", "-v", str(test_vector), "lib"]
        else:
            cmd = [str(binary), "lib", "-q", "-c", str(test_vector)]
    else:
        # Application tests: just run the binary with the given arguments and input
        argv_strs = [
            str(arg) for arg in spec.get("argv", [])
        ]  # TA3's JSON may have raw ints, not just strings
        cmd = [str(binary), *argv_strs]

    result, failed_outcome = _run_test_command(
        cmd, test_name, cwd=cwd, stdin=spec.get("stdin", None)
    )
    if failed_outcome is not None:
        return failed_outcome
    assert result is not None

    if is_lib_test and cando2_new_interface:
        return _test_outcome_from_cando2_v2_result(
            test_name=test_name, test_vector=test_vector, result=result
        )
    return _test_outcome_from_direct_result(
        test_name=test_name, spec=spec, result=result, verbose=verbose
    )
