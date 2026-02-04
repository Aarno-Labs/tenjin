from pathlib import Path
import platform
import os
import tempfile
import tarfile
import shutil
import subprocess
from urllib.parse import urlparse
from typing import Protocol
import json
import enum
import sys
import textwrap
import zipfile

from packaging.version import Version
import click

import repo_root
import hermetic
from sha256sum import compute_sha256
from constants import WANT, SYSROOT_NAME


class InstallationState(enum.Enum):
    NOT_INSTALLED = 0
    VERSION_OK = 1
    VERSION_MISMATCH = 2


class TrackingWhatWeHave:
    def __init__(self):
        self.localdir = repo_root.localdir()

        # This must be set in any code path that may call back to hermetic.run(), etc.
        # provision_desires() sets this, and individual calls to want_*() must do so as well.
        self.provisioning_depth = 0
        try:
            with open(Path(self.localdir, "config.10j-HAVE.json"), "r", encoding="utf-8") as f:
                self._have = json.load(f)
        except OSError:
            self._have = {}

    def save(self):
        with open(Path(self.localdir, "config.10j-HAVE.json"), "w", encoding="utf-8") as f:
            json.dump(self._have, f, indent=2, sort_keys=True)

    def note_we_have(self, name: str, version: Version | None = None, specifier: str | None = None):
        match [version is None, specifier is None]:
            case [True, True]:
                raise ValueError(f"For '{name}' must provide either version or specifier")
            case [False, False]:
                raise ValueError(f"For '{name}' must provide either version or specifier, not both")
            case _:
                pass

        had = self._have.get(name)
        now = str(version) if version else specifier
        self._have[name] = now
        if had != now:
            self.save()

    def note_removed(self, name: str):
        if name in self._have:
            del self._have[name]
        self.save()

    def query(self, name: str) -> str | None:
        return self._have.get(name)

    def compatible(self, name: str) -> InstallationState:
        assert name in WANT, f"Unknown wanted item '{name}'"
        wanted_spec: str = WANT[name]

        if name not in self._have:
            return InstallationState.NOT_INSTALLED

        if self._have[name] == wanted_spec:
            return InstallationState.VERSION_OK

        return InstallationState.VERSION_MISMATCH


HAVE = TrackingWhatWeHave()


class ProvisioningError(Exception):
    pass


def sez(msg: str, ctx: str, err=False):
    click.echo("TENJIN SEZ: " + ctx + msg, err=err)


def download(url: str, filename: Path, first_attempt=True) -> None:
    # This import is relatively expensive (20 ms) and is rarely needed,
    # so it is imported here to avoid slowing down the common case.
    from urllib.request import urlretrieve  # noqa: PLC0415
    from urllib.error import HTTPError  # noqa: PLC0415
    from http.client import RemoteDisconnected  # noqa: PLC0415

    def report_potentially_transient_problem_and_retry():
        sez(
            "Hopefully this is a temporary issue and will resolve itself.",
            ctx="(download) ",
            err=True,
        )
        sez("I will wait for a few seconds then re-try, once.", ctx="(download) ", err=True)
        import time  # noqa: PLC0415

        time.sleep(6)
        download(url, filename, first_attempt=False)

    def report_insurmountable_error_and_die(e: Exception):
        sez(f"Failed to download {url}: {e}", ctx="(download) ", err=True)
        sez(
            "You might try deleting the `_local` directory and re-provisioning?",
            ctx="(download) ",
            err=True,
        )
        sys.exit(1)

    try:
        urlretrieve(url, filename)
    except HTTPError as e:
        if first_attempt and e.code in (500, 502, 503, 504):
            # These are HTTP error codes for (at least potentially) transient issues.
            sez(f"Server returned error {e} when downloading {url}", ctx="(download) ", err=True)
            report_potentially_transient_problem_and_retry()
            return

        report_insurmountable_error_and_die(e)
    except RemoteDisconnected as e:
        if first_attempt:
            sez(
                f"Server closed connection unexpectedly when downloading {url}",
                ctx="(download) ",
                err=True,
            )
            report_potentially_transient_problem_and_retry()
            return

        report_insurmountable_error_and_die(e)


# platform.system() in ["Linux", "Darwin"]


def clone_or_fetch_git_repo(
    repo_url: str,
    target_dir: Path,
    version: str,
    log_prefix: str,
    display_name: str,
    ctx: str,
) -> None:
    """Clone a git repo if not present, or fetch and reset to the given version.

    Args:
        repo_url: The URL of the git repository to clone.
        target_dir: The directory to clone into or update.
        version: The git ref (tag, branch, commit) to checkout.
        log_prefix: Prefix for log files, e.g., "xj-c2rust" -> "xj-c2rust-clone.log".
        display_name: Human-readable name for messages, e.g., "C2Rust".
        ctx: Context string for sez(), e.g., "(c2rust) ".
    """
    localdir = HAVE.localdir
    if target_dir.is_dir():
        sez(f"Fetching and resetting {display_name} to version {version} ...", ctx)
        subprocess.check_call(["git", "fetch", "--all"], cwd=str(target_dir))
        subprocess.check_call(["git", "switch", "--detach", version], cwd=str(target_dir))
    else:
        sez(f"Cloning {display_name} {version} ...", ctx)
        stdout_path = Path(localdir, f"{log_prefix}-clone.log")
        stderr_path = Path(localdir, f"{log_prefix}-clone.err")
        hermetic.run_command_with_progress(
            ["git", "clone", repo_url, str(target_dir)],
            stdout_file=stdout_path,
            stderr_file=stderr_path,
        )
        subprocess.check_call(["git", "switch", "--detach", version], cwd=str(target_dir))


def query_git_head(repo_dir: Path) -> str | None:
    """Return the current HEAD commit hash of the given git repository."""
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=str(repo_dir),
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


# See https://stackoverflow.com/questions/45125516/possible-values-for-uname-m
# See https://gist.github.com/skyzyx/d82b7d9ba05523dd1a9301fd282b32c4
def machine_normalized(aarch64="aarch64") -> str:
    x: dict[str, str] = {}
    for src in "arm64 arm64v8 arm64v9 armv8b armv8l aarch64_be aarch64".split():
        x[src] = aarch64

    for src in "amd64 AMD64 x64 x86_64".split():
        x[src] = "x86_64"

    m = platform.machine()
    return x.get(m, m)


def provision_desires(wanted: str):
    """This is the main entry point for provisioning.

    It is designed to be very fast when nothing needs updating:
    `provision_desires("all")` takes less than 1 ms.
    """
    if wanted == "uv":
        return
    assert wanted in "all llvm ocaml rust".split()
    assert HAVE.provisioning_depth == 0, "Re-provisioning loop detected!"
    HAVE.provisioning_depth += 1

    # Unlike the other stuff, we won't provision rustup ourselves,
    # so if it's not available, we should inform the user ASAP.
    if wanted in ("all", "rust"):
        require_rustup()

    # First time install?
    if wanted == "all" and HAVE.query("10j-reference-c2rust-tag") is None:

        def say(msg: str):
            sez(msg, ctx="(overall-provisioning) ")

        say(f"Provisioning local directory {HAVE.localdir}...")
        say("This involves downloading and extracting a few hundred megs of tarballs:")
        say("    Clang+LLVM, a sysroot, and misc build tools like CMake.")
        say("We'll also install Rust and OCaml, which will take a few minutes...")

    # We get these unconditionally, because both Rust and OCaml (and/or the
    # projects in those languages) end up needing them.
    want_10j_deps()
    want_10j_llvm()
    want_cmake()
    want_10j_more_deps()
    want_10j_ast_grep()

    if wanted in ("all", "rust"):
        want_10j_rust_toolchains()

    if wanted in ("all", "ocaml"):
        want_dune()
        want_codehawk_c()

    if wanted == "all":
        want_10j_reference_c2rust_tag()

    HAVE.provisioning_depth -= 1


