import json
from pathlib import Path
from subprocess import TimeoutExpired
import tempfile
import re
from difflib import unified_diff
from dataclasses import dataclass

import filelock
import pytest_html.extras

import hermetic


def annotate_pytest_request_with_translation_notes(request, tmp_resultsdir: Path, extras):
    if (tmp_resultsdir / "translation_metadata.json").exists():
        metadata_text = (tmp_resultsdir / "translation_metadata.json").read_text(encoding="utf-8")
        metadata_json = json.loads(metadata_text)

        # This would attach a "file" (as a data: URI) -- not what we want!
        # extras.append(pytest_html.extras.text(metadata_text))

        # This puts the metadata text directly in the report, preceding the
        # test's captured CLI output.
        extras.append(pytest_html.extras.html("<pre>" + metadata_text + "</pre>"))

        before = (
            metadata_json.get("results", {})
            .get("c2rust_baseline", {})
            .get("total_unsafe_fns_count")
        )
        after = (
            metadata_json.get("results", {}).get("tenjin_final", {}).get("total_unsafe_fns_count")
        )
        request.node.summary_html = f"unsafe: {before}->{after}"


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


def run_tractor_test_vector(
    binary: Path,
    test_name: str,
    spec: dict,
    verbose: bool = True,
    cwd: Path | None = None,
) -> TestOutcome:
    """
    Execute a single test vector against the binary.

    Parameters
    ----------
    binary : Path
        Path to the binary to test
    test_name : str
        Name of the test (for reporting)
    spec : dict
        Test specification containing argv, stdin, stdout, stderr, rc, has_ub
    verbose : bool
        If True, print verbose output including diffs
    cwd : Path, optional
        Working directory for running the binary

    Returns
    -------
    TestOutcome
        Result of the test execution
    """
    # Skip tests marked with undefined behavior
    if "has_ub" in spec:
        return TestOutcome(
            skipped=True,
            ok=True,
            name=test_name,
            message=f"[test] {test_name}: Skipped (has_ub: {spec['has_ub']})",
        )

    # Build command
    argv_strs = [
        str(arg) for arg in spec.get("argv", [])
    ]  # TA3's JSON may have raw ints, not just strings
    cmd = [str(binary), *argv_strs]
    stdin = spec.get("stdin", None)

    try:
        result = hermetic.run(
            cmd,
            cwd=cwd,
            input=stdin,
            text=True,
            capture_output=True,
            timeout=30,  # 30 second timeout
        )
    except TimeoutExpired:
        return TestOutcome(
            skipped=False,
            ok=False,
            name=test_name,
            message=f"{test_name}: Command timed out after 30 seconds",
        )
    except Exception as e:
        return TestOutcome(
            skipped=False,
            ok=False,
            name=test_name,
            message=f"{test_name}: Failed to execute: {e}",
        )

    # Extract results
    test_stdout = result.stdout or ""
    test_stderr = result.stderr or ""

    # Build expectations
    expected_rc = spec.get("rc", 0)
    exp_out = spec.get("stdout", {"pattern": "", "is_regex": False})
    exp_err = spec.get("stderr", {"pattern": "", "is_regex": False})

    # Compare results
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

    # Report results
    if rc_ok and out_ok and err_ok:
        return TestOutcome(
            skipped=False,
            ok=True,
            name=test_name,
            message=f"[test] {test_name}: Passed",
        )
    else:
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
