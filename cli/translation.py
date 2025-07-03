import shutil
import tempfile
from pathlib import Path
import tomllib
import re
import os

import hermetic


def do_translate(
    root: Path,
    codebase: Path,
    resultsdir: Path,
    cratename: str,
    c_main_in: str | None = None,
    guidance: str | None = None,
):
    """
    Translate a codebase from C to Rust.

    The input directory should have a pre-generated compile_commands.json file,
    or an easy means of generating one (such as a CMakeLists.txt file).

    The `resultsdir` directory will contain subdirectories with intermediate
    stages of the resulting translation. Assuming no errors occurred, the final
    translation will be in the `final` subdirectory. The `resultsdir` will also
    (eventually)
    have a file called `ingest.json` containing metadata about the translation.
    """

    def perform_pre_translation(builddir: Path) -> Path:
        provided_compdb = codebase / "compile_commands.json"
        provided_cmakelists = codebase / "CMakeLists.txt"

        if provided_cmakelists.exists() and not provided_compdb.exists():
            # If we have a CMakeLists.txt, we can generate the compile_commands.json
            hermetic.run(
                [
                    "cmake",
                    "-S",
                    str(codebase),
                    "-B",
                    str(builddir),
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                ],
                check=True,
            )
            return builddir / "compile_commands.json"
        else:
            # Otherwise, we assume the compile_commands.json is already present
            return provided_compdb

    c2rust_transpile_flags = [
        "--translate-const-macros",
        "--reduce-type-annotations",
        "--disable-refactoring",
        "--log-level",
        "INFO",
        "--guidance",
        guidance if guidance else "{}",
    ]

    if c_main_in:
        c2rust_transpile_flags.extend(["--binary", c_main_in.removesuffix(".c")])

    with tempfile.TemporaryDirectory() as builddirname:
        builddir = Path(builddirname)
        compdb = perform_pre_translation(builddir)

        c2rust_bin = root / "c2rust" / "target" / "debug" / "c2rust"

        # The crate name that c2rust uses is based on the directory stem,
        # so we create a subdirectory with the desired crate name.
        output = resultsdir / cratename
        output.mkdir(parents=True, exist_ok=False)
        hermetic.run(
            [str(c2rust_bin), "transpile", str(compdb), "-o", str(output), *c2rust_transpile_flags],
            check=True,
            env_ext={
                "RUST_BACKTRACE": "1",
            },
        )

    # This is a nightly toolchain, which we'll use if needed due to unstable features.
    toolchain = get_multitool_toolchain(root)

    # Normalize the unmodified translation results to end up
    # in a directory with a project-independent name.
    output = output.rename(output.with_name("00_out"))

    # Verify that the initial translation is valid Rust code.
    # If it has errors, we won't be able to run the improvement passes.
    hermetic.run_cargo_in(["check"], cwd=output, check=True, toolchain=toolchain)
    hermetic.run_cargo_in(["clean"], cwd=output, check=True)

    run_improvement_passes(root, output, resultsdir, toolchain)

    # Find the highest numbered output directory and copy its contents
    # to the final output directory.
    highest_out = find_highest_numbered_dir(resultsdir)
    if highest_out is not None:
        shutil.copytree(
            highest_out,
            resultsdir / "final",
        )


def find_highest_numbered_dir(base: Path) -> Path | None:
    """
    Find the directory with the largest underscore-suffixed numeric prefix.
    """
    pattern = re.compile(r"^(\d+)_.*$")
    base = Path(base)

    if not base.exists():
        return None

    max_num = -1
    latest_dir = None

    for item in base.iterdir():
        if item.is_dir():
            match = pattern.match(item.name)
            if match:
                num = int(match.group(1))
                if num > max_num:
                    max_num = num
                    latest_dir = item

    return latest_dir if latest_dir else None


def get_multitool_toolchain(root: Path) -> str:
    with open(root / "xj-improve-multitool" / "rust-toolchain.toml", "rb") as f:
        toolchain_dict = tomllib.load(f)
        return "+" + toolchain_dict["toolchain"]["channel"]


def run_improve_multitool(root: Path, tool: str, args: list[str], dir: Path):
    # External Cargo tools need not be installed to be used, they only need to be on the PATH.
    # Since rustc plugins like xj-improve-multitool are tied to a specific toolchain,
    # it's not ideal to install them globally.
    # We could unconditionally `cargo install` into the _local/bin directory,
    # but it's a bit faster to just build & run from `target`.
    hermetic.run_cargo_in(
        ["xj-improve-multitool", "--tool", tool, *args],
        cwd=dir,
        toolchain=get_multitool_toolchain(root),
        env_ext={
            "PATH": os.pathsep.join([
                str(root / "xj-improve-multitool" / "target" / "debug"),
                os.environ["PATH"],
            ]),
        },
        check=True,
    )


def run_improvement_passes(root: Path, output: Path, resultsdir: Path, toolchain: str):
    improvement_passes = [
        (
            "fix",
            lambda _root, dir: hermetic.run_cargo_in(
                ["fix", "--allow-no-vcs", "--allow-dirty"],
                cwd=dir,
                check=True,
                toolchain=toolchain,
            ),
        ),
        (
            "trimdead",
            lambda root, dir: run_improve_multitool(
                root, "TrimDeadItems", ["--modify-in-place"], dir
            ),
        ),
        ("fmt", lambda _root, dir: hermetic.run_cargo_in(["fmt"], cwd=dir, check=True)),
        (
            "fix",
            lambda _root, dir: hermetic.run_cargo_in(
                ["fix", "--allow-no-vcs", "--allow-dirty"],
                cwd=dir,
                check=True,
                toolchain=toolchain,
            ),
        ),
    ]

    prev = output
    for counter, (tag, func) in enumerate(improvement_passes, start=1):
        newdir = resultsdir / f"{counter:02d}_{tag}"
        shutil.copytree(prev, newdir)
        # Run the actual improvement pass, modifying the contents of `newdir`.
        func(root, newdir)
        # Use explicit toolchain for checks because c2rust may use extern_types which is unstable.
        hermetic.run_cargo_in(["check"], cwd=newdir, check=True, toolchain=toolchain)
        # Clean up the target directory so the next pass starts fresh.
        hermetic.run_cargo_in(["clean"], cwd=newdir, check=True)
        prev = newdir