def require_rustup():
    def say(msg: str, fg: str = ""):
        if fg:
            msg = click.style(msg, fg=fg)
        sez(msg, ctx="(rust) ")

    rustup_installer = "rustup-installer.sh"

    # We don't run the installer ourselves because rustup pretty much requires
    # PATH modifications, and it's not our place to do that. In theory we could
    # have a hermetic copy of rustup + cargo etc but it seems silly because (A)
    # rustup is already hermetic enough, and (B) if someone is using Tenjin to
    # translate C to Rust, why on earth would they avoid having Rust installed?
    # Also one of TRACTOR's requirements is that translated Rust code works with
    # stable Rust, so pinning to a specific version of Rust would only result in
    # us not learning about bugs we really need to fix.
    #
    # HOWEVER: note that on a machine where only Tenjin's C compiler is available,
    # any cargo command that leads to compilation (cargo build, for many projects,
    # and also things like `rustup +nightly component add miri`, always) must be
    # run via 10j.
    def complain_about_tool_then_die(tool: str):
        say(f"{tool} is not installed, or is not available on your $PATH", fg="red")
        match platform.system():
            case "Linux":
                say("Please install Rust using rustup (or via your package manager).")
            case "Darwin":
                say("Please install Rust using rustup (or via Homebrew).")
            case sysname:
                say(f"Tenjin doesn't yet support {sysname}, sorry!", fg="red")
                sys.exit(1)

        download("https://sh.rustup.rs", Path(rustup_installer))
        subprocess.check_call(["chmod", "+x", rustup_installer])

        say("")
        say("For your convenience, I've downloaded the rustup installer script,")
        say("so you can just run")
        say(click.style(f"                  ./{rustup_installer}", bold=True))
        say("")
        say("It will interactively prompt you for the details of how and where")
        say("to install Rust. Most people choose the default options.")
        say("")
        say("Once you can run `cargo --version`, please re-run provisioning", fg="green")
        sys.exit(1)

    if shutil.which("rustc") is None:
        complain_about_tool_then_die("Rust")
    if shutil.which("cargo") is None:
        complain_about_tool_then_die("cargo")
    if shutil.which("rustup") is None:
        complain_about_tool_then_die("rustup")

    # At this point, the installer's job is done.
    if Path(rustup_installer).is_file():
        os.remove(rustup_installer)


class Provisioner(Protocol):
    def __call__(self, version: str, keyname: str) -> None:
        """Provision the given version of the software into the given localdir."""


def want(
    keyname: str,
    lowername: str,
    titlename: str,
    provisioner: Provisioner,
):
    match HAVE.compatible(keyname):
        case InstallationState.VERSION_OK:
            return
        case InstallationState.VERSION_MISMATCH:
            sez(f"{titlename} version is outdated; re-provisioning...", ctx=f"({lowername}) ")
            provisioner(version=WANT[keyname], keyname=keyname)
        case InstallationState.NOT_INSTALLED:
            provisioner(version=WANT[keyname], keyname=keyname)


def want_cmake() -> None:
    want("10j-cmake", "cmake", "CMake", provision_cmake_with)


def want_dune():
    want("10j-dune", "dune", "Dune", provision_dune_with)


def want_opam():
    want("10j-opam", "opam", "opam", provision_opam_with)


def want_ocaml():
    want("10j-ocaml", "ocaml", "OCaml", provision_ocaml_with)


def xj_llvm_dir(keyname: str) -> Path:
    if keyname == "10j-llvm14":
        return hermetic.xj_llvm14_root(HAVE.localdir)
    return hermetic.xj_llvm_root(HAVE.localdir)


def want_10j_llvm():
    for keyname in ["10j-llvm", "10j-llvm14"]:
        want(keyname, "llvm", "LLVM", provision_10j_llvm_with)
        want_10j_sysroot_extras(xj_llvm_dir(keyname))


def want_10j_rust_toolchains():
    """This must not lead back to hermetic.common_helper_for_run()."""
    want("10j-xj-improve-multitool-toolchain", "rust", "Rust", provision_10j_rust_toolchain_with)
    want("10j-xj-default-rust-toolchain", "rust", "Rust", provision_10j_rust_toolchain_with)


def want_10j_reference_c2rust_tag():
    def provision_10j_reference_c2rust_source_with(
        version: str,
        xj_upstream_c2rust: Path,
    ):
        clone_or_fetch_git_repo(
            repo_url="https://github.com/immunant/c2rust.git",
            target_dir=xj_upstream_c2rust,
            version=version,
            log_prefix="xj-c2rust",
            display_name="C2Rust",
            ctx="(c2rust) ",
        )

    def provision_10j_reference_c2rust_tag_with(
        version: str,
        keyname: str,
    ):
        xj_upstream_c2rust = hermetic.xj_upstream_c2rust(HAVE.localdir)

        if (
            hermetic.running_in_ci()
            and xj_upstream_c2rust.is_dir()
            and query_git_head(xj_upstream_c2rust) == version
        ):
            sez("Upstream c2rust restored from CI cache...", ctx="(c2rust) ")
        else:
            provision_10j_reference_c2rust_source_with(version, xj_upstream_c2rust)
            rebuild_10j_upstream_c2rust(xj_upstream_c2rust)

        HAVE.note_we_have(keyname, specifier=version)

    want(
        "10j-reference-c2rust-tag",
        "c2rust",
        "Upstream C2Rust",
        provision_10j_reference_c2rust_tag_with,
    )


def rebuild_10j_upstream_c2rust(xj_upstream_c2rust: Path):
    stdout_path = Path(xj_upstream_c2rust, "xj-c2rust-build.log")
    stderr_path = Path(xj_upstream_c2rust, "xj-c2rust-build.err")

    sez("Building upstream C2Rust...", ctx="(c2rust) ")
    hermetic.run_command_with_progress(
        [
            "cargo",
            hermetic.tenjin_cargo_toolchain_specifier(),
            "build",
            "--locked",
            "-p",
            "c2rust",
            "-p",
            "c2rust-transpile",
        ],
        stdout_file=stdout_path,
        stderr_file=stderr_path,
        cwd=xj_upstream_c2rust,
        env_ext=hermetic.cargo_encoded_rustflags_env_ext(None),
    )
    # Ensure a clean checkout for future updates
    stdout_path.unlink(missing_ok=True)
    stderr_path.unlink(missing_ok=True)


def want_codehawk():
    def rebuild_codehawk(xj_codehawk: Path):
        sez("Updating CodeHawk dependencies...", ctx="(codehawk) ")

        # Start by synchronizing Opam's database, just in case CodeHawk has a new
        # dependency which wasn't in the previous cache.
        hermetic.run_opam(["update"], check=False)

        hermetic.run_opam(
            [
                "install",
                "--assume-depexts",
                "--deps-only",
                "--yes",
                str(xj_codehawk / "CodeHawk" / "codehawk.opam"),
            ],
            cwd=xj_codehawk,
            check=True,
        )

        sez("Building CodeHawk...", ctx="(codehawk) ")
        hermetic.run_opam(
            [
                "exec",
                "--",
                "dune",
                "build",
                "@install",
            ],
            check=True,
            cwd=xj_codehawk / "CodeHawk",
        )

    def provision_codehawk_source_with(
        version: str,
        xj_codehawk: Path,
    ):
        clone_or_fetch_git_repo(
            repo_url="https://github.com/static-analysis-engineering/codehawk.git",
            target_dir=xj_codehawk,
            version=version,
            log_prefix="xj-codehawk",
            display_name="CodeHawk",
            ctx="(codehawk) ",
        )

    def provision_codehawk_with(
        version: str,
        keyname: str,
    ):
        xj_codehawk = hermetic.xj_codehawk(HAVE.localdir)
        if (
            hermetic.running_in_ci()
            and xj_codehawk.is_dir()
            and query_git_head(xj_codehawk) == version
        ):
            sez("CodeHawk restored from CI cache...", ctx="(codehawk) ")
        else:
            provision_codehawk_source_with(version, xj_codehawk)
            rebuild_codehawk(xj_codehawk)
            # If we update the pinned CodeHawk hash but not CodeHawk-C's,
            # we must re-synchronize here or CodeHawk-C will use stale binaries.
            try_sync_codehawk_artifacts_into_codehawk_c()

        HAVE.note_we_have(keyname, specifier=version)

    want(
        "10j-codehawk",
        "codehawk",
        "CodeHawk",
        provision_codehawk_with,
    )


