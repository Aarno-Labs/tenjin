from pathlib import Path
import os
import subprocess
from dataclasses import dataclass

import click

import hermetic


def vcs_diff(path: Path) -> bytes:
    vcs_dir = find_containing_vcs_dir(path)
    if vcs_dir is None:
        raise RuntimeError(f"No containing .jj or .git directory found for path {path}")
    if vcs_dir.name == ".jj":
        return hermetic.check_output(
            ["jj", "diff", "--git", "--no-pager", "--quiet", path.as_posix()],
            cwd=vcs_root(vcs_dir),
        )
    else:
        assert vcs_dir.name == ".git"
        return hermetic.check_output(
            ["git", "diff", path.as_posix()],
            cwd=vcs_root(vcs_dir),
        )


@dataclass
class WorkingCopyStatus:
    """Status of a working copy in a version control system.

    Notes:
    - `vcs` is either "jj" or "git"
    - `clean` indicates whether the working copy has no uncommitted changes.
    - `pushed` indicates whether the current commit has been pushed to a remote.
       Any branch on any remote! This helps ensure that the working copy is
       reproducible and that the commit is not just local, but it's not foolproof:
       it might have been pushed to a branch that ends up getting squash-merged,
       which will render the observed commit unreachable.
    """

    root: Path
    vcs: str
    clean: bool
    pushed: bool
    origin: str | None = None
    commit: str | None = None


def vcs_working_copy_status(vcs_dir: Path, origin_remote: str = "origin") -> WorkingCopyStatus:
    if vcs_dir.name == ".git":
        return git_working_copy_status(vcs_root(vcs_dir), origin_remote)
    assert vcs_dir.name == ".jj", "Expected .jj or .git directory"
    return jj_working_copy_status(vcs_root(vcs_dir), origin_remote)


def jj_working_copy_status(vcs_root: Path, origin_remote: str = "origin") -> WorkingCopyStatus:
    """
    Get the status of the working copy in a `jj` repository.
    A clean working copy is defined as one that has an empty commit
    on top of an immutable commit. The returned `commit` field is the commit ID
    of the immutable commit.

    - Raises RuntimeError if the working copy does not have the expected origin remote.
    - Raises CalledProcessError if a `jj` invocation fails."""

    remotes_lines: list[str] = (
        hermetic.check_output(["jj", "-R", vcs_root, "git", "remote", "list"])
        .decode("utf-8")
        .splitlines()
    )

    origin_url = None
    for line in remotes_lines:
        if line.startswith("origin "):
            origin_url = line.split(" ", 1)[1].strip()
            break

    if not origin_url:
        raise RuntimeError(f"No remote named {origin_remote} found in 'jj git remote list' output.")

    fields = "empty,commit_id,immutable,remote_bookmarks,parents.len()".split(",")
    #           0       1       2         3         4
    template = ' ++ "," ++ '.join(fields) + r' ++ "\n"'

    lines = (
        hermetic.check_output(
            ["jj", "-R", vcs_root, "log", "--no-graph", "-n2", "--color", "never", "-T", template],
        )
        .decode("utf-8")
        .splitlines()
    )

    if not lines or len(lines) < 2:
        raise RuntimeError(f"Unexpected output from 'jj log': {lines}")

    # Parse the output
    c1 = lines[0].strip().split(",")
    c2 = lines[1].split(",")

    if len(c1) != 5 or len(c2) != 5:
        raise RuntimeError(f"Unexpected output format from 'jj log': {lines}")

    if c1[0] == "true":
        # The first commit is empty, so we use the second commit's details.
        commit = c2[1]
        if c2[2] == "true":
            clean = True
        else:
            # If the second commit is not immutable, we consider the working copy dirty.
            print(
                "The current jj commit is not an empty commit atop an immutable commit, "
                "indicating that the working copy is dirty or otherwise not reproducible."
            )
            clean = False
    else:
        clean = False
        commit = c1[1]

    # Merge commits are never clean. Warn if this changes the determined status.
    if int(c1[4]) > 1:
        if clean:
            click.echo(
                f"jj working copy at {vcs_root} on merge commit thus not considered clean", err=True
            )
        clean = False

    pushed = False
    if commit:
        git_dir = (
            hermetic.check_output(
                ["jj", "-R", vcs_root, "git", "root"],
            )
            .decode("utf-8")
            .strip()
        )

        remote_refs_containing = (
            hermetic.check_output(
                ["git", "--git-dir", git_dir, "branch", "-r", "--contains", commit],
            )
            .decode("utf-8")
            .strip()
            .splitlines()
        )

        if len(remote_refs_containing) > 0:
            pushed = True

    return WorkingCopyStatus(
        root=vcs_root, vcs="jj", clean=clean, origin=origin_url, commit=commit, pushed=pushed
    )


def git_working_copy_status(vcs_root: Path, origin_remote: str = "origin") -> WorkingCopyStatus:
    """
    Get the status of the working copy in a `git` repository.
    A clean working copy is defined as one that has no uncommitted changes.

    - Raises RuntimeError if the working copy does not have the expected origin remote.
    - Raises CalledProcessError if a `git` invocation fails."""
    try:
        remotes_lines = (
            hermetic.check_output(
                ["git", "remote", "get-url", origin_remote],
                cwd=vcs_root,
            )
            .decode("utf-8")
            .strip()
        )
    except subprocess.CalledProcessError:
        remotes_lines = None

    try:
        commit = (
            hermetic.check_output(
                ["git", "rev-parse", "HEAD"],
                cwd=vcs_root,
            )
            .decode("utf-8")
            .strip()
        )
    except subprocess.CalledProcessError:
        commit = None

    clean = (
        hermetic.run(
            ["git", "diff-index", "--quiet", "HEAD", "--"],
            cwd=vcs_root,
        ).returncode
        == 0
    )

    pushed = False
    if commit:
        remote_refs_containing = (
            hermetic.check_output(
                ["git", "branch", "-r", "--contains", commit],
                cwd=vcs_root,
            )
            .decode("utf-8")
            .strip()
            .splitlines()
        )

        if len(remote_refs_containing) > 0:
            pushed = True

    return WorkingCopyStatus(
        root=vcs_root, vcs="git", clean=clean, origin=remotes_lines, commit=commit, pushed=pushed
    )


def vcs_root(vcs_dir: Path) -> Path:
    return vcs_dir.parent


def find_containing_vcs_dir(start_dir: Path) -> Path | None:
    """
    Walk up from the specified directory towards the filesystem root, looking
    for directories named '.jj' or '.git' and return the full path of the first
    such directory found, or None if no such directory was found.
    """
    if start_dir is None:
        start_dir = os.getcwd()

    # Convert to absolute path to handle relative paths
    current_dir = os.path.abspath(start_dir)

    # Keep going until we hit the filesystem root
    while True:
        # Check for .jj or .git in the current directory, preferring .jj
        # (in case of a colocated repository).
        for repo_dir in [".jj", ".git"]:
            potential_path = os.path.join(current_dir, repo_dir)
            if os.path.isdir(potential_path):
                return Path(potential_path)

        # Move up one directory
        parent_dir = os.path.dirname(current_dir)

        # If we've reached the filesystem root, stop searching
        if parent_dir == current_dir:
            return None

        current_dir = parent_dir
