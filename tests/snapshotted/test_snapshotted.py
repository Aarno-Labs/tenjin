from pathlib import Path
import shutil
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
        if path.is_file():
            result[path.relative_to(dir)] = path.read_text(encoding="utf-8")
    return result


def test_assorted_guidance(root: Path, test_dir: Path, tmp_resultsdir: Path):
    src_dir = test_dir / "assorted_guidance"
    cp_ce = hermetic.run(
        [
            (root / "cli" / "10j").as_posix(),
            "translate",
            "--codebase",
            (src_dir / "assorted_guidance.c").as_posix(),
            "--resultsdir",
            tmp_resultsdir,
            "--guidance",
            (src_dir / "guidance.json").as_posix(),
        ],
        check=False,
        capture_output=False,
    )

    snapshot_dir = src_dir / "snapshot"
    if not snapshot_dir.exists():
        snapshot_dir.mkdir()

    before = contents_of_non_ignored_files(snapshot_dir)
    shutil.copytree(dst=snapshot_dir, src=tmp_resultsdir / "final", dirs_exist_ok=True)
    after = contents_of_non_ignored_files(snapshot_dir)

    assert cp_ce.returncode == 0, "translation did not succeed"

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
        assert False, f"saw unexpected change in snapshot of {src_dir.relative_to(root)}"
