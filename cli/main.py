import subprocess
import sys
import os
from pathlib import Path
import shutil

import click
import requests

import repo_root
import provisioning
import hermetic
import translation
import cli_subcommands
import e2e_smoke_tests


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
@click.option(
    "--buildcmd",
    help="Build command (for in-tree build), will be run via `intercept-build`.",
)
@click.option(
    "--reset-resultsdir",
    help="If the results directory already exists, delete its contents.",
    is_flag=True,
)
def translate(codebase, resultsdir, cratename, c_main_in, guidance, buildcmd, reset_resultsdir):
    root = repo_root.find_repo_root_dir_Path()
    cli_subcommands.do_build_rs(root)
    if guidance is None:
        click.echo("Using empty guidance; pass `--guidance` to refine translation.", err=True)
        guidance = "{}"

    resultsdir = Path(resultsdir)
    if reset_resultsdir and resultsdir.is_dir():
        # Remove contents but not the directory itself, so that open editors don't lose their place.
        for item in resultsdir.iterdir():
            if item.is_dir():
                shutil.rmtree(item)
            else:
                item.unlink()

    translation.do_translate(
        root, Path(codebase), resultsdir, cratename, guidance, c_main_in, buildcmd
    )


@cli.command()
def fmt_py():
    cli_subcommands.do_fmt_py()


@cli.command()
def check_py():
    try:
        cli_subcommands.do_check_py()
    except subprocess.CalledProcessError:
        sys.exit(1)


@cli.command()
def fix_rs():
    """Run `cargo clippy --fix` (+ flags) on our Rust code"""
    cli_subcommands.do_fix_rs()


@cli.command()
def fmt_rs():
    cli_subcommands.do_fmt_rs()


@cli.command()
def build_rs():
    try:
        cli_subcommands.do_build_rs(repo_root.find_repo_root_dir_Path())
    except subprocess.CalledProcessError:
        sys.exit(1)


@cli.command()
def check_rs():
    try:
        cli_subcommands.do_check_rs()
    except subprocess.CalledProcessError:
        sys.exit(1)


@cli.command()
def test_unit_rs():
    try:
        cli_subcommands.do_test_unit_rs()
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
        cli_subcommands.do_check_py()
        cli_subcommands.do_check_rs()
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


@cli.command()
def clang():
    "Alias for `10j exec clang`"
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


@cli.command()
@click.argument(
    "directory", type=click.Path(exists=True, file_okay=False, dir_okay=True, path_type=Path)
)
@click.option(
    "--to", "host_port", required=True, help="Host and port to upload to (e.g., localhost:8080)"
)
def upload_results(directory: Path, host_port: str):
    """Upload translation_metadata.json and translation_snapshot.json to a Tenjin dashboard."""

    # Check if required files exist
    metadata_file = directory / "translation_metadata.json"
    snapshot_file = directory / "translation_snapshot.json"

    if not metadata_file.exists():
        click.echo(f"Error: {metadata_file} does not exist", err=True)
        sys.exit(1)

    if not snapshot_file.exists():
        click.echo(f"Error: {snapshot_file} does not exist", err=True)
        sys.exit(1)

    if host_port.startswith("http"):
        url = f"{host_port}/ingest"
    elif host_port.startswith("localhost") or host_port.startswith("100."):  # Tailscale IP
        url = f"http://{host_port}/ingest"
    else:
        url = f"https://{host_port}/ingest"

    try:
        with open(metadata_file, "rb") as metadata_fp, open(snapshot_file, "rb") as snapshot_fp:
            files = {
                "metadata": ("translation_metadata.json", metadata_fp, "application/json"),
                "snapshot": ("translation_snapshot.json", snapshot_fp, "application/json"),
            }

            response = requests.post(url, files=files)
            response.raise_for_status()

            click.echo(f"Successfully uploaded files to {url}")
            click.echo(f"Response status: {response.status_code}")
            if response.text:
                click.echo(f"Response: {response.text}")

    except requests.exceptions.HTTPError as e:
        click.echo(f"HTTP Error uploading files: {e}", err=True)
        click.echo(f"Response status: {e.response.status_code}", err=True)
        click.echo(f"Response headers: {dict(e.response.headers)}", err=True)
        if e.response.text:
            click.echo(f"Response body: {e.response.text}", err=True)
        sys.exit(1)
    except requests.exceptions.RequestException as e:
        click.echo(f"Request error uploading files: {e}", err=True)
        sys.exit(1)
    except Exception as e:
        click.echo(f"Unexpected error: {e}", err=True)
        sys.exit(1)


@cli.command()
@click.argument("testnames", nargs=-1)
def check_e2e_smoke_tests(testnames):
    allnames = {"1": e2e_smoke_tests.e2e_smoke_test_1, "2": e2e_smoke_tests.e2e_smoke_test_2}
    if not testnames or testnames == ("all",):
        testnames = sorted(list(allnames.keys()))
    for name in testnames:
        if name not in allnames:
            click.echo(f"Unknown test name: {name}", err=True)
            sys.exit(1)
        else:
            allnames[name]()


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
        if sys.argv[1] == "clang":
            sys.exit(hermetic.run_shell_cmd(sys.argv[1:]).returncode)
        if sys.argv[1] == "chkc":
            sys.exit(hermetic.run_chkc(sys.argv[2:]).returncode)
        if sys.argv[1] == "exec":
            sys.exit(hermetic.run_shell_cmd(sys.argv[2:]).returncode)
        if sys.argv[1] == "true":
            sys.exit(0)
        if sys.argv[1] == "uv":
            try:
                hermetic.check_call_uv(sys.argv[2:], cwd=Path.cwd())
            except Exception as e:
                click.echo(f"Error occurred while running uv: {e}", err=True)
                sys.exit(1)
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
