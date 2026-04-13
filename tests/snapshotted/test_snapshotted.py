import json
from pathlib import Path
import shutil
from subprocess import CompletedProcess
import pytest
import sys

import vcs_helpers
import hermetic
import repo_root


def run_cargo_on_final(cwd: Path, args: list[str], capture_output: bool = False):
    return hermetic.run_cargo_on_translated_code(
        args,
        cwd=cwd,
        check=True,
        capture_output=capture_output,
    )


def contents_of_non_ignored_files(dir: Path) -> dict[Path, str]:
    root = repo_root.find_repo_root_dir_Path()
    result = {}
    filenames = hermetic.check_output_git([
        "ls-files",
        "-z",
        "--cached",
        "--others",
        "--exclude-standard",
        dir.as_posix(),
    ]).split(b"\0")
    for path_b in filenames:
        path = Path(path_b.decode("utf-8"))
        if not path:
            continue
        assert not path.is_absolute()
        path = root / path
        if path.name == "Cargo.lock":
            continue  # We don't care about changes to Cargo.lock
        if path.is_file():
            result[path.relative_to(dir)] = path.read_text(encoding="utf-8")
    return result


def diff_results_with_snapshot(root: Path, src_dir: Path, results_dir: Path):
    snapshot_dir = src_dir / "snapshot"
    if not snapshot_dir.exists():
        snapshot_dir.mkdir()

    before = contents_of_non_ignored_files(snapshot_dir / "final")
    shutil.copytree(dst=snapshot_dir / "00_out", src=results_dir / "00_out", dirs_exist_ok=True)
    shutil.copytree(dst=snapshot_dir / "final", src=results_dir / "final", dirs_exist_ok=True)
    after = contents_of_non_ignored_files(snapshot_dir / "final")

    added = [p for p in after if p not in before]
    removed = [p for p in before if p not in after]
    changed = [p for p in before if p in after and before[p] != after[p]]
    if added:
        print("ADDED:", [str(p) for p in added], file=sys.stderr)
    if removed:
        print("REMOVED:", [str(p) for p in removed], file=sys.stderr)
    if changed:
        print("CHANGED:", [str(p) for p in changed], file=sys.stderr)
        # Best-effort diff of changed files. There are some cases where
        # this will not print a diff for a changed file. One such case is
        # when using git, when a second run changes a file that the first run
        # added. We'll print CHANGED because we'll see the on-disk contents change,
        # but assuming the first run's results were not `git add`ed, git won't
        # have the old contents cached to diff against. This can also happen with jj,
        # but only if the user disables auto tracking.
        print(vcs_helpers.vcs_diff(snapshot_dir).decode("utf-8"), file=sys.stderr)

    if added or removed or changed:
        assert False, (
            f"saw unexpected change in snapshot of {src_dir.relative_to(root)}: {added=}, {removed=}, {changed=}"
        )


def assert_translation_success(cp_ce: CompletedProcess, resultsdir: Path):
    assert cp_ce.returncode == 0, "translation did not succeed"

    translation_metadata_path = resultsdir / "translation_metadata.json"
    assert translation_metadata_path.is_file(), "translation_metadata.json was not generated"
    tmj = json.loads(translation_metadata_path.read_text(encoding="utf-8"))
    assert tmj["results"]["tenjin_final"]["rustc_errors"] == 0, (
        "unexpected rustc errors in final translation results"
    )


def run_snapshotted(root: Path, src_dir: Path, cmd_args: list[str], tmp_resultsdir: Path):
    cp_ce = hermetic.run(
        cmd_args,
        check=False,
        capture_output=False,
    )

    assert_translation_success(cp_ce, tmp_resultsdir)
    diff_results_with_snapshot(root, src_dir, tmp_resultsdir)


def test_assorted_guidance(root: Path, test_dir: Path, tmp_resultsdir: Path):
    src_dir = test_dir / "assorted_guidance"
    cmd_args = [
        (root / "cli" / "10j").as_posix(),
        "translate",
        "--codebase",
        (src_dir / "assorted_guidance.c").as_posix(),
        "--resultsdir",
        tmp_resultsdir.as_posix(),
        "--guidance",
        (src_dir / "guidance.json").as_posix(),
    ]
    run_snapshotted(root, src_dir, cmd_args, tmp_resultsdir)


# We skip on macOS because CIL (and hence codehawk) can't parse some mac headers
@pytest.mark.skipif(sys.platform == "darwin", reason="Skipping on macOS.")
def test_basic_errno(root: Path, test_dir: Path, tmp_resultsdir: Path):
    src_dir = test_dir / "errno_localization" / "assign"
    cmd_args = [
        (root / "cli" / "10j").as_posix(),
        "translate",
        "--codebase",
        src_dir.as_posix(),
        "--resultsdir",
        tmp_resultsdir,
    ]
    run_snapshotted(root, src_dir, cmd_args, tmp_resultsdir)


# We skip on macOS because CIL (and hence codehawk) can't parse some mac headers
@pytest.mark.skipif(sys.platform == "darwin", reason="Skipping on macOS.")
def test_errno_time(root: Path, test_dir: Path, tmp_resultsdir: Path):
    src_dir = test_dir / "errno_localization" / "time"
    cmd_args = [
        (root / "cli" / "10j").as_posix(),
        "translate",
        "--codebase",
        (src_dir / "main.c").as_posix(),
        "--resultsdir",
        tmp_resultsdir,
    ]
    run_snapshotted(root, src_dir, cmd_args, tmp_resultsdir)


def replace_in_snapshotted_files(resultsdir, rel_paths: list[Path], old: str, new: str):
    for thing in ("00_out", "final"):
        for rel_path in rel_paths:
            path = resultsdir / thing / rel_path
            text = path.read_text(encoding="utf-8")
            new_text = text.replace(old, new)
            path.write_text(new_text, encoding="utf-8")


def test_various_unguided(root: Path, test_dir: Path, tmp_resultsdir: Path):
    src_dir = test_dir / "various_unguided"
    cmd_args = [
        (root / "cli" / "10j").as_posix(),
        "translate",
        "--codebase",
        src_dir.as_posix(),
        "--resultsdir",
        tmp_resultsdir.as_posix(),
        "--buildcmd",
        "sh go.sh",
    ]
    cp_ce = hermetic.run(
        cmd_args,
        check=False,
        capture_output=False,
    )

    assert_translation_success(cp_ce, tmp_resultsdir)
    replace_in_snapshotted_files(
        tmp_resultsdir,
        [Path("src", "various_unguided.rs"), Path("src", "various_unguided_unsafe.rs")],
        "fn isatty(__fd: ::core::ffi::c_int",
        "fn isatty(_: ::core::ffi::c_int",
    )  # normalize `isatty`'s function signature across Mac and Linux.
    diff_results_with_snapshot(root, src_dir, tmp_resultsdir)
