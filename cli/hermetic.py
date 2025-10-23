import subprocess
import shlex
import shutil
import time
from pathlib import Path
import tomllib
import os
from typing import Sequence

import click

import repo_root
import provisioning


def check_call_uv(args: Sequence[str | os.PathLike[str]], cwd: Path) -> None:
    # The args here should be kept in sync with the 10j script.
    localdir = repo_root.localdir()
    run(
        [localdir / "uv", "--config-file", localdir / "uv.toml", *args],
        cwd=cwd,
        check=True,
        with_tenjin_deps=False,
    )


def xj_build_deps(localdir: Path) -> Path:
    return localdir / "xj-build-deps"


def xj_more_deps(localdir: Path) -> Path:
    return localdir / "xj-more-deps"


def xj_llvm_root(localdir: Path) -> Path:
    return localdir / "xj-llvm"


def xj_upstream_c2rust(localdir: Path) -> Path:
    return localdir / "upstream-c2rust"


def xj_codehawk(localdir: Path) -> Path:
    return localdir / "codehawk"


def xj_codehawk_c(localdir: Path) -> Path:
    return localdir / "codehawk-c"


def mk_env_for(localdir: Path, with_tenjin_deps=True, env_ext=None, **kwargs) -> dict[str, str]:
    if "env" in kwargs:
        env = kwargs["env"]
        del kwargs["env"]  # we'll pass it explicitly, so not via kwargs
    else:
        env = os.environ.copy()

    if env_ext is not None:
        env = {**env, **env_ext}

    if with_tenjin_deps:
        # We define LLVM_LIB_DIR for c2rust (unconditionally).
        env["LLVM_LIB_DIR"] = str(xj_llvm_root(localdir) / "lib")
        env["PATH"] = os.pathsep.join([
            str(xj_build_deps(localdir) / "bin"),
            str(xj_more_deps(localdir) / "bin"),
            str(xj_llvm_root(localdir) / "bin"),
            str(localdir / "cmake" / "bin"),
            env["PATH"],
        ])
        env["LD_LIBRARY_PATH"] = os.pathsep.join([
            str(xj_llvm_root(localdir) / "lib"),
            env.get("LD_LIBRARY_PATH", ""),
        ])

    return env


def run_command_with_progress(
    command, stdout_file, stderr_file, cwd=None, shell=False, env_ext=None, suppress_helper=False
) -> None:
    """
    Run a command, redirecting stdout/stderr to files, and print dots while waiting.
    """
    if not suppress_helper:
        common_helper_for_run(command)

    with open(stdout_file, "wb") as out_f, open(stderr_file, "wb") as err_f:
        proc = subprocess.Popen(
            command,
            stdout=out_f,
            stderr=err_f,
            shell=shell,
            cwd=cwd,
            env=mk_env_for(repo_root.localdir(), with_tenjin_deps=True, env_ext=env_ext),
        )

        start_s = time.perf_counter()
        while proc.poll() is None:
            # Process is still running
            print(".", end="", flush=True)
            time.sleep(0.3)
        elapsed_s = time.perf_counter() - start_s

        # Overall time elapsed including final newline after progress dots
        print(f" ({elapsed_s:.2f} s)")

        # If something went wrong, show the error. The redirected files
        # are, in general, an implementation detail, not for user consumption.
        if proc.returncode != 0:
            with open(stderr_file, encoding="utf-8") as f:
                click.echo(f.read(), err=True)
        assert proc.returncode == 0, f"Command failed with return code {proc.returncode}"


type RunSpec = str | Sequence[str | bytes | os.PathLike[str] | os.PathLike[bytes]]


def shellize(cmd: RunSpec) -> str:
    if isinstance(cmd, str):
        return cmd
    else:
        return " ".join(shlex.quote(str(x)) for x in cmd)


def common_helper_for_run(cmd: RunSpec, cmd_cwd: Path | str | None = None):
    if provisioning.HAVE.provisioning_depth == 0 and not running_in_ci():
        # CI is careful to provision what it needs; doing more here
        # would merely slow down the CI run with unnecessary work.
        provisioning.provision_desires("all")

    def print_cmd_only():
        click.echo(f": {cmd}")

    def print_cmd_within(cdpath: Path):
        click.echo(f": ( cd {cdpath.as_posix()} ; {shellize(cmd)} )")

    if os.environ.get("XJ_SHOW_CMDS", "0") != "0":
        if os.environ.get("PWD") is None:
            print_cmd_only()
            return

        if cmd_cwd is None:
            print_cmd_only()
            return

        invoked_from = Path(os.environ["PWD"]).resolve()
        cmd_cwd = Path(cmd_cwd).resolve()
        if cmd_cwd == invoked_from:
            print_cmd_only()
        else:
            try:
                cdpath = cmd_cwd.relative_to(invoked_from)
                print_cmd_within(cdpath)
            except ValueError:
                print_cmd_within(cmd_cwd)