def try_sync_codehawk_artifacts_into_codehawk_c():
    def copy_and_make_executable(src: Path, dst: Path):
        shutil.copyfile(src, dst)
        dst.chmod(0o755)

    ch_os_name = "linux" if platform.system() == "Linux" else "macOS"
    destdir = hermetic.xj_codehawk_c(HAVE.localdir) / "chc" / "bin" / ch_os_name
    if not destdir.is_dir():
        return

    ch_build_dir = hermetic.xj_codehawk(HAVE.localdir) / "CodeHawk" / "_build"
    ch_bin_dir = ch_build_dir / "install" / "default" / "bin"

    copy_and_make_executable(ch_bin_dir / "canalyzer", destdir / "canalyzer")
    copy_and_make_executable(ch_bin_dir / "parseFile", destdir / "parseFile")


def want_codehawk_c():
    want_codehawk()

    def rebuild_codehawk_c():
        try_sync_codehawk_artifacts_into_codehawk_c()

    def provision_codehawk_c_source_with(
        version: str,
        xj_codehawk: Path,
    ):
        clone_or_fetch_git_repo(
            repo_url="https://github.com/static-analysis-engineering/CodeHawk-C.git",
            target_dir=xj_codehawk,
            version=version,
            log_prefix="xj-codehawk-c",
            display_name="CodeHawk-C",
            ctx="(codehawk) ",
        )

    def provision_codehawk_c_with(
        version: str,
        keyname: str,
    ):
        xj_codehawk = hermetic.xj_codehawk_c(HAVE.localdir)
        provision_codehawk_c_source_with(version, xj_codehawk)
        rebuild_codehawk_c()

        HAVE.note_we_have(keyname, specifier=version)

    want(
        "10j-codehawk-c",
        "codehawk-c",
        "CodeHawk-C",
        provision_codehawk_c_with,
    )


def want_10j_sysroot_extras(xj_llvm_root: Path):
    if platform.system() != "Linux":
        return

    def provision_10j_sysroot_extras_with(
        version: str,
        keyname: str,
    ):
        filename = f"xj-bullseye-sysroot-extras_{machine_normalized()}.tar.xz"
        url = f"https://github.com/Aarno-Labs/tenjin-build-deps/releases/download/{version}/{filename}"

        tarball = xj_llvm_root / filename
        download(url, tarball)

        tmp_dest = xj_llvm_root / "tmp"
        tmp_dest.mkdir()

        shutil.unpack_archive(tarball, tmp_dest, filter="tar")
        tarball.unlink()

        # We use non-normalized platform.machine() here because we want to match
        # the (non-normalized) Linux native naming convention.
        triple = f"{platform.machine()}-linux-gnu"
        shutil.copytree(
            tmp_dest / "debian-bullseye_gcc_glibc" / machine_normalized() / "usr_lib",
            xj_llvm_root / "sysroot" / "usr" / "lib" / triple,
            dirs_exist_ok=True,
        )

        # We need the .a files to enable static linking for our hermetic clang.
        shutil.copytree(
            tmp_dest / "debian-bullseye_gcc_glibc" / machine_normalized() / "usr_lib_gcc",
            xj_llvm_root / "sysroot" / "usr" / "lib" / "gcc" / triple / "10",
            dirs_exist_ok=True,
        )

        shutil.rmtree(tmp_dest)

        HAVE.note_we_have(keyname, specifier=version)

    want(
        "10j-bullseye-sysroot-extras",
        "sysroot-extras",
        "sysroot-extras",
        provision_10j_sysroot_extras_with,
    )


def want_10j_deps():
    want(
        "10j-build-deps",
        "10j-build-deps",
        "Tenjin build deps",
        provision_10j_deps_with,
    )


def want_10j_more_deps():
    def provision_10j_more_deps_with(version: str, keyname: str):
        loweros = {"Linux": "linux", "Darwin": "macos"}[platform.system()]
        filename = f"xj-more-deps_{loweros}-{machine_normalized()}.tar.xz"
        url = f"https://github.com/Aarno-Labs/tenjin-build-deps/releases/download/{version}/{filename}"
        target = hermetic.xj_more_deps(HAVE.localdir)
        if target.is_dir():
            shutil.rmtree(target)
        download_and_extract_tarball(url, target, ctx="(builddeps) ")

        if platform.system() == "Darwin":
            subprocess.check_call([
                "install_name_tool",
                "-add_rpath",
                "@executable_path/../../xj-llvm-14/lib",
                str(target / "bin" / "cc2json-llvm14"),
            ])

            subprocess.check_call([
                "install_name_tool",
                "-id",
                str(target / "gmp-6.3.0" / "lib" / "libgmp.10.dylib"),
                str(target / "gmp-6.3.0" / "lib" / "libgmp.10.dylib"),
            ])

        z3_pc = target / "lib" / "pkgconfig" / "z3.pc"
        if z3_pc.is_file():
            lines = z3_pc.read_text(encoding="utf-8").splitlines()
            lines[0] = f"prefix={target}"
            lines[1] = "exec_prefix=${prefix}"
            z3_pc.write_text("\n".join(lines), encoding="utf-8")

        gmp_dir = hermetic.xj_gmp_root(HAVE.localdir)
        gmp_files_to_cook = [
            gmp_dir / "lib" / "pkgconfig" / "gmp.pc",
            gmp_dir / "lib" / "libgmp.la",
        ]
        for f in gmp_files_to_cook:
            if f.is_file():
                lines = f.read_text(encoding="utf-8").splitlines()
                lines[0] = f"prefix={gmp_dir}"
                f.write_text("\n".join(lines), encoding="utf-8")

        HAVE.note_we_have(keyname, specifier=version)

    want(
        "10j-more-deps",
        "10j-more-deps",
        "Tenjin more deps",
        provision_10j_more_deps_with,
    )


def want_10j_ast_grep():
    want("10j-ast-grep", "ast-grep", "ast-grep", provision_10j_ast_grep_with)


def provision_10j_rust_toolchain_with(version: str, keyname: str):
    """This must not lead back to hermetic.common_helper_for_run()."""

    # Examples of expected toolchain specs: "1.88.0" or "nightly-2025-08-20".
    # Specifying the point release helps avoid redundant downloads. Observe:
    #     $ docker run --rm -it rust:1.88-alpine rustc +1.88.0 --version
    #     rustc 1.88.0 (6b00bc388 2025-06-23)
    #     $ docker run --rm -it rust:1.88-alpine rustc +1.88 --version
    #     info: syncing channel updates for '1.88-x86_64-unknown-linux-musl'
    #     ...
    #     $ docker run --rm -it rust:1.88-alpine rustup toolchain list
    #     1.88.0-x86_64-unknown-linux-musl (active, default)
    toolchain_spec = version
    assert not toolchain_spec.startswith("+")
    assert not toolchain_spec == "stable"

    def say(msg: str):
        sez(msg, ctx="(rust) ")

    # The rustup.sh installer defaults to the 'default' profile, which provides clippy,
    # but the 'minimal' profile (as used in official Rust docker images) does not.
    # The easiest thing to do is unconditionally add the clippy component.
    # This will also download the requested toolchain if it is not already installed.
    cmd = ["rustup", "component", "add", "--toolchain", toolchain_spec, "clippy", "rustfmt"]
    if toolchain_spec.startswith("nightly"):
        cmd.append("rustc-dev")
        cmd.append("llvm-tools")

    say(f"Installing Rust toolchain {toolchain_spec}...")
    log_path = repo_root.localdir() / "rustup.log.txt"
    hermetic.run_command_with_progress(
        cmd, stdout_file=log_path, stderr_file=log_path.with_suffix(".err"), suppress_helper=True
    )

    HAVE.note_we_have(keyname, specifier=toolchain_spec)


