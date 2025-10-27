import os
from pathlib import Path

import hermetic
import repo_root


def do_fmt_py():
    hermetic.check_call_uv("run ruff format".split(), cwd=repo_root.find_repo_root_dir_Path())


def do_check_py_fmt():
    hermetic.check_call_uv(
        "run ruff format --check".split(), cwd=repo_root.find_repo_root_dir_Path()
    )


def do_check_py():
    root = repo_root.find_repo_root_dir_Path()
    hermetic.check_call_uv("run ruff check --quiet".split(), cwd=root)
    hermetic.check_call_uv(
        [
            "run",
            "mypy",
            root / "cli" / "main.py",
            root / "cli" / "repo_root.py",
            root / "cli" / "constants.py",
            root / "cli" / "sha256sum.py",
            root / "cli" / "provisioning.py",
            root / "cli" / "translation.py",
        ],
        cwd=root,
    )
    do_check_py_fmt()


def do_fmt_rs():
    root = repo_root.find_repo_root_dir_Path()
    hermetic.run_cargo_in(["fmt"], cwd=root / "c2rust", check=True)
    hermetic.run_cargo_in(["fmt"], cwd=root / "xj-improve-multitool", check=True)


def do_check_rs_fmt():
    root = repo_root.find_repo_root_dir_Path()
    hermetic.run_cargo_in(["fmt", "--", "--check"], cwd=root / "c2rust", check=True)
    hermetic.run_cargo_in(["fmt", "--", "--check"], cwd=root / "xj-improve-multitool", check=True)


def do_check_rs():
    root = repo_root.find_repo_root_dir_Path()
    hermetic.run_cargo_in(
        """clippy --locked -p c2rust -p c2rust-transpile
                -- -Aclippy::needless_lifetimes -Aclippy::uninlined_format_args""".split(),
        cwd=root / "c2rust",
        check=True,
    )
    hermetic.run_cargo_in(
        "clippy --locked --workspace".split(),
        cwd=root / "xj-improve-multitool",
        check=True,
    )
    do_check_rs_fmt()


def do_fix_rs():
    root = repo_root.find_repo_root_dir_Path()
    hermetic.run_cargo_in(
        """clippy --locked -p c2rust -p c2rust-transpile
                --fix --allow-no-vcs
                -- -Aclippy::needless_lifetimes -Aclippy::uninlined_format_args""".split(),
        cwd=root / "c2rust",
        check=True,
    )
    hermetic.run_cargo_in(
        "clippy --locked --fix --allow-no-vcs --workspace".split(),
        cwd=root / "xj-improve-multitool",
        check=True,
    )
    do_fmt_rs()


def do_build_rs(root: Path, capture_output: bool = False):
    cargo_profile = os.environ.get("XJ_BUILD_RS_PROFILE", "dev")
    cargo_flags = f"--locked --profile={cargo_profile}"
    hermetic.run_cargo_in(
        f"build {cargo_flags} -p c2rust -p c2rust-transpile".split(),
        cwd=root / "c2rust",
        check=True,
        capture_output=capture_output,
    )
    hermetic.run_cargo_in(
        f"build {cargo_flags} --workspace".split(),
        cwd=root / "xj-improve-multitool",
        check=True,
        capture_output=capture_output,
    )


def do_test_unit_rs():
    root = repo_root.find_repo_root_dir_Path()

    env_ext = {}
    if "INSTA_UPDATE" not in os.environ and not hermetic.running_in_ci():
        # INSTA_UPDATE=always has `insta` write updated snapshots directly,
        # avoiding `.snap.new` files and a separate review step with the
        # `cargo-insta` CLI tool. Net: review via version control.
        env_ext["INSTA_UPDATE"] = "always"

    hermetic.run_cargo_in(
        "test --locked -p c2rust -p c2rust-transpile".split(),
        cwd=root / "c2rust",
        check=True,
        env_ext=env_ext,
    )