def run(
    cmd: RunSpec, check=False, with_tenjin_deps=True, env_ext=None, **kwargs
) -> subprocess.CompletedProcess:
    common_helper_for_run(cmd, kwargs.get("cwd", None))

    return subprocess.run(
        cmd,
        check=check,
        env=mk_env_for(repo_root.localdir(), with_tenjin_deps, env_ext),
        **kwargs,
    )


def run_shell_cmd(
    cmd: RunSpec, check=False, with_tenjin_deps=True, env_ext=None, **kwargs
) -> subprocess.CompletedProcess:
    return run(
        shellize(cmd),
        check=check,
        with_tenjin_deps=with_tenjin_deps,
        env_ext=env_ext,
        shell=True,
        **kwargs,
    )


def check_output(cmd: RunSpec, cwd: Path | None = None) -> bytes:
    common_helper_for_run(cmd)

    return subprocess.check_output(
        cmd,
        cwd=cwd,
        env=mk_env_for(repo_root.localdir(), with_tenjin_deps=False, env_ext=None),
    )


def tenjin_cargo_toolchain_specifier() -> str:
    spec = provisioning.HAVE.query("10j-xj-default-rust-toolchain")
    if spec is None:
        # We should not encounter this case because we call `provisioning.want_10j_rust_toolchains()`
        # beforehand. But if we do, let's at least give a helpful warning & soldier on.
        click.echo(
            "WARNING: `10j provision` needs to be run, falling back to stable Rust.", err=True
        )
        spec = "stable"
    return os.environ.get("XJ_CARGO_TOOLCHAIN_SPEC", "+" + spec)


def tenjin_multitool_toolchain_specifier() -> str:
    spec = provisioning.HAVE.query("10j-xj-improve-multitool-toolchain")
    assert spec is not None, "10j-xj-improve-multitool-toolchain should have been provisioned!"
    return os.environ.get("XJ_CARGO_TOOLCHAIN_SPEC", "+" + spec)


def get_toolchain_for_directory(dir: Path) -> str:
    try:
        with open(dir / "rust-toolchain.toml", "rb") as f:
            toolchain_dict = tomllib.load(f)
            return "+" + toolchain_dict["toolchain"]["channel"]
    except FileNotFoundError:
        # If no rust-toolchain.toml is found, we use Tenjin's default toolchain.
        return tenjin_cargo_toolchain_specifier()


def implicit_cargo_toolchain_arg(cwd: Path, args: Sequence[str]) -> list[str]:
    if args and len(args) > 0 and args[0].startswith("+"):
        # If the first argument is a toolchain specifier, we don't need
        # to add our own.
        return []

    if cwd.name == "c2rust":
        # Special-case c2rust, which we want to always consistently override.
        return [tenjin_cargo_toolchain_specifier()]

    assert cwd.is_dir(), f"cwd {cwd} is not a directory"
    # Without an explicit toolchain, try using the directory's toolchain, if one is
    # specified, but fall back to Tenjin's default toolchain rather than rustup's.
    return [get_toolchain_for_directory(cwd)]


def cargo_encoded_rustflags_env_ext() -> dict:
    # We need this to get Cargo to build executables and tests (which, on
    # macOS, end up linking to libclang-cpp.dylib) with an embedded rpath
    # entry that allows the running binary to find our LLVM library.
    #
    # For executables that we control the invocation of, we could use
    # LD_LIBRARY_PATH or similar, but for tests it's awkward because cargo
    # does the build and run all in one step. The downside of what we do here
    # is that the binaries are not relocatable between machines, which will
    # have differing paths for repo_root.localdir().
    #
    # Per https://doc.rust-lang.org/cargo/reference/config.html#buildrustflags
    # we cannot reliably use --config because RUSTFLAGS takes precedence and
    # settings are not merged. So we look up the value of RUSTFLAGS, if any,
    # and add it to CARGO_ENCODED_RUSTFLAGS, which takes precedence over
    # RUSTFLAGS itself.
    llvm_lib_dir = xj_llvm_root(repo_root.localdir()) / "lib"

    rustflags = os.environ.get("RUSTFLAGS", "")
    rustflags_parts = rustflags.split()
    rustflags_parts.extend(["-C", f"link-args=-Wl,-rpath,{llvm_lib_dir}"])
    return {
        "CARGO_ENCODED_RUSTFLAGS": b"\x1f".join(x.encode("utf-8") for x in rustflags_parts),
    }


