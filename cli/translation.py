import shutil
import sys
from pathlib import Path
from subprocess import CompletedProcess, CalledProcessError
import re
import json
import pprint
import time
import uuid
from platform import platform
import hashlib
from os import environ
import tomllib

import click

import compilation_database
from repo_root import find_repo_root_dir_Path, localdir
import provisioning
import ingest
import ingest_tracking
import targets
import hermetic
import vcs_helpers
import cargo_workspace_helpers
import static_measurements_rust
from c_refact_identify_mains import find_main_translation_units
from translation_preparation import run_preparation_passes
from translation_improvement import run_improvement_passes
from constants import XJ_GUIDANCE_FILENAME


def stub_ingestion_record(codebase: Path, guidance: dict) -> ingest.TranslationRecord | None:
    """
    Create a stub IngestionRecord for the given codebase and crate name.
    This is used to initialize the ingestion record before the actual translation.
    """

    codebase_vcs_dir = vcs_helpers.find_containing_vcs_dir(codebase)
    if codebase_vcs_dir is None:
        return None

    codebase_wcs = vcs_helpers.vcs_working_copy_status(codebase_vcs_dir)
    if codebase_wcs.origin is None or codebase_wcs.commit is None:
        return None

    tenjin_vcs_dir = vcs_helpers.find_containing_vcs_dir(find_repo_root_dir_Path())
    assert tenjin_vcs_dir is not None, "No VCS directory found for Tenjin?!?!"

    tenjin_wcs = vcs_helpers.vcs_working_copy_status(tenjin_vcs_dir)

    assert tenjin_wcs.origin is not None, "Tenjin working copy has no origin URL?!?"
    assert tenjin_wcs.commit is not None, "Tenjin working copy has no commit hash?!?"

    assert codebase_wcs.origin is not None, "Codebase working copy has no origin URL?!?"
    assert codebase_wcs.commit is not None, "Codebase working copy has no commit hash?!?"

    codebase_relative_path = codebase
    if codebase_relative_path.is_absolute() and codebase_vcs_dir is not None:
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
        try:
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
        except CalledProcessError as e:
            step.update_err(e)
            raise

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
        assert not subdir.is_file()
        if subdir.is_dir():
            rust_snapshots.append(create_subdirectory_snapshot(True, subdir, dirname))

    results_snapshot = ingest.TranslationResultsSnapshot(
        for_translation=record.translation_uuid,
        c_versions=[c_snapshot],
        rust_versions=rust_snapshots,
    )

    return results_snapshot


def apply_behind_the_scenes_guidance_to(guidance: dict) -> dict:
    # Some bits of guidance are internal to Tenjin and not exposed to users.

    if "vars_of_type" not in guidance:
        guidance["vars_of_type"] = {}

    guidance["vars_of_type"]["&mut XjGlobals"] = ["*:xjg"]
    return guidance


