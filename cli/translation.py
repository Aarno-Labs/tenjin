import shutil
import tempfile
from pathlib import Path
from subprocess import CompletedProcess, CalledProcessError
import re
import json
import pprint
import time
import uuid
import shlex
from platform import platform
import hashlib

import click

from repo_root import find_repo_root_dir_Path, localdir
import provisioning
import ingest
import ingest_tracking
import hermetic
import vcs_helpers
import static_measurements_rust
from translation_improvement import run_improvement_passes


def stub_ingestion_record(codebase: Path, guidance: dict) -> ingest.TranslationRecord | None:
    """
    Create a stub IngestionRecord for the given codebase and crate name.
    This is used to initialize the ingestion record before the actual translation.
    """

    codebase_vcs_dir = vcs_helpers.find_containing_vcs_dir(codebase)
    if codebase_vcs_dir is None:
        return None

    codebase_wcs = vcs_helpers.vcs_working_copy_status(codebase_vcs_dir)

    tenjin_vcs_dir = vcs_helpers.find_containing_vcs_dir(find_repo_root_dir_Path())
    assert tenjin_vcs_dir is not None, "No VCS directory found for Tenjin?!?!"

    tenjin_wcs = vcs_helpers.vcs_working_copy_status(tenjin_vcs_dir)

    assert tenjin_wcs.origin is not None, "Tenjin working copy has no origin URL?!?"
    assert tenjin_wcs.commit is not None, "Tenjin working copy has no commit hash?!?"

    assert codebase_wcs.origin is not None, "Codebase working copy has no origin URL?!?"
    assert codebase_wcs.commit is not None, "Codebase working copy has no commit hash?!?"

    codebase_relative_path = codebase.relative_to(vcs_helpers.vcs_root(codebase_vcs_dir))
    upstream_c2rust = provisioning.HAVE.query("10j-reference-c2rust-tag")
    return ingest.TranslationRecord(
        translation_uuid=uuid.uuid4(),
        inputs=ingest.TranslationInputs(
            codebase=ingest.IngestedCodebase(
                git_repo_url=codebase_wcs.origin,
                git_commit=codebase_wcs.commit,
                relative_path=str(codebase_relative_path),
            ),
            host_platform=platform(),
            tenjin_git_repo_url=tenjin_wcs.origin,
            tenjin_git_commit=tenjin_wcs.commit,
            c2rust_baseline_version=upstream_c2rust or "unknown",
            per_file_preprocessor_definitions={},
            guidance=guidance,
        ),
        results=ingest.TranslationResults(
            translation_start_unix_timestamp=int(time.time()),
            translation_elapsed_ms=0,
            static_measurement_elapsed_ms=0,
            transformations=[],
            c2rust_baseline=None,
            tenjin_initial=None,
            tenjin_final=None,
        ),
    )


def run_c2rust(
    tracker: ingest_tracking.TimingRepo,
    tag: str,
    c2rust_bin: Path,
    compdb: Path,
    output: Path,
    flags: list[str],
) -> CompletedProcess:
    with tracker.tracking(tag, output) as step:
        cp = hermetic.run(
            [
                str(c2rust_bin),
                "transpile",
                str(compdb),
                "-o",
                str(output),
                *flags,
            ],
            check=True,
            env_ext={
                "RUST_BACKTRACE": "1",
            },
            capture_output=True,
        )
        step.update_sub(cp)
        return cp


def create_subdirectory_snapshot(
    is_rust: bool,
    codebase: Path,
    subdir_label: str,
) -> ingest.SubdirectorySnapshot:
    def snapshot_for_file(p: Path) -> ingest.SubdirectoryFileSnapshot:
        content_bytes = p.read_bytes()
        lines = content_bytes.decode("utf-8", errors="replace").splitlines()
        relpath = p.relative_to(codebase).as_posix()
        if relpath == ".":
            relpath = p.name
        return ingest.SubdirectoryFileSnapshot(
            path=relpath,
            lines=lines,
            sha256=hashlib.sha256(content_bytes).hexdigest(),
        )

    if codebase.is_file():
        # If the codebase is a single file, we treat it as a subdirectory with one file.
        return ingest.SubdirectorySnapshot(
            path=subdir_label,
            files=[snapshot_for_file(codebase)],
        )

    file_snapshots = []
    paths = sorted(list(codebase.rglob("*")))
    for p in paths:
        if p.is_file():
            if p.suffix not in [".json", ".rs", ".c", ".h"]:
                continue
            if not is_rust and "CMakeFiles" in p.parts:
                continue
            if is_rust and p.relative_to(codebase).parts[0] == "target":
                continue
            file_snapshots.append(snapshot_for_file(p))
    return ingest.SubdirectorySnapshot(
        path=subdir_label,
        files=file_snapshots,
    )


