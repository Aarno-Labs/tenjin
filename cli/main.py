import subprocess
import sys
import os
from pathlib import Path

import click

import repo_root
import provisioning
import hermetic
import translation


def do_fmt_py():
    hermetic.check_call_uv("run ruff format".split())


def do_check_py_fmt():
    hermetic.check_call_uv("run ruff format --check".split())


def do_check_py():
    root = repo_root.find_repo_root_dir_Path()
    hermetic.check_call_uv("run ruff check --quiet".split())
    hermetic.check_call_uv([
        "run",
        "mypy",
        root / "cli" / "main.py",
        root / "cli" / "repo_root.py",
        root / "cli" / "constants.py",
        root / "cli" / "sha256sum.py",
        root / "cli" / "provisioning.py",
        root / "cli" / "translation.py",
    ])
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


def do_build_rs(root: Path):
    cargo_profile = os.environ.get("XJ_BUILD_RS_PROFILE", "dev")
    cargo_flags = f"--locked --profile={cargo_profile}"
    hermetic.run_cargo_in(
        f"build {cargo_flags} -p c2rust -p c2rust-transpile".split(),
        cwd=root / "c2rust",
        check=True,
    )
    hermetic.run_cargo_in(
        f"build {cargo_flags} --workspace".split(),
        cwd=root / "xj-improve-multitool",
        check=True,
    )


def do_test_unit_rs():
    root = repo_root.find_repo_root_dir_Path()
    hermetic.run_cargo_in(
        "test --locked -p c2rust -p c2rust-transpile".split(),
        cwd=root / "c2rust",
        check=True,
    )


def parse_git_name_status_line(bs: bytes) -> tuple[str, bytes]:
    """
    >>> parse_git_name_status_line(b'A       .gitignore')
    ("A", b'.gitignore')
    """
    status = bs[0:1].decode("utf-8")
    path = bs.split(b"\t", 1)[-1]
    return (status, path)


def do_check_repo_file_sizes() -> bool:
    """Returns True if the check passed, False otherwise"""

    max_file_size = 987654

    rootdir = repo_root.find_repo_root_dir_Path()
    # fmt: off
    exclusions = [
        "-path", rootdir / ".git", "-o",
        "-path", rootdir / ".jj", "-o",
        "-path", rootdir / "cli" / ".venv", "-o",
        "-path", rootdir / "_local",
    ]
    cmd = [
        "find", str(rootdir), "(", *[str(x) for x in exclusions], ")", "-prune", "-o",
            "-type", "f", "-size", f"+{max_file_size}c", "-print",
    ]
    # fmt: on
    lines = subprocess.check_output(cmd, stderr=subprocess.PIPE).split(b"\n")
    strlines = [os.fsdecode(line) for line in lines if line != b""]
    if not strlines:
        return True

    # See https://git-scm.org/docs/git-check-ignore for details of the output format.
    # We don't check the return value because it is non-zero when no path is ignored,
    # which is not an error case in this context.
    lines = hermetic.run_output_git([
        "check-ignore",
        "--verbose",
        "--non-matching",
        *strlines,
    ]).split(b"\n")
    non_ignored = []
    for line in lines:
        if line == b"":
            continue

        fields, pathname = line.split(b"\t")
        if fields == b"::":
            # Fields are source COLON linenum COLON pattern
            # If all fields are empty, the pathname did not match any pattern,
            # which is to say: it was not ignored.
            non_ignored.append(pathname)

    if not non_ignored:
        return True

    click.echo("ERROR: Unexpected large files:", err=True)
    for line in non_ignored:
        click.echo("\t" + os.fsdecode(line), err=True)
    return False


@click.group()
def cli():
    pass