def grab_opam_stdout(args: list[str]) -> str:
    cp = hermetic.run_opam(
        args,
        check=True,
        capture_output=True,
    )
    return cp.stdout.decode("utf-8").strip()


# Prerequisite: opam provisioned.
def grab_opam_version_str() -> str:
    return grab_opam_stdout(["--version"])


# Prerequisite: opam and ocaml provisioned.
def grab_ocaml_version_str() -> str:
    return grab_opam_stdout(["exec", "--", "ocamlc", "--version"])


# Prerequisite: opam and dune provisioned.
def grab_dune_version_str() -> str:
    return grab_opam_stdout(["exec", "--", "dune", "--version"])


def provision_ocaml_with(version: str, keyname: str):
    provision_ocaml(version)

    HAVE.note_we_have(keyname, version=Version(version))

    hermetic.run_opam(["config", "report"], check=False)

    if Version(grab_ocaml_version_str()) != Version(version):
        raise ProvisioningError(
            f"Expected OCaml version {version}, got {grab_ocaml_version_str()}."
        )


def provision_ocaml(ocaml_version: str):
    want_opam()

    def say(msg: str):
        sez(msg, ctx="(ocaml) ")

    TENJIN_SWITCH = "tenjin"

    def install_ocaml():
        if not hermetic.opam_non_hermetic():
            # For hermetic installations, we will simply bulldoze the existing
            # opam root and start fresh. For non-hermetic installations, we'll
            # try to reuse what's already there.
            opamroot = HAVE.localdir / "opamroot"
            if opamroot.is_dir():
                shutil.rmtree(opamroot)

        sandboxing_arg = infer_bwrap_sandboxing_args()

        cp = hermetic.run_opam(["config", "report"], eval_opam_env=False, capture_output=True)
        if b"please run `opam init'" in cp.stderr:
            say("================================================================")
            say("Initializing opam; this will take about half a minute...")
            say("      (subsequent output comes from `opam init --bare`)")
            say("----------------------------------------------------------------")
            say("")
            hermetic.check_call_opam(
                ["init", "--bare", "--no-setup", "--disable-completion", *sandboxing_arg],
                eval_opam_env=False,
            )

        cp = hermetic.run_opam(
            ["switch", "list"], eval_opam_env=False, check=True, capture_output=True
        )
        if TENJIN_SWITCH in cp.stdout.decode("utf-8"):
            if grab_ocaml_version_str() == ocaml_version:
                say("================================================================")
                say("Reusing cached OCaml, saving a few minutes of compiling...")
                say("----------------------------------------------------------------")
                return
            else:
                say("Removing cached OCaml switch due to version mismatch.")
                say(f"  had {grab_ocaml_version_str()}, want {ocaml_version}.")
                hermetic.check_call_opam(
                    ["switch", "remove", TENJIN_SWITCH, "-y"], eval_opam_env=False
                )

        say("")
        say("================================================================")
        say("Installing OCaml; this will take four-ish minutes to compile...")
        say("      (subsequent output comes from `opam switch create`)")
        say("----------------------------------------------------------------")

        hermetic.check_call_opam(
            ["switch", "create", TENJIN_SWITCH, ocaml_version, "--no-switch"],
            eval_opam_env=False,
            env_ext={
                "OPAMNOENVNOTICE": "1",
                "CC": str(hermetic.xj_llvm_root(HAVE.localdir) / "bin" / "clang"),
                "CXX": str(hermetic.xj_llvm_root(HAVE.localdir) / "bin" / "clang++"),
            },
        )

    install_ocaml()


def provision_debian_bullseye_sysroot_with(dest_sysroot: Path):
    def say(msg: str):
        sez(msg, ctx="(sysroot) ")

    say("Downloading and unpacking sysroot tarball, will take maybe 10 s...")

    CHROME_LINUX_SYSROOT_URL = "https://commondatastorage.googleapis.com/chrome-linux-sysroot"

    # These don't go in WANT because they're quite stable;
    # we don't expect to need a new version, ever.
    DEBIAN_BULLSEYE_SYSROOT_SHA256SUMS = {
        "x86_64": "dec7a3a0fc5b83b909cba1b6d119077e0429a138eadef6bf5a0f2e03b1904631",
        "aarch64": "308e23faba3174bd01accfe358467b8a40fad4db4c49ef629da30219f65a275f",
        "armhf": "fe81e7114b97440262bce004caf02c1514732e2fa7f99693b2836932ad1c4626",
    }
    tarball_sha256sum = DEBIAN_BULLSEYE_SYSROOT_SHA256SUMS[machine_normalized()]

    url = CHROME_LINUX_SYSROOT_URL + "/" + tarball_sha256sum

    if dest_sysroot.is_dir():
        shutil.rmtree(dest_sysroot)
    dest_sysroot.mkdir()
    tarball = dest_sysroot / "tenjin-sysroot.tar.xz"

    download(url, tarball)
    sha256sum = compute_sha256(tarball)
    if sha256sum != tarball_sha256sum:
        raise ProvisioningError("Sysroot hash verification failed!")
    shutil.unpack_archive(tarball, dest_sysroot, filter="tar")
    tarball.unlink()


def provision_opam_binary_with(opam_version: str) -> None:
    def say(msg: str):
        sez(msg, ctx="(opam) ")

    localdir = HAVE.localdir
    # If the system happens to have a copy of a suitable version of opam, grab it.
    sys_opam = shutil.which("opam")
    if sys_opam is not None:
        sys_opam_version = subprocess.check_output([sys_opam, "--version"]).decode("utf-8")
        if Version(sys_opam_version) >= Version(opam_version):
            say(f"Symlinking to a suitable version of opam at {sys_opam}")
            os.symlink(sys_opam, str(localdir / "opam"))
            return

    # Otherwise, we'll need to run the installer to get it.
    say("Downloading a local copy of opam...")
    installer_sh = localdir / "install-opam.sh"
    download("https://opam.ocaml.org/install.sh", installer_sh)
    if not installer_sh.is_file():
        raise ProvisioningError(f"Unable to download installer script for opam {opam_version}.")
    subprocess.check_call(["sh", installer_sh, "--download-only", "--version", opam_version])
    tagged = list(Path(".").glob(f"opam-{opam_version}-*"))
    assert len(tagged) == 1
    tagged_path = tagged[0]
    subprocess.check_call(["chmod", "+x", tagged_path])
    tagged_path.replace(localdir / "opam")

    if hermetic.running_in_ci():
        dotlocalbin = Path.home() / ".local" / "bin"
        if str(dotlocalbin) in os.environ["PATH"]:
            if not dotlocalbin.is_dir():
                dotlocalbin.mkdir(parents=True)

            # XREF:ci-opam-paths
            # We are in CI, but didn't have opam on the path already.
            # Install it where (A) nrsr.yaml will cache it and (B) it'll be on PATH.
            # Then our next CI run will be faster.
            shutil.copy(Path(localdir, "opam"), dotlocalbin / "opam")
        else:
            click.echo("WARNING: ~/.local/bin not on PATH anymore?!? OCaml cache won't work.")


def provision_dune_with(version: str, keyname: str):
    provision_dune(version)

    HAVE.note_we_have(keyname, version=Version(version))

    if Version(grab_dune_version_str()) != Version(version):
        raise ProvisioningError(f"Expected dune version {version}, got {grab_dune_version_str()}.")