def create_translation_snapshot(
    root: Path, codebase: Path, resultsdir: Path, record: ingest.TranslationRecord
) -> ingest.TranslationResultsSnapshot:
    c_snapshot = create_subdirectory_snapshot(False, codebase, "original_codebase")

    rust_snapshots = []
    for dirname in ["vanilla_c2rust", "00_out", "final"]:
        subdir = resultsdir / dirname
        assert subdir.is_dir()
        rust_snapshots.append(create_subdirectory_snapshot(True, subdir, dirname))

    results_snapshot = ingest.TranslationResultsSnapshot(
        for_translation=record.translation_uuid,
        c_versions=[c_snapshot],
        rust_versions=rust_snapshots,
    )

    return results_snapshot


def write_synthetic_compile_commands_to(compdb_path: Path, c_file: Path, builddir: Path):
    """Write a synthetic compile_commands.json file for a single C file."""
    assert compdb_path.parent.is_dir()
    outname = c_file.with_suffix(".o").name
    cc = hermetic.xj_llvm_root(localdir()) / "bin" / "clang"
    c_file_full_q = shlex.quote(c_file.resolve().as_posix())
    contents = json.dumps(
        [
            {
                "directory": builddir.as_posix(),
                "command": f"{cc} -c {c_file_full_q} -o {shlex.quote(outname)}",
                "file": c_file.resolve().as_posix(),
                "output": outname,
            }
        ],
        indent=2,
    )
    compdb_path.write_text(contents, encoding="utf-8")


def do_translate(
    root: Path,
    codebase: Path,
    resultsdir: Path,
    cratename: str,
    guidance_path_or_literal: str,
    c_main_in: str | None = None,
):
    """
    Translate a codebase from C to Rust.

    The `codebase` argument should be a path to a directory or a single C file.

    The input directory should have a pre-generated compile_commands.json file,
    or an easy means of generating one (such as a CMakeLists.txt file).

    The `resultsdir` directory will contain subdirectories with intermediate
    stages of the resulting translation. Assuming no errors occurred, the final
    translation will be in the `final` subdirectory. The `resultsdir` will also
    have files called `translation_metadata.json` and `translation_snapshot.json`.
    """

    guidance = load_and_parse_guidance(guidance_path_or_literal)

    tracker = ingest_tracking.TimingRepo(stub_ingestion_record(codebase, guidance))

    def perform_pre_translation(builddir: Path) -> Path:
        """Returns the path to the provided-or-generated compile_commands.json file."""
        provided_compdb = codebase / "compile_commands.json"
        provided_cmakelists = codebase / "CMakeLists.txt"

        if provided_cmakelists.exists() and not provided_compdb.exists():
            # If we have a CMakeLists.txt, we can generate the compile_commands.json
            cp = hermetic.run(
                [
                    "cmake",
                    "-S",
                    str(codebase),
                    "-B",
                    str(builddir),
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                ],
                check=True,
                capture_output=True,
            )
            tracker.update_sub(cp)
            return builddir / "compile_commands.json"
        elif codebase.is_file() and codebase.suffix == ".c":
            # If we have a single C file, we can trivially generate a compile_commands.json
            # with a single entry for it.
            write_synthetic_compile_commands_to(
                builddir / "compile_commands.json", codebase, builddir
            )
            return builddir / "compile_commands.json"
        else:
            # Otherwise, we assume the compile_commands.json is already present
            return provided_compdb

    c2rust_transpile_flags = [
        "--translate-const-macros",
        "--reduce-type-annotations",
        "--disable-refactoring",
    ]

    if not c_main_in and codebase.is_file() and codebase.suffix == ".c":
        c_main_in = codebase.name

    if not c_main_in and (codebase / "main.c").is_file():
        c_main_in = "main.c"

    if not c_main_in and (codebase / "src" / "main.c").is_file():
        c_main_in = "main.c"

    if c_main_in:
        c2rust_transpile_flags.extend(["--binary", c_main_in.removesuffix(".c")])

    xj_c2rust_transpile_flags = [
        *c2rust_transpile_flags,
        "--log-level",
        "INFO",
        "--guidance",
        json.dumps(guidance),
    ]

    with tempfile.TemporaryDirectory() as builddirname:
        builddir = Path(builddirname)
        with tracker.tracking("pretranslation", builddir) as _step:
            compdb = perform_pre_translation(builddir)

        with open(compdb, "r", encoding="utf-8") as compdb_f:
            tracker.set_preprocessor_definitions(
                extract_preprocessor_definitions_from_compile_commands(
                    json.load(compdb_f),
                    codebase,
                ),
            )

        # The crate name that c2rust uses is based on the directory stem,
        # so we create a subdirectory with the desired crate name.
        output = resultsdir / cratename
        output.mkdir(parents=True, exist_ok=False)
        # First run the upstream c2rust tool to get a baseline translation.
        upstream_c2rust_bin = localdir() / "upstream-c2rust" / "target" / "debug" / "c2rust"
        _up_cp = run_c2rust(
            tracker, "upstream-c2rust", upstream_c2rust_bin, compdb, output, c2rust_transpile_flags
        )
        output = output.rename(output.with_name("vanilla_c2rust"))

        # Then run our version, using guidance and preanalysis.
        output = resultsdir / cratename
        output.mkdir(parents=True, exist_ok=False)
        c2rust_bin = root / "c2rust" / "target" / "debug" / "c2rust"
        try:
            _xj_cp = run_c2rust(
                tracker, "xj-c2rust", c2rust_bin, compdb, output, xj_c2rust_transpile_flags
            )
        except CalledProcessError as e:
            click.echo("stdout:", err=True)
            click.echo(e.stdout.decode("utf-8", errors="replace"), err=True)
            click.echo("stderr:", err=True)
            click.echo(e.stderr.decode("utf-8", errors="replace"), err=True)
            raise

        # Normalize the unmodified translation results to end up
        # in a directory with a project-independent name.
        output = output.rename(output.with_name("00_out"))

    # Verify that the initial translation is valid Rust code.
    # If it has errors, we won't be able to run the improvement passes.
    initial_cp = hermetic.run_cargo_in(["check"], cwd=output, check=False)
    # Ensure that subsequent passes start with a clean slate.
    clean_p_cp = hermetic.run_cargo_in(["clean", "-p", cratename], cwd=output, check=False)

    if initial_cp.returncode == 0 and clean_p_cp.returncode == 0:
        run_improvement_passes(root, output, resultsdir, cratename, tracker)

    # Find the highest numbered output directory and copy its contents
    # to the final output directory.
    highest_out = find_highest_numbered_dir(resultsdir)
    if highest_out is not None:
        shutil.copytree(
            highest_out,
            resultsdir / "final",
        )

    tracker.mark_translation_finished()
    print("Translation finished.")
    print("Collecting static code quality measurements...")
    baseline_metrics = static_measurements_rust.static_rust_metrics(resultsdir / "vanilla_c2rust")
    xj_start_metrics = static_measurements_rust.static_rust_metrics(resultsdir / "00_out")
    xj_final_metrics = static_measurements_rust.static_rust_metrics(resultsdir / "final")

    print("Baseline from upstream c2rust:")
    pprint.pprint(baseline_metrics)

    print("Tenjin's initial, un-improved Rust output:")
    pprint.pprint(xj_start_metrics)

    print("Tenjin's final, improved Rust output:")
    pprint.pprint(xj_final_metrics)

    mb_mut_res = tracker.mb_mut_translation_results()
    if mb_mut_res:
        mb_mut_res.c2rust_baseline = baseline_metrics
        mb_mut_res.tenjin_initial = xj_start_metrics
        mb_mut_res.tenjin_final = xj_final_metrics

    record = tracker.finalize()
    if record is not None:
        with (resultsdir / "translation_metadata.json").open("w") as f:
            f.write(record.to_json(indent=2))

        results_snapshot = create_translation_snapshot(root, codebase, resultsdir, record)

        with (resultsdir / "translation_snapshot.json").open("w") as f:
            f.write(results_snapshot.to_json(indent=2))