def run_cargo_in(
    args: Sequence[str],
    cwd: Path,
    env_ext=None,
    check=True,
    **kwargs,
) -> subprocess.CompletedProcess:
    provisioning.want_10j_rust_toolchains()

    if not env_ext:
        env_ext = {}

    return run(
        ["cargo", *implicit_cargo_toolchain_arg(cwd, args), *args],
        cwd=cwd,
        check=check,
        with_tenjin_deps=True,
        env_ext={**env_ext, **cargo_encoded_rustflags_env_ext()},
        **kwargs,
    )


def opamroot(localdir: Path) -> Path:
    return localdir / "opamroot"


def running_in_ci() -> bool:
    return os.environ.get("CI") in ("true", "1")


def opam_non_hermetic() -> bool:
    """If we're running in CI and opam is installed, we should use it.

    Note that we don't do any version checks; we're assuming that CI is
    set up to use a version of opam that is either known to be compatible,
    or that we want to test the compatibility of.
    """
    return running_in_ci() and shutil.which("opam") is not None


def run_opam(
    args: list[str],
    eval_opam_env=True,
    with_tenjin_deps=True,
    check=False,
    env_ext=None,
    **kwargs,
) -> subprocess.CompletedProcess:
    localdir = repo_root.localdir()
    localopam = localdir / "opam"

    def insert_opam_subcmd_args(args: list[str], subcmd_args: list[str]) -> list[str]:
        match args:
            case []:
                return subcmd_args
            case [subcmd, *rest]:
                # If args is something like ["exec", "--", "dune"], we need to make
                # sure the subcmd args come before the double dash, otherwise we'll
                # pass them to `dune` instead of `opam exec`!
                return [subcmd, *subcmd_args, *rest]
            case _:
                raise ValueError("Invalid args for opam command")

    def mk_shell_cmd() -> str:
        def shell_cmd(parts: list[str]) -> str:
            return " ".join(str(x) for x in parts)

        hermetic = not opam_non_hermetic()

        opam_subcmd_args = ["--cli=2.3"]
        if hermetic:
            opam_subcmd_args += ["--root", str(opamroot(localdir))]

        maincmd = shell_cmd([str(localopam), *insert_opam_subcmd_args(args, opam_subcmd_args)])

        if eval_opam_env:
            opam_env_cmd = f"{localopam} env {shell_cmd(opam_subcmd_args)}"
            opam_env_cmd += " --switch=tenjin --set-switch --set-root"

            return f"eval $({opam_env_cmd}) && {maincmd}"
        else:
            return maincmd

    # Opam's warnings about running as root aren't particularly actionable.
    if not env_ext:
        env_ext = {}
    if "OPAMROOTISOK" not in env_ext:
        env_ext["OPAMROOTISOK"] = "1"

    # See COMMENTARY(goblint-cil-gcc-wrapper)
    path_elts = [str(xj_llvm_root(localdir) / "goblint-sadness")]
    # If PATH is in env_ext, it is assumed to be a full PATH, not a delta.
    if "PATH" in env_ext:
        path_elts.append(env_ext["PATH"])
    else:
        path_elts.append(os.environ["PATH"])
    env_ext["PATH"] = os.pathsep.join(path_elts)

    return run_shell_cmd(mk_shell_cmd(), check, with_tenjin_deps, env_ext, **kwargs)


def check_call_opam(
    args: list[str], eval_opam_env=True, with_tenjin_deps=True, **kwargs
) -> subprocess.CompletedProcess:
    cp = run_opam(args, eval_opam_env, with_tenjin_deps, check=False, **kwargs)
    if cp.stderr:
        click.echo(cp.stderr, err=True)
    cp.check_returncode()
    return cp


def run_output_git(args: list[str], check=False) -> bytes:
    jjdir = repo_root.find_repo_root_dir_Path() / ".jj"
    if jjdir.is_dir():
        gitroot = subprocess.check_output(["jj", "git", "root"]).decode("utf-8")
        cp = subprocess.run(["git", "--git-root", gitroot, *args], check=False, capture_output=True)
    else:
        cp = subprocess.run(["git", *args], check=False, capture_output=True)

    if cp.stderr:
        click.echo(cp.stderr, err=True)
    if check:
        cp.check_returncode()
    return cp.stdout


def check_output_git(args: list[str]):
    return run_output_git(args, check=True)