def infer_bwrap_sandboxing_args() -> list[str]:
    if platform.system() != "Linux":
        return []  # bwrap is Linux-only

    bwrap_path = hermetic.xj_build_deps(HAVE.localdir) / "bin" / "bwrap"
    if not bwrap_path.is_file():
        return ["--disable-sandboxing"]  # no bwrap, no sandboxing

    # Bubblewrap does not work inside Docker containers, at least not without
    # heinous workarounds, if we're in Docker then we don't really need it anyway.
    # So we'll try running a trivial command with it; if it fails, we'll tell opam
    # not to use it.
    #
    # We really want to run a statically linked binary, since
    # for a dynamically linked binary we'd need to add symlinks
    # for whatever libraries it needs, which can be platform-dependent.
    # But which binaries are statically linked will vary by distro.
    # So we'll compile one ourselves.
    tru_c_src = "int main() { return 0; }"
    binary_path = Path.cwd() / "tru"

    with tempfile.NamedTemporaryFile(
        suffix=".c", mode="w", encoding="utf-8", delete=False
    ) as temp_c_file:
        temp_c_file.write(tru_c_src)
        temp_c_file_path = temp_c_file.name

    try:
        compile_cmd = ["clang", temp_c_file_path, "-static", "-o", str(binary_path)]
        hermetic.run_shell_cmd(compile_cmd, check=True)
        os.chmod(binary_path, 0o755)

        subprocess.check_call([bwrap_path, "--ro-bind", Path.cwd(), "/", "/tru"])
    except subprocess.CalledProcessError as e:
        print(f"TENJIN: Compilation of bubblewrap-able static binary failed with error: {e.stderr}")
    except Exception as e:
        print(f"An error occurred: {str(e)}")
    else:
        return []  # do not disable sandboxing
    finally:
        Path(temp_c_file_path).unlink(missing_ok=True)
        binary_path.unlink(missing_ok=True)

    # If we reach here, it means we didn't return on the happy path
    return ["--disable-sandboxing"]


# Precondition: not installed, or version too old.
def provision_dune(dune_version: str):
    want_ocaml()

    def say(msg: str):
        sez(msg, ctx="(opam) ")

    try:
        actual_version = Version(grab_dune_version_str())
        if actual_version == Version(dune_version):
            # We only get here when the HAVE cache is incorrect: it thinks dune
            # is not installed or is out of date, but dune is in fact installed
            # with the desired version.
            say(f"Dune {actual_version} is already installed.")
            return

        say(f"Found dune version {actual_version}, but we need {dune_version}.")
    except subprocess.CalledProcessError:
        # This is expected if we don't have dune installed yet.
        pass

    say("")
    say("================================================================")
    say("Installing Dune; this will take a minute to compile...")
    say("      (subsequent output comes from `opam install dune`)")
    say("----------------------------------------------------------------")

    try:
        # Try installing from opam registry first
        hermetic.check_call_opam(["pin", "add", "--no-action", "--yes", "dune", dune_version])
        hermetic.check_call_opam([
            "pin",
            "add",
            "--no-action",
            "--yes",
            "dune-configurator",
            dune_version,
        ])
        hermetic.check_call_opam(["install", "--yes", "dune", "dune-configurator"])
    except subprocess.CalledProcessError:
        say("Failed to install dune from opam registry.")
        provision_dune_from_source(dune_version, say)


def provision_dune_from_source(dune_version: str, say):
    """
    Download and install dune from source when opam registry installation fails.
    """
    # GitHub releases URL pattern
    say("Installing dune from source...")
    dune_git_url = f"git+https://github.com/ocaml/dune.git#{dune_version}"
    hermetic.check_call_opam([
        "pin",
        "add",
        "--yes",
        f"dune.{dune_version}",
        dune_git_url,
    ])
    hermetic.check_call_opam([
        "pin",
        "add",
        "--yes",
        f"dune-configurator.{dune_version}",
        dune_git_url,
    ])


def provision_opam_with(version: str, keyname: str):
    def say(msg: str):
        sez(msg, ctx="(opam) ")

    provision_opam_binary_with(version)
    HAVE.note_we_have(keyname, version=Version(version))

    opam_version_seen = grab_opam_version_str()
    say(f"opam version: {opam_version_seen}")
    # Opam provisioning is flexible; we'll treat greater versions as if they are
    # the requested version, in some cases (primarily to make CI faster).
    # So we don't want to fail if the version is greater than requested.


def provision_cmake_with(version: str, keyname: str):
    def fmt_url(tag: str) -> str:
        return f"https://github.com/Kitware/CMake/releases/download/v{version}/cmake-{version}-{tag}.tar.gz"

    def mk_url() -> str:
        match [platform.system(), machine_normalized()]:
            case ["Linux", "x86_64"]:
                return fmt_url("linux-x86_64")
            case ["Linux", "aarch64"]:
                return fmt_url("linux-aarch64")
            case ["Darwin", _]:
                return fmt_url("macos-universal")
            case sys_mach:
                raise ProvisioningError(
                    f"Tenjin does not yet support {sys_mach} for acquiring CMake."
                )

    cmake_dir = HAVE.localdir / "cmake"
    if cmake_dir.is_dir():
        # Clear prior installation to avoid tarball unpacking conflicts
        shutil.rmtree(cmake_dir)
    download_and_extract_tarball(mk_url(), cmake_dir, ctx="(cmake) ")

    if platform.system() == "Darwin" and (cmake_dir / "CMake.app").is_dir():
        # The tarball for macOS contains a .app bundle; we'll make a symlink
        # into it to create paths consistent with other platforms.
        cmake_app_bin = cmake_dir / "CMake.app" / "Contents" / "bin"
        os.symlink(cmake_app_bin, cmake_dir / "bin")

    HAVE.note_we_have(keyname, version=Version(version))
    # Running validation after we've set our HAVE version
    # avoids infinite loops.
    validate_actual_cmake_version(version)


def validate_actual_cmake_version(expected_version: str):
    out: bytes = hermetic.run_shell_cmd("cmake --version", check=True, capture_output=True).stdout
    outstr = out.decode("utf-8")
    lines = outstr.splitlines()
    if lines == []:
        raise ProvisioningError("CMake version command returned no output.")
    else:
        match lines[0].split():
            case ["cmake", "version", version]:
                if Version(version) != Version(expected_version):
                    raise ProvisioningError(
                        f"Expected CMake version {expected_version}, got {version}."
                    )
            case _:
                raise ProvisioningError(f"Unexpected output from CMake version command:\n{outstr}")


