from pathlib import Path
from codehawk import CodehawkSummary
import hermetic
import translation_preparation
import compilation_database


def run_errno_analysis_on_file(test_dir, tmp_codebase, filename):
    """Test that chkc can parse, analyze, and report on a trivial C file."""
    translation_preparation.copy_codebase(test_dir / filename, tmp_codebase)

    # Running codehawk on a single file (i.e., using c-file instead of c-project)
    # actually doesn't produce the same summary json. Since c-project is how
    # codehawk is used in tenjin, we generate a compile_commands so that we can use it here.
    compilation_database.write_synthetic_compile_commands_to(
        compdb_path=Path(tmp_codebase / "compile_commands.json"),
        c_file=tmp_codebase / filename,
        builddir=tmp_codebase,
    )
    hermetic.run_chkc(["c-project", "parse", tmp_codebase, "errno_analysis"], check=True)
    hermetic.run_chkc(
        ["c-project", "analyze", "--analysis", "errno", tmp_codebase, "errno_analysis"], check=True
    )
    results = open(
        (tmp_codebase / "errno_analysis_summaryresults.json").as_posix(), encoding="utf-8"
    )
    assert results is not None, "Analysis did not generate a report"

    report = CodehawkSummary.from_json(results.read())
    assert "errno-must-written" in report.tagresults.ppos
    errno = report.tagresults.ppos["errno-must-written"]
    return errno


def test_errno_basic(test_dir, tmp_codebase):
    errno = run_errno_analysis_on_file(test_dir / "basic", tmp_codebase, "basic.c")
    assert errno.local == 3, "Expected only safe POs"
    assert errno.violated == 0, "Found violated POs"
    assert errno.open == 0, "Found open POs"


def test_errno_pointers_pos(test_dir, tmp_codebase):
    errno = run_errno_analysis_on_file(test_dir / "fpointers", tmp_codebase, "pos.c")
    assert errno.violated == 0, "Found a violated PO"
    assert errno.open == 0, "Found an open PO"
    assert errno.local == 3, "Expected a safe PO"


def test_errno_pointers_neg(test_dir, tmp_codebase):
    errno = run_errno_analysis_on_file(test_dir / "fpointers", tmp_codebase, "neg.c")
    assert errno.open == 3, "Expected an open PO"