def do_translate(
    root: Path,
    codebase: Path,
    resultsdir: Path,
    cratename: str,
    guidance_path_or_literal: str,
    buildcmd: str | None = None,
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

    skip_remainder_of_translation = False

    resultsdir = resultsdir.resolve()
    resultsdir.mkdir(parents=True, exist_ok=True)

    # Preparation passes may modify the guidance stored in XJ_GUIDANCE_FILENAME
    final_prepared_codebase, build_info = run_preparation_passes(
        codebase, resultsdir, tracker, guidance, buildcmd
    )

    c2rust_transpile_flags = [
        "--translate-const-macros",
        "conservative",
        "--reduce-type-annotations",
        "--disable-refactoring",
    ]

    saw_binaries = False
    for target in build_info.get_all_targets():
        print("Found build target:", target)
        if target.type == targets.TargetType.EXECUTABLE:
            target_compdb = build_info.compdb_for_target_within(
                target.name, final_prepared_codebase
            )
            ccs_with_main_funcs = find_main_translation_units(target_compdb)
            assert len(ccs_with_main_funcs) == 1, f"Expected one main function in target {target}"
            # We eventually want to use target.stem as the name of the Rust binary,
            # but for now c2rust only takes the file name, and uses it as the binary name.
            for cmd in ccs_with_main_funcs:
                c2rust_transpile_flags.extend([
                    "--binary",
                    cmd.absolute_file_path.stem,
                ])

            saw_binaries = True

    if not saw_binaries:
        c2rust_transpile_flags.extend(["--emit-build-files"])

    # Preparation passes can modify the guidance, so we re-load it here.
    xj_c2rust_transpile_flags = [
        *c2rust_transpile_flags,
        "--log-level",
        "INFO",
        "--guidance",
        json.dumps(json.load((final_prepared_codebase / XJ_GUIDANCE_FILENAME).open())),
    ]

    # The crate name that c2rust uses is based on the directory stem,
    # so we (temporarily) create a subdirectory with the desired crate name.
    output = resultsdir / cratename
    output.mkdir(parents=True, exist_ok=False)

    compdb = final_prepared_codebase / "compile_commands.json"
    build_info.compdb_for_all_targets_within(final_prepared_codebase).to_json_file(compdb)

    # We must explicitly pass c2rust our sysroot
    compilation_database.munge_compile_commands_for_hermetic_translation(compdb)

    click.echo("Running upstream c2rust translation...")
    # First run the upstream c2rust tool to get a baseline translation.
    run_upstream_c2rust(tracker, c2rust_transpile_flags, compdb, output)

    output = output.rename(output.with_name("vanilla_c2rust"))

    # After upstream c2rust finishes, we can munge the compilation database
    # to make Tenjin-specific tweaks to the compilation process.
    compilation_database.munge_compile_commands_for_tenjin_translation(compdb)

    # Then run our version, using guidance and preanalysis.
    output = resultsdir / cratename
    output.mkdir(parents=True, exist_ok=False)
    target_subdir = environ.get("XJ_BUILD_RS_PROFILE", "debug")
    c2rust_bin = root / "c2rust" / "target" / target_subdir / "c2rust"
    try:
        click.echo("Running Tenjin's c2rust translation...")
        _xj_cp = run_c2rust(
            tracker, "xj-c2rust", c2rust_bin, compdb, output, xj_c2rust_transpile_flags
        )

        fixup_binary_crates_in_workspace(output, cratename)

        # Normalize the unmodified translation results to end up
        # in a directory with a project-independent name.
        output = output.rename(output.with_name("00_out"))
    except CalledProcessError as e:

        def oops(msg: str):
            click.secho("TENJIN: " + msg, err=True, fg="red", bg="white")

        oops(
            f"Tenjin's initial production of Rust via c2rust failed with error code {e.returncode}."
        )
        oops("The command we ran was:")
        click.echo(" ".join(e.cmd))
        oops("    but note that it was invoked in a modified environment.")
        oops("The compilation database was:")
        click.echo(compdb.read_text(encoding="utf-8"))
        oops("Here was stdout:")
        click.echo(e.stdout)
        oops("and stderr:")
        click.echo(e.stderr)

        tracker.mark_translation_finished()
        skip_remainder_of_translation = True

    if not skip_remainder_of_translation:
        # Verify that the initial translation is valid Rust code.
        # If it has errors, we won't be able to run the improvement passes.
        initial_cp = hermetic.run_cargo_on_translated_code(["check"], cwd=output, check=False)
        # Ensure that subsequent passes start with a clean slate.
        clean_p_cp = hermetic.run_cargo_on_translated_code(
            ["clean", *cargo_workspace_helpers.flags_for_all_cargo_workspace_packages(output)],
            cwd=output,
            check=False,
        )

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
        baseline_metrics = static_measurements_rust.static_rust_metrics(
            resultsdir / "vanilla_c2rust"
        )
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


def run_upstream_c2rust(tracker, c2rust_transpile_flags, compdb, output):
    upstream_c2rust_bin = localdir() / "upstream-c2rust" / "target" / "debug" / "c2rust"
    try:
        _up_cp = run_c2rust(
            tracker,
            "upstream-c2rust",
            upstream_c2rust_bin,
            compdb,
            output,
            c2rust_transpile_flags,
        )
    except CalledProcessError as e:

        def oops(msg: str):
            click.secho("TENJIN: " + msg, err=True, fg="red", bg="white")

        oops(f"Upstream c2rust failed with error code {e.returncode}.")
        oops("When upstream c2rust cannot translate a codebase, it's very unlikely that Tenjin")
        oops("will succeed, so there's not much we can do.")
        oops("The command we ran was:")
        click.echo(" ".join(e.cmd))
        oops("    but note that it was invoked in a modified environment.")
        oops("The compilation database was:")
        click.echo(compdb.read_text(encoding="utf-8"))
        oops("Here was stdout:")
        click.echo(e.stdout)
        oops("and stderr:")
        click.echo(e.stderr)

        sys.exit(1)


def load_and_parse_guidance(guidance_path_or_literal: str) -> dict:
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


def fixup_binary_crates_in_workspace(outdir: Path, workspace_cratename: str):
    """
    For a binary crate in a non-trivial workspace setup,
    it ought to `use` the associated library crate name.
    But `c2rust` hardcodes use of the workspace's crate name.
    So we'll do some string munging and fix it up.
    """

    def fixup(rs_path: str, member_cratename: str):
        rs_file = outdir / member_cratename / rs_path
        content = rs_file.read_text(encoding="utf-8")
        fixed_content = content.replace(
            f"use ::{workspace_cratename}::",
            f"use ::{member_cratename}::",
        )
        rs_file.write_text(fixed_content, encoding="utf-8")

    cargo_toml_path = outdir / "Cargo.toml"
    ct = tomllib.load(cargo_toml_path.open("rb"))
    if "workspace" not in ct:
        return
    member_names = ct["workspace"].get("members", [])
    if len(member_names) <= 1:
        return

    for member in member_names:
        member_path = outdir / member
        if not member_path.is_dir():
            continue
        member_ct_path = member_path / "Cargo.toml"
        if not member_ct_path.is_file():
            continue
        member_ct = tomllib.load(member_ct_path.open("rb"))
        if "bin" in member_ct:
            for rcd in member_ct["bin"]:
                fixup(rcd["path"], member)