def provision_10j_llvm_with(version: str, keyname: str):
    xj_llvm_root = xj_llvm_dir(keyname)

    assert "@" in version, "Expected version of the form 'LLVM_VERSION@tenjin-build-deps-release'"
    llvm_version, release = version.split("@", 1)

    def provision_clang_config_files(sysroot_path):
        match platform.system():
            case "Linux":
                platform_specific_stuff = textwrap.dedent(f"""\
                    # OCaml will explicitly pass -L/usr/lib/x86_64-linux-gnu
                    # which throws a wrench in things because the version of glibc in the
                    # sysroot assumes that the libc_nonshared.a file (implicitly linked by
                    # the libc.so linker script) defines __libc_csu_init, but that
                    # symbol has been removed in more recent versions of glibc. Thus,
                    # combining old Scrt1.o from within the sysroot, with new libc_nonshared.a
                    # from outside of the sysroot, results in a link error.
                    # To avoid this outcome, we forcibly add the within-sysroot path
                    # to take precedence over the external system path.
                    #     (Disabled because I have been unable to reproduce the issue
                    #      with a re-pinned sysroot recently. Leaving this note
                    #      for future reference, in case it recurs.  -brk)
                    #-L <CFGDIR>/../sysroot/usr/lib/{platform.machine()}-linux-gnu/

                    # This one's unfortunate. LLD defaults to --no-allow-shlib-undefined
                    # but the libgcc_s.so.1 shipped with Ubuntu 22.04 has an undefined
                    # symbol for _dl_find_object@GLIBC_2.35
                    -Wl,--allow-shlib-undefined
                    """)
            case _:
                platform_specific_stuff = ""

        # Write config files to make sure that the sysroot is used by default.
        for name in ("clang", "clang++", "cc", "c++"):
            with open(xj_llvm_root / "bin" / f"{name}.cfg", "w", encoding="utf-8") as f:
                f.write(
                    textwrap.dedent(f"""\
                        --sysroot {sysroot_path}
                        {platform_specific_stuff}
                        """)
                )

    def provision_debian_sysroot():
        provision_debian_bullseye_sysroot_with(xj_llvm_root / SYSROOT_NAME)

    def create_goblint_gcc_wrapper():
        #                   COMMENTARY(goblint-cil-gcc-wrapper)
        # Okay, this one is unfortunate. We generally only care about software that
        # builds with Clang. But CodeHawk depends on goblint-cil, which uses C code
        # in its config step that has GCC extensions which Clang doesn't support, &
        # thus goblint-cil looks specifically for a GCC binary. So what we're gonna
        # do here is write out a wrapper script for goblint-cil to find, which will
        # intercept the GCC-specific stuff in the code it compiles and patch it out
        # before passing it on to Clang. Hurk!
        sadness = xj_llvm_root / "goblint-sadness"
        sadness.mkdir(exist_ok=True)
        gcc_wrapper_path = sadness / "gcc"
        with open(gcc_wrapper_path, "w", encoding="utf-8") as f:
            f.write(
                textwrap.dedent("""\
                    #!/bin/sh

                    # See COMMENTARY(goblint-cil-gcc-wrapper) in cli/provisioning.py

                    if [ "$1" = "--version" ]; then
                        echo "gcc (GCC) 7.999.999"
                    elif [ "$*" = "-D_GNUCC machdep-ml.c -o machdep-ml.exe" ]; then

                        CFILE=machdep-ml-clangcompat.c
                        # Remove references to Clang-unsupported type _Float128.
                        cat machdep-ml.c \\
                                | sed 's/_Float128 _Complex/struct { char _[32]; }/g' \\
                                | sed 's/_Float128 _Complex/struct { char _[32]; }/g' \\
                                | sed 's/_Float128/struct { char _[16]; }/g' \\
                                | sed 's/_Float64x/double/g' \\
                                | sed 's/_Float32x/float/g' > "$CFILE" || {
                            rm -f "$CFILE"
                            exit 1
                        }

                        exec clang -D_GNUCC "$CFILE" -o machdep-ml.exe
                        rm -f "$CFILE"
                    else
                        exec clang "$@"
                    fi
                    """)
            )
        gcc_wrapper_path.chmod(0o755)

    def add_binutils_alike_symbolic_links():
        # Add symbolic links for the binutils-alike tools.
        # Tools not provided by LLVM: ranlib, size
        # Tenjin's LLVM ships `as` as a wrapper around `llvm-mc`
        #   because `llvm-as` is for assembling LLVM IR, not platform assembly.
        binutils_names = ["ar", "nm", "objcopy", "objdump", "readelf", "strings", "strip"]
        for name in binutils_names:
            src = xj_llvm_root / "bin" / f"llvm-{name}"
            dst = xj_llvm_root / "bin" / f"{name}"
            if not dst.is_symlink():
                os.symlink(src, dst)

        # On macOS, `as` is provided by the system SDK, and expects flags
        # which diverge from `llvm-mc`. So instead of trying to filter those out,
        # we deactivate the `as` wrapper on macOS.
        if platform.system() == "Darwin":
            as_wrapper_path = xj_llvm_root / "bin" / "as"
            as_wrapper_path.rename(as_wrapper_path.with_name("xj-as-wrapper-disabled"))

        # These symbolic links follow a different naming pattern.
        symlinks = [("clang", "cc"), ("clang++", "c++")]
        if platform.system() != "Darwin":
            # On macOS, lld does not support -r (--relocatable) but the flag is used
            # by OCaml's build system, so we omit the symlink. This means that Clang
            # will use ld64.lld directly, but when OCaml invokes ld, it will get the
            # system's ld64 (non-LLD).
            symlinks.append(("lld", "ld"))
        for src, dst in symlinks:
            src = xj_llvm_root / "bin" / src
            dst = xj_llvm_root / "bin" / dst
            if not dst.is_symlink():
                os.symlink(src, dst)

    target_dir_existed = xj_llvm_root.is_dir()
    if xj_llvm_root.is_dir():
        shutil.rmtree(xj_llvm_root)
        # In nuking the prior LLVM installation, we also lose the prior sysroot's extras.
        HAVE.note_removed("10j-bullseye-sysroot-extras")

    tarball_name = f"LLVM-{llvm_version}-{platform.system()}-{machine_normalized()}.tar.xz"
    if Path(tarball_name).is_file():
        # A local tarball was likely manually downloaded. Use it if we've got it.
        extract_tarball(Path(tarball_name), xj_llvm_root, ctx="(llvm) ")
    else:
        url = f"https://github.com/Aarno-Labs/tenjin-build-deps/releases/download/{release}/{tarball_name}"
        download_and_extract_tarball(url, xj_llvm_root, ctx="(llvm) ")

    match platform.system():
        case "Linux":
            provision_debian_sysroot()
            provision_clang_config_files(sysroot_path=f"<CFGDIR>/../{SYSROOT_NAME}")
        case "Darwin":
            xcrun_path = (
                subprocess.check_output(["xcrun", "--show-sdk-path"]).decode("utf-8").strip()
            )
            provision_clang_config_files(sysroot_path=xcrun_path)

    add_binutils_alike_symbolic_links()

    if target_dir_existed and not llvm_version.startswith("14."):
        # We must clean up any executables that dynamically linked against the old LLVM.
        sez("Cleaning up binaries linked against the prior LLVM version...", ctx="(c2rust) ")

        # Tenjin's c2rust binary will be rebuilt on demand.
        hermetic.run_cargo_in(["clean"], repo_root.find_repo_root_dir_Path() / "c2rust")

        # So will xj-prepare-find-fn-ptr-decls, so we can just delete its build dir.
        dirty = hermetic.xj_prepare_findfnptrdecls_build_dir(HAVE.localdir)
        if dirty.is_dir():
            shutil.rmtree(dirty, ignore_errors=False)

        # Upstream c2rust is not rebuilt automatically, so we need to do it here.
        upstream_c2rust_dir = hermetic.xj_upstream_c2rust(HAVE.localdir)
        if upstream_c2rust_dir.is_dir():
            hermetic.run_cargo_in(["clean"], upstream_c2rust_dir)
            rebuild_10j_upstream_c2rust(upstream_c2rust_dir)

    create_goblint_gcc_wrapper()
    update_10j_llvm_have(keyname, version, llvm_version, xj_llvm_root)


def update_10j_llvm_have(keyname: str, version: str, llvm_version: str, xj_llvm_root: Path):
    out = subprocess.check_output([xj_llvm_root / "bin" / "llvm-config", "--version"])
    # If our requested LLVM version looks like "X.Y.Z+foo", we'll compare the tool's reported
    # version against the X.Y.Z part only.
    comparable_llvm_version = llvm_version.split("+")[0]
    saw = out.decode("utf-8")
    if Version(saw) != Version(comparable_llvm_version):
        raise ProvisioningError(f"Expected LLVM version {comparable_llvm_version}, got {saw}.")
    # But the HAVE database needs the full version.
    HAVE.note_we_have(keyname, specifier=version)


def get_path_of_unusual_size() -> bytes:
    fifty = b"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    rilly = b"thisverylongpathistogiveusroomtooverwriteitlaterok"
    twohundredfifty = fifty + fifty + fifty + fifty + rilly
    return b"/tmp/" + twohundredfifty + b"/" + twohundredfifty