def load_and_parse_guidance(guidance_path_or_literal):
    try:
        if guidance_path_or_literal == "":
            guidance = {}
        else:
            guidance = json.loads(guidance_path_or_literal)
    except json.JSONDecodeError:
        guidance = json.load(Path(guidance_path_or_literal).open("r", encoding="utf-8"))
    return guidance


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


def extract_preprocessor_definitions_from_compile_commands(
    parsed_compile_commands: list[dict],
    codebase: Path,
) -> ingest.PerFilePreprocessorDefinitions:
    """Extract preprocessor definitions from `compile_commands.json`"""
    definitions = {}
    for command_info in parsed_compile_commands:
        command_str = command_info.get("command", "")
        # command_info["directory"] is build directory, which can be
        # located anywhere; it has no relation to the source file path.
        relative_path = Path(command_info.get("file", "")).relative_to(codebase)
        defs: list[ingest.PreprocessorDefinition] = []
        args = shlex.split(command_str)
        i = 0
        while i < len(args):
            arg = args[i]
            i += 1
            if arg == "-D" and i + 1 < len(args):
                # If we find a -D, the next argument is a definition.
                key, _, value = args[i + 1].partition("=")
                i += 1  # Skip the value
                defs.append((key, value))
            elif arg.startswith("-D") and "=" in arg:
                # Handle -Dkey=value style definitions.
                key, _, value = arg[2:].partition("=")
                defs.append((key, value))
            if arg.startswith("-D"):
                # Handle -Dkey style definitions.
                defs.append((arg[2:], None))  # Add the definition without the -D prefix
        if defs:
            definitions[relative_path.as_posix()] = defs
    return definitions
