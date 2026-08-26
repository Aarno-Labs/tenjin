"""Automated coverage for xj-analysis, the must-equality library.

`xj-prepare-baserewrite` deletes a pointer and substitutes a base on the
strength of what this library proves, so a wrong answer here is a
miscompilation rather than a missed optimization. Its own test surface is
annotation-driven: each `examples/*.c` carries `xj-expect` comments naming
what every tracked pointer must resolve to, and `xj-musteq-check` exits
non-zero when the analysis disagrees with any of them.

The expected counts are asserted alongside the exit code on purpose. A
file that loses its annotations still exits 0, and "0/0 expectations met"
is the one failure mode a pass/fail check would not see.
"""

import re

import pytest

import cli_subcommands
import hermetic
import repo_root

# (example, number of xj-expect annotations it carries)
_EXAMPLES = [
    ("copy_propagation.c", 12),
    ("field_paths.c", 10),
    ("table_storage.c", 4),
    # The pre-transform shape, kept as documentation of what the pointer
    # pass sees. It has no annotations; what it must do is parse and run
    # the analysis without crashing.
    ("table_storage_input.c", 0),
]


@pytest.fixture
def analysis_builddir():
    """Build xj-analysis and xj-musteq-check, and return the build dir.

    Separate from the `root` fixture's `do_build_star`: the pipeline
    builds this library through xj-prepare-baserewrite's own CMake, so
    `_build_analysis` exists only for the check tool.
    """
    cli_subcommands.do_build_xj_analysis()
    return hermetic.xj_analysis_build_dir(repo_root.localdir())


@pytest.mark.parametrize("example,expected", _EXAMPLES)
def test_musteq_annotations(analysis_builddir, example, expected):
    source = repo_root.find_repo_root_dir_Path() / "xj-analysis" / "examples" / example
    cp = hermetic.run(
        [(analysis_builddir / "xj-musteq-check").as_posix(), source.as_posix(), "--", "-std=c11"],
        capture_output=True,
        check=False,
    )
    stderr = cp.stderr.decode("utf-8")
    assert cp.returncode == 0, f"{example}: xj-musteq-check failed\n{stderr}"

    match = re.search(r"(\d+)/(\d+) expectations met", stderr)
    assert match, f"{example}: no expectation tally in output\n{stderr}"
    met, total = int(match.group(1)), int(match.group(2))
    assert (met, total) == (expected, expected), (
        f"{example}: expected {expected} annotations all met, got {met}/{total}\n{stderr}"
    )