#                COMMENTARY(pkg-config-paths)
# pkg-config embeds various configured paths into the binary.
# In particular, it embeds, via compiler flags during compilation,
# LIBDIR (via --prefix), PKG_CONFIG_PC_PATH, PKG_CONFIG_SYSTEM_INCLUDE_PATH,
# and PKG_CONFIG_SYSTEM_LIBRARY_PATH.
#
# So what we do, and ugh this leaves me feeling a little queasy, hurk, is...
# we embed very large fake paths into the pkg-config binary, and we call that
# binary "uncooked". Then we make a copy of the binary, and make in-place edits
# to have the embedded paths match those on the user's system. (The fake paths
# are large enough that they should accommodate whatever path the user has.)
def cook_pkg_config_within():
    def say(msg: str):
        sez(msg, ctx="(pkg-config) ")

    path_of_unusual_size = get_path_of_unusual_size()
    libdir = path_of_unusual_size + b"/prefix/lib"
    sysinc = path_of_unusual_size + b"/sysinc:/usr/include"
    syslib = path_of_unusual_size + b"/syslib:/usr/lib:/lib"
    pcpath = path_of_unusual_size + b"/lib/pkgconfig:" + path_of_unusual_size + b"/share/pkgconfig"
    nullbyte = b"\0"

    bindir = hermetic.xj_build_deps(HAVE.localdir) / "bin"

    uncooked = bindir / "pkg-config.uncooked"
    assert uncooked.is_file()
    cooked = bindir / "pkg-config"
    shutil.copy(uncooked, cooked)

    def replace_null_terminated_needle_in(haystack: bytes, needle: bytes, newstuff: bytes) -> bytes:
        # Make sure is has the embedded path/data we are expecting it to have.
        assert (needle + nullbyte) in haystack

        assert len(newstuff) <= len(needle)
        if len(newstuff) < len(needle):
            # Pad the newstuff with null bytes to match the length of needle.
            newstuff += nullbyte * (len(needle) - len(newstuff))

        assert len(newstuff) == len(needle)
        return haystack.replace(needle, newstuff)

    say("Cooking pkg-config...")
    with open(cooked, "r+b") as f:
        # Read the file into memory
        data = f.read()

        # Note that these paths need not exist on the filesystem yet,
        # we're just baking them into the binary.
        sysroot_usr = hermetic.xj_llvm_root(HAVE.localdir) / "sysroot" / "usr"
        newpcpath_lib = sysroot_usr / "lib" / "pkgconfig"
        newpcpath_shr = sysroot_usr / "share" / "pkgconfig"

        # Replace the placeholder strings with the actual paths.
        # Note that we set up the paths to include pkg-config's standard paths as backups,
        # in case the user is trying to compile against a library that isn't in the sysroot.
        data = replace_null_terminated_needle_in(
            data,
            sysinc,
            bytes(sysroot_usr / "include") + b":/usr/include",
        )
        data = replace_null_terminated_needle_in(
            data,
            syslib,
            bytes(sysroot_usr / "lib") + b":/usr/lib:/lib",
        )
        data = replace_null_terminated_needle_in(
            data, pcpath, bytes(newpcpath_lib) + b":" + bytes(newpcpath_shr) + b":/usr/lib:/lib"
        )
        data = replace_null_terminated_needle_in(
            data, libdir, bytes(hermetic.xj_build_deps(HAVE.localdir) / "lib")
        )

        # Write the modified data back to the file
        f.seek(0)
        f.write(data)
        f.truncate()
    say("... done cooking pkg-config.")

    assert path_of_unusual_size not in data, "Oops, pkg-config was left undercooked!"


def cook_m4_within():
    def say(msg: str):
        sez(msg, ctx="(m4) ")

    path_of_unusual_size = get_path_of_unusual_size()
    localedir = path_of_unusual_size + b"/share/locale"
    nullbyte = b"\0"

    bindir = hermetic.xj_build_deps(HAVE.localdir) / "bin"

    uncooked = bindir / "m4.uncooked"
    if not uncooked.is_file():
        # m4.uncooked may not be present in older tarballs; skip cooking if absent.
        return
    cooked = bindir / "m4"
    shutil.copy(uncooked, cooked)

    def replace_null_terminated_needle_in(haystack: bytes, needle: bytes, newstuff: bytes) -> bytes:
        # Make sure it has the embedded path/data we are expecting it to have.
        assert (needle + nullbyte) in haystack

        assert len(newstuff) <= len(needle)
        if len(newstuff) < len(needle):
            # Pad the newstuff with null bytes to match the length of needle.
            newstuff += nullbyte * (len(needle) - len(newstuff))

        assert len(newstuff) == len(needle)
        return haystack.replace(needle, newstuff)

    say("Cooking m4...")
    with open(cooked, "r+b") as f:
        # Read the file into memory
        data = f.read()

        # Replace the placeholder pkgdatadir with the actual path.
        data = replace_null_terminated_needle_in(
            data,
            localedir,
            bytes(hermetic.xj_build_deps(HAVE.localdir) / "share" / "locale"),
        )

        # Write the modified data back to the file
        f.seek(0)
        f.write(data)
        f.truncate()
    say("... done cooking m4.")

    assert path_of_unusual_size not in data, "Oops, m4 was left undercooked!"


def cook_automake_and_autoconf_within() -> None:
    """Cook automake/aclocal/autoconf scripts and Config.pm to use actual paths.

    These are text files (Perl scripts and modules) that have '/outputs'
    embedded as a placeholder prefix. We replace it with the actual
    xj-build-deps path.
    """

    def say(msg: str):
        sez(msg, ctx="(automake) ")

    xj_build_deps = hermetic.xj_build_deps(HAVE.localdir)
    placeholder = b"/outputs"  # XREF(tenjin-build-deps-automake-path)
    replacement = bytes(xj_build_deps)

    # Collect all the text files that need cooking
    files_to_cook: list[Path] = []

    config_pm = xj_build_deps / "share" / "automake-1.17" / "Automake" / "Config.pm"
    assert config_pm.is_file()
    files_to_cook.append(config_pm)

    autom4te_cfg = xj_build_deps / "share" / "autoconf" / "autom4te.cfg"
    assert autom4te_cfg.is_file()
    files_to_cook.append(autom4te_cfg)

    # automake, aclocal, and autoconf scripts (including versioned variants)
    bindir = xj_build_deps / "bin"
    for pattern in ["automake*", "aclocal*"]:
        files_to_cook.extend(bindir.glob(pattern))
    for name in [
        "autoconf",
        "autoreconf",
        "autoheader",
        "autom4te",
        "autoscan",
        "autoupdate",
        "ifnames",
    ]:
        files_to_cook.append(bindir / name)

    say("Cooking automake/aclocal/autoconf...")
    for filepath in files_to_cook:
        with open(filepath, "rb") as f:
            data = f.read()

        if placeholder not in data:
            # This file doesn't need cooking (or was already cooked)
            continue

        data = data.replace(placeholder, replacement)

        with open(filepath, "wb") as ftc:
            ftc.write(data)

    say("... done cooking automake/aclocal.")