@cli.command()
@click.option("--codebase", help="Path to the codebase to translate.")
@click.option(
    "--resultsdir",
    help="Output directory for translation results (intermediates + final).",
)
@click.option(
    "--cratename",
    default="tenjinized",
    help="Name of the crate to generate (default: 'tenjinized').",
)
@click.option(
    "--c_main_in",
    help="Relative path to the main C file to translate.",
)
@click.option(
    "--guidance",
    help="Guidance for the translation process. Path or JSON literal.",
)
def translate(codebase, resultsdir, cratename, c_main_in, guidance):
    root = repo_root.find_repo_root_dir_Path()
    do_build_rs(root)
    translation.do_translate(root, Path(codebase), Path(resultsdir), cratename, guidance, c_main_in)


@cli.command()
def fmt_py():
    do_fmt_py()


@cli.command()
def check_py():
    try:
        do_check_py()
    except subprocess.CalledProcessError:
        sys.exit(1)


@cli.command()
def fix_rs():
    """Run `cargo clippy --fix` (+ flags) on our Rust code"""
    do_fix_rs()


@cli.command()
def fmt_rs():
    do_fmt_rs()


@cli.command()
def build_rs():
    try:
        do_build_rs(repo_root.find_repo_root_dir_Path())
    except subprocess.CalledProcessError:
        sys.exit(1)


@cli.command()
def check_rs():
    try:
        do_check_rs()
    except subprocess.CalledProcessError:
        sys.exit(1)


@cli.command()
def test_unit_rs():
    try:
        do_test_unit_rs()
    except subprocess.CalledProcessError:
        sys.exit(1)


@cli.command()
def check_star():
    """Runs all code-level checks (formatting and linting)"""
    # The Click documentation discourages invoking one command from
    # another, and doing so is quite awkward.
    # We instead implement functionality in the do_*() functions
    # and then make each command be a thin wrapper to invoke the fn.
    try:
        do_check_py()
        do_check_rs()
    except subprocess.CalledProcessError:
        sys.exit(1)


@cli.command()
def check_repo_file_sizes():
    if not do_check_repo_file_sizes():
        sys.exit(1)


@cli.command()
def opam():
    "Run opam (with 10j's switch, etc)"
    pass  # placeholder command


@cli.command()
def dune():
    "Run dune (with 10j's switch, etc)"
    pass  # placeholder command


@cli.command()
def cargo():
    "Alias for `10j exec cargo`"
    pass  # placeholder command


# placeholder command
@cli.command()
def exec():
    "Run a command with 10j's PATH etc"
    pass


@cli.command()
@click.argument("wanted", required=False, default="all")
def provision(wanted: str):
    provisioning.provision_desires(wanted)


if __name__ == "__main__":
    # Per its own documentation, Click does not support losslessly forwarding
    # command line arguments. So when we want to do that, we bypass Click.
    # This especially matters for commands akin to `opam exec -- dune --help`.
    # Here, `--` separator ensures that opam passes the `--help` argument to
    # dune. But Click unconditionally consumes the double-dash, resulting in
    # the `--help` argument being unhelpfully consumed by opam itself.
    #
    # Some of the commands below have placeholder `click` commands above,
    # so that `10j --help` will show them in the help text. The ones without
    # placeholders are effectively "hidden" commands.
    if len(sys.argv) > 1:
        if sys.argv[1] == "opam":
            sys.exit(hermetic.run_opam(sys.argv[2:]).returncode)
        if sys.argv[1] == "dune":
            sys.exit(hermetic.run_opam(["exec", "--", "dune", *sys.argv[2:]]).returncode)
        if sys.argv[1] == "cargo":
            sys.exit(hermetic.run_cargo_in(sys.argv[2:], cwd=Path.cwd(), check=False).returncode)
        if sys.argv[1] == "exec":
            sys.exit(hermetic.run_shell_cmd(sys.argv[2:]).returncode)
        if sys.argv[1] == "true":
            sys.exit(0)
        if sys.argv[1] == "clang-ast-xml":
            sys.exit(
                hermetic.run_shell_cmd([
                    "clang",
                    "-fsyntax-only",
                    "-Xclang",
                    "-ast-dump",
                    *sys.argv[2:],
                ]).returncode
            )

    cli()
