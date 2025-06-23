import subprocess
import shlex
import shutil
import time
from pathlib import Path
import os
from typing import Sequence

import click

import repo_root
import provisioning


def check_call_uv(args: Sequence[str | os.PathLike[str]]) -> None:
    # The args here should be kept in sync with the 10j script.
    localdir = repo_root.localdir()
    subprocess.check_call([localdir / "uv", "--config-file", localdir / "uv.toml", *args])


def xj_build_deps(localdir: Path) -> Path:
    return localdir / "xj-build-deps"


def xj_llvm_root(localdir: Path) -> Path:
    return localdir / "xj-llvm"


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
            str(xj_llvm_root(localdir) / "bin"),
            str(localdir / "cmake" / "bin"),
            env["PATH"],
        ])

    return env


def run_command_with_progress(command, stdout_file, stderr_file, shell=False) -> None:
    """
    Run a command, redirecting stdout/stderr to files, and print dots while waiting.
    """
    if os.environ.get("XJ_SHOW_CMDS", "0") != "0":
        click.echo(f": {command}")

    with open(stdout_file, "wb") as out_f, open(stderr_file, "wb") as err_f:
        proc = subprocess.Popen(
            command,
            stdout=out_f,
            stderr=err_f,
            bufsize=1,
            shell=shell,
            env=mk_env_for(repo_root.localdir(), with_tenjin_deps=True, env_ext=None),
        )

        while proc.poll() is None:
            # Process is still running
            print(".", end="", flush=True)
            time.sleep(0.3)

        # Final newline after progress dots
        print()
        assert proc.returncode == 0, f"Command failed with return code {proc.returncode}"


type RunSpec = str | Sequence[str | bytes | os.PathLike[str] | os.PathLike[bytes]]


def run(
    cmd: RunSpec, check=False, with_tenjin_deps=True, env_ext=None, **kwargs
) -> subprocess.CompletedProcess:
    if os.environ.get("XJ_SHOW_CMDS", "0") != "0":
        click.echo(f": {cmd}")

    return subprocess.run(
        cmd,
        check=check,
        env=mk_env_for(repo_root.localdir(), with_tenjin_deps, env_ext),
        **kwargs,
    )


def run_shell_cmd(
    cmd: RunSpec, check=False, with_tenjin_deps=True, env_ext=None, **kwargs
) -> subprocess.CompletedProcess:
    if not isinstance(cmd, str):
        cmd = " ".join(shlex.quote(str(x)) for x in cmd)

    return run(
        cmd, check=check, with_tenjin_deps=with_tenjin_deps, env_ext=env_ext, shell=True, **kwargs
    )


def cargo_toolchain_specifier() -> str:
    return "+stable"


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
    args: Sequence[str], cwd: Path | None, check=True, **kwargs
) -> subprocess.CompletedProcess:
    return run(
        ["cargo", cargo_toolchain_specifier(), *args],
        cwd=cwd,
        check=check,
        with_tenjin_deps=True,
        env_ext=cargo_encoded_rustflags_env_ext(),
        **kwargs,
    )


def opamroot(localdir: Path) -> Path:
    return localdir / "opamroot"


def running_in_ci() -> bool:
    return os.environ.get("CI") in ("true", "1")


def opam_non_hermetic() -> bool:
    """If we're running in CI and opam is installed, we should use it.

    Note that we don't do any version checks; we're assuming that CI is
    set up to use a version opam that is either known to be compatible,
    or that we want to test the compatibility of.
    """
    return running_in_ci() and shutil.which("opam") is not None


def run_opam(
    args: list[str],
    eval_opam_env=True,
    with_tenjin_deps=True,
    check=False,
    env_ext=None,
    suppress_provisioning_check=False,
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
        else:
            # We save about four minutes per run in CI by using the system opam.
            # If it appears to be installed, use it.
            if not suppress_provisioning_check:
                provisioning.want_opam()

        if not suppress_provisioning_check:
            # Provisioning needs to check Dune's version, which is done via
            # run_opam(["exec", "--", "dune", "--version"]), so we need to
            # break the recursion.
            match args:
                case ["exec", "--", "dune", *_rest]:
                    provisioning.want_dune()
                case _:
                    pass

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