def provision_10j_deps_with(version: str, keyname: str):
    match platform.system():
        case "Linux":
            filename = f"xj-build-deps_{machine_normalized()}.tar.xz"
            url = f"https://github.com/Aarno-Labs/tenjin-build-deps/releases/download/{version}/{filename}"
            target = hermetic.xj_build_deps(HAVE.localdir)
            if target.is_dir():
                shutil.rmtree(target)
            download_and_extract_tarball(url, target, ctx="(builddeps) ")
            cook_pkg_config_within()
            cook_m4_within()
            cook_automake_and_autoconf_within()

        case "Darwin":
            # For macOS, we don't do hermetic build deps; instead, we rely on homebrew.
            # This bypasses the need to cook pkg-config, because it will have hardcoded
            # paths that are suitable for interoperation with homebrew.
            if shutil.which("brew") is None:
                raise ProvisioningError(
                    "Homebrew is not installed. Please install it from https://brew.sh"
                )

            if shutil.which("pkg-config") is None:
                try:
                    subprocess.check_call(["brew", "install", "pkg-config"])
                except subprocess.CalledProcessError:
                    # Maybe homebrew removed this formula?
                    pass

            if shutil.which("pkgconf") is None:
                subprocess.check_call(["brew", "install", "pkgconf"])

            # The other dependencies we need on Linux, like patch and make,
            # should have been provided already by Xcode Developer Tools.
            # Bubblewrap is Linux-only.

    HAVE.note_we_have(keyname, specifier=version)


def download_and_extract_tarball(
    tarball_url: str,
    target_dir: Path,
    ctx: str,
) -> None:
    """
    Downloads a compressed tar file from the given URL and extracts it to the target directory.

    Args:
        tarball_url (str): URL of the tarball file to download
        target_dir (str): Directory to extract contents to.
    """

    def say(msg: str):
        sez(msg, ctx)

    temp_file = None
    try:
        # Create a temporary file name for the download
        temp_file = os.path.basename(urlparse(tarball_url).path)

        say(f"Downloading {tarball_url}...")
        download(tarball_url, Path(temp_file))
    except Exception:
        # Clean up any temporary files if they exist
        if temp_file:
            if os.path.exists(temp_file):
                os.remove(temp_file)
        raise

    extract_tarball(Path(temp_file), target_dir, ctx)

    # Clean up the temporary file
    os.remove(temp_file)

    say(f"Download and extraction of {temp_file} completed successfully!")


# The extraction process is about twice as slow on macOS
# for clang+llvm versus the native bsdtar utility, but
# since this is a one-time cost it seems better to just
# avoid non-Python dependencies as much as we can.
def extract_tarball(
    tarball_path: Path, initial_target_dir: Path, ctx: str, time_estimate="a few seconds"
) -> Path:
    """
    Extracts the given tarball into (or within) the target directory.

    If the tarball unpacks a single directory with the same name as the tarball
    (minus the suffix), the contents of that directory will be moved up a level,
    and the empty directory will be removed.

    Returns the path to the directory that contains the unpacked contents.
    """

    def say(msg: str):
        sez(msg, ctx)

    def is_empty_dir(path: Path) -> bool:
        if not path.is_dir():
            return False

        is_empty = True
        for item in path.iterdir():
            is_empty = False
            break
        return is_empty

    def choose_target_dir(initial_target_dir: Path) -> tuple[Path, str]:
        # Check if the tarball unpacks a single directory with the same name as the tarball
        def select_tarball_suffix(filename: str) -> str:
            if filename.endswith(".tar.xz"):
                return ".tar.xz"
            elif filename.endswith(".tar.gz"):
                return ".tar.gz"
            elif filename.endswith(".tgz"):
                return ".tgz"
            elif filename.endswith(".tar.bz2"):
                return ".tar.bz2"
            elif filename.endswith(".tbz"):
                return ".tbz"
            raise ValueError(f"Unknown tarball suffix for URL: {filename}")

        suffix = select_tarball_suffix(tarball_path.name)
        tarball_basename = tarball_path.name.removesuffix(suffix)

        target_dir_preexisted = initial_target_dir.is_dir()
        if target_dir_preexisted and not is_empty_dir(initial_target_dir):
            # If the target directory already existed, and is not empty,
            # we'll unpack the tarball into a new directory inside it.

            final_target_dir = initial_target_dir / tarball_basename
            final_target_dir.mkdir(parents=True, exist_ok=True)
        else:
            final_target_dir = initial_target_dir

        return final_target_dir, tarball_basename

    if time_estimate is not None:
        say(f"This will take {time_estimate}...")

    final_target_dir, tarball_basename = choose_target_dir(initial_target_dir)

    if final_target_dir != initial_target_dir:
        say(f"Extracting to subdirectory {final_target_dir}...")
    else:
        say(f"Extracting to {initial_target_dir}...")

    # Create target/parent directory if it doesn't exist
    initial_target_dir.mkdir(parents=True, exist_ok=True)

    # Extract the compressed tar file
    with tarfile.open(str(tarball_path), "r:*") as tar:
        tar.extractall(path=final_target_dir)

    if time_estimate is not None:
        say(f"Extraction of {tarball_path.name} completed successfully!")

    # For example, we have foo-bar.tar.gz, and unpack it into blah/;
    #   then if we find blah/foo-bar/, we trim out the foo-bar part.
    final_dir_contents = list(final_target_dir.iterdir())
    replicated_tarball_name = final_dir_contents == [final_target_dir / tarball_basename]
    # Likewise, if we find blah/blah/, we trim out the middle blah part.
    replicated_target_basename = final_dir_contents == [final_target_dir / final_target_dir.name]
    if replicated_tarball_name or replicated_target_basename:
        extracted_path = final_dir_contents[0]
        # The tarball unpacks a single directory, so move its contents up a level
        for item in extracted_path.iterdir():
            shutil.move(str(item), str(final_target_dir))

        # Remove the now-empty directory
        extracted_path.rmdir()

    return final_target_dir


def provision_10j_ast_grep_with(version: str, keyname: str):
    """Download and install ast-grep binary."""

    def say(msg: str):
        sez(msg, ctx="(ast-grep) ")

    def mk_url() -> str:
        """Build the download URL based on platform and architecture."""
        base_url = f"https://github.com/ast-grep/ast-grep/releases/download/{version}"
        match [platform.system(), machine_normalized()]:
            case ["Linux", "x86_64"]:
                return f"{base_url}/app-x86_64-unknown-linux-gnu.zip"
            case ["Linux", "aarch64"]:
                return f"{base_url}/app-aarch64-unknown-linux-gnu.zip"
            case ["Darwin", "x86_64"]:
                return f"{base_url}/app-x86_64-apple-darwin.zip"
            case ["Darwin", "aarch64"]:
                return f"{base_url}/app-aarch64-apple-darwin.zip"
            case sys_mach:
                raise ProvisioningError(f"ast-grep: unsupported platform: {sys_mach}")

    target_dir = HAVE.localdir / "ast-grep"
    if target_dir.is_dir():
        shutil.rmtree(target_dir)

    target_dir.mkdir(parents=True, exist_ok=True)
    url = mk_url()

    say("Downloading and extracting ast-grep...")
    temp_file = None
    try:
        with tempfile.NamedTemporaryFile(delete=False, suffix=".zip") as tmp:
            temp_file = tmp.name
            download(url, Path(temp_file))
    except Exception:
        if temp_file and Path(temp_file).exists():
            os.remove(temp_file)
        raise

    with zipfile.ZipFile(temp_file, "r") as zip_ref:
        zip_ref.extractall(target_dir)

    # Clean up the temporary file
    os.remove(temp_file)

    # Make the binary executable
    binary_path = target_dir / "ast-grep"
    if binary_path.is_file():
        binary_path.chmod(binary_path.stat().st_mode | 0o111)
    else:
        # Check if it's in a subdirectory (some releases extract to a subfolder)
        for item in target_dir.iterdir():
            if item.is_dir():
                potential_binary = item / "ast-grep"
                if potential_binary.is_file():
                    # Move binary up to target_dir
                    shutil.move(str(potential_binary), str(binary_path))
                    binary_path.chmod(binary_path.stat().st_mode | 0o111)
                    # Remove the now-empty subdirectory
                    shutil.rmtree(item)
                    break

    if not binary_path.is_file():
        raise ProvisioningError("ast-grep binary not found after extraction")

    HAVE.note_we_have(keyname, version=Version(version))
    say(f"v{version} acquired successfully")
