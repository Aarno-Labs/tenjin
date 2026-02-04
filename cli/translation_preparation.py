import os
import json
import shutil
import time
from pathlib import Path
from typing import Callable
from subprocess import CompletedProcess
import hashlib
from collections import defaultdict
import dataclasses
from enum import Enum

from clang.cindex import Cursor, CursorKind  # type: ignore
from cmake_file_api import CMakeProject

import compilation_database
import batching_rewriter
import c_refact
import c_refact_arglifter
import c_refact_decl_splitter
import c_refact_type_mod_replicator
import cindex_helpers
import hermetic
import repo_root
import ingest_tracking
import llvm_bitcode_linking
import targets_from_intercept
from targets import BuildInfo, TargetType
from caching_file_contents import CachingFileContents
from constants import WANT, XJ_GUIDANCE_FILENAME
from tenj_types import FileContentsStr, FilePathStr, RelativeFilePathStr, ResolvedPath


def elapsed_ms_of_ns(start_ns: int, end_ns: int) -> float:
    """Calculate elapsed time in milliseconds from nanoseconds."""
    return (end_ns - start_ns) / 1_000_000.0


def compute_build_info_in(
    builddir: Path,
    codebase: Path,
    buildcmd: str | list[str] | None,
    tracker: ingest_tracking.TimingRepo,
    mut_build_info: BuildInfo,
):
    """Builds the codebase in the given directory,
    intercepting compile and link commands.

    To account for builds that generate files in the source tree, such as `Vim`,
    any newly created files ending in `.h`, `.c`, or `.inc` will be copied back
    to the original codebase. Otherwise, later builds would fail due to missing files.

    Postcondition: `builddir` exists.
    """
    buildcmds = codebase / ".xj-build-commands"

    def convert_intercepted_commands_to_build_info():
        entries = []
        for json_file in buildcmds.glob("*.json"):
            with open(json_file, "r", encoding="utf-8") as f:
                entry = json.load(f)
                # if entry["type"] != "cc":
                #     continue  # FIXME
                entries.append(entry)
        intercepted_commands = targets_from_intercept.convert_json_entries(entries)

        mut_build_info.set_intercepted_commands(intercepted_commands)

    provided_compdb = codebase / "compile_commands.json"
    provided_cmakelists = codebase / "CMakeLists.txt"

    cc_ld_intercept_dir = repo_root.find_repo_root_dir_Path() / "cli" / "sh" / "cc-ld-intercept"

    if provided_cmakelists.exists() and not provided_compdb.exists():
        # If we have a CMakeLists.txt, we can generate the compile_commands.json
        cmake_project = CMakeProject(str(builddir), str(codebase), api_version=1)
        cmake_project.cmake_file_api.instrument_all()
        cc_launcher = str(cc_ld_intercept_dir / "cc")
        ld_launcher = str(cc_ld_intercept_dir / "ld")
        cmake_preset = os.environ.get("XJ_CMAKE_PRESET", "")
        cp = hermetic.run(
            [
                "cmake",
                "-S",
                str(codebase),
                "-B",
                str(builddir),
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                f"-DCMAKE_C_COMPILER_LAUNCHER={cc_launcher}",
                f"-DCMAKE_C_LINKER_LAUNCHER={ld_launcher}",
                *(["--preset", cmake_preset] if cmake_preset else []),
            ],
            check=True,
            # capture_output=True,
        )
        tracker.update_sub(cp)

        # Build the project to ensure all compile and link commands are intercepted.
        cp2 = hermetic.run(
            [
                "cmake",
                "--build",
                str(builddir),
                "--parallel",
            ],
            check=True,
            env_ext={
                "BUILD_COMMANDS_DIRECTORY": str(buildcmds),
            },
            # capture_output=True,
        )
        tracker.update_sub(cp2)
        convert_intercepted_commands_to_build_info()
        assert not mut_build_info.is_empty(), "Failed to intercept commands from CMake build"
        return

    if not buildcmd:
        # If the codebase is a C file, or a directory containing a single C file,
        # we assume it's the intended source file to compile.

        if codebase.is_file() and codebase.suffix == ".c":
            c_file = codebase
        else:
            c_files = list(codebase.glob("*.c"))
            if len(c_files) != 1:
                raise ValueError(
                    f"Unable to build multi-C-file {codebase} without CMakeLists.txt or buildcmd"
                )
            c_file = c_files[0]

        # TODO allow non-exe compilation via checking for main fn, then update docs/USE.md
        buildcmd = [
            "clang",
            "-o",
            f"{c_file.stem}.exe",
            c_file.as_posix(),
        ]
    assert buildcmd is not None
    # Invoke the build command from a temporary directory with a copy of the
    # input codebase, logging the intercepted commands.
    shutil.copytree(codebase, builddir, dirs_exist_ok=True)

    hermetic.run(
        buildcmd,
        cwd=builddir,
        shell=isinstance(buildcmd, str),
        check=True,
        env_ext={
            "BUILD_COMMANDS_DIRECTORY": str(buildcmds),
            "pre-Tenjin PATH prefix": [str(cc_ld_intercept_dir)],
        },
    )
    copy_new_source_files_back(codebase, builddir)
    convert_intercepted_commands_to_build_info()
    assert not mut_build_info.is_empty(), "Failed to intercept commands from build"


def copy_new_source_files_back(
    codebase: Path,
    builddir: Path,
):
    """Copy newly created source files from the build directory back to the original codebase."""
    for new_file in builddir.rglob("*.*"):
        if new_file.suffix not in {".h", ".c", ".inc"}:
            continue
        relative_path = new_file.relative_to(builddir)
        dest_file = codebase / relative_path
        if not dest_file.exists():
            dest_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(new_file, dest_file)


def copy_codebase(pristine: Path, newdir: Path):
    """Copy the original codebase (src or directory) to a new directory."""
    if pristine.is_file():
        newdir.mkdir()
        shutil.copy2(pristine, newdir / pristine.name)
    else:
        copy_codebase_dir(pristine, newdir)


def copy_codebase_dir(
    src: Path,
    dst: Path,
):
    """Copy the original codebase (directory) to a new directory."""
    assert src.is_dir()
    shutil.copytree(src, dst)

    # Remove stale compile_commands.json if present; they should be
    # regenerated in the new location as needed.
    compdb_path = dst / "compile_commands.json"
    if compdb_path.exists():
        compdb_path.unlink()


type QUSS = c_refact_type_mod_replicator.QuasiUniformSymbolSpecifier
type QUSS_is_defn = bool
type QUSS_and_defn = tuple[QUSS, QUSS_is_defn]


class FnDefHandling(Enum):
    EXCLUDE = 1
    INCLUDE_BODY = 2
    INCLUDE_DECL_ONLY = 3


def function_signature_span_end(cursor: Cursor) -> int:
    """Visual example of return value:
    ```
        void f(int a, int b)  { ... }
                            ^
    ```
    """
    cursor_end_offset = cursor.extent.end.offset
    # Do not include the function body in the recorded declaration.
    prev_tok = None
    for t in cursor.get_tokens():
        if t.location.offset >= cursor.location.offset:
            # This will break on pernicious code which defines a macro
            # to hide the opening curly brace of the function body.
            if t.spelling == "{":
                if prev_tok:
                    # Probably a ) before { but it could be
                    # an attribute or an #endif or a non-expanding macro...)
                    cursor_end_offset = prev_tok.extent.end.offset
                else:
                    cursor_end_offset = t.extent.start.offset
                break
            prev_tok = t
    return cursor_end_offset


def collect_decls_by_rel_tu(
    current_codebase: Path,
    compdb: compilation_database.CompileCommands,
    restricted_to_files: set[FilePathStr] | None = None,
    fn_def_handling: FnDefHandling = FnDefHandling.EXCLUDE,
) -> dict[
    RelativeFilePathStr,
    dict[QUSS, list[tuple[RelativeFilePathStr, int, int, FileContentsStr, QUSS_is_defn]]],
]:
    """Collect declarations from the codebase's translation units,
    indexed by path relative to the codebase root.

    The caller may specify a set of files to restrict collection to,
    so as to e.g. only collect declarations from (a subset of) headers.
    The caller may also specify whether functions should be omitted,
    or included with or without their bodies.
    """

    def path_of_interest(p: FilePathStr) -> bool:
        if not Path(p).is_relative_to(current_codebase):
            assert not Path(p).is_relative_to(current_codebase.parent), (
                f"Unexpected path: {p} not relative to {current_codebase=}"
            )
            return False
        return restricted_to_files is None or p in restricted_to_files

    header_contents = CachingFileContents()

    decls_by_rel_tu: dict[
        RelativeFilePathStr,
        dict[QUSS, list[tuple[RelativeFilePathStr, int, int, FileContentsStr, QUSS_is_defn]]],
    ] = {}

    # Note that the two FilePathStrs here can be different,
    # e.g. the declaration can be in a header file included by the TU file.

    index = c_refact.create_xj_clang_index()
    tus = c_refact.parse_project(index, compdb)
    for tu_path, tu in tus.items():
        assert Path(tu_path).is_relative_to(current_codebase), (
            f"Unexpected TU path: {tu_path} not relative to {current_codebase=}\n{compdb=}"
        )
        rel_tu_path = Path(tu_path).relative_to(current_codebase).as_posix()
        macro_inst_ranges: dict[tuple[int, FilePathStr], int] = {}
        for cursor in tu.cursor.get_children():
            if cursor.kind == CursorKind.MACRO_INSTANTIATION:
                inst_loc = (cursor.extent.start.offset, cursor.location.file.name)
                macro_inst_ranges[inst_loc] = cursor.extent.end.offset
                continue

            # print(
            #     "PPRC: Visiting cursor:",
            #     cursor.kind,
            #     cursor.spelling,
            #     "is_definition=",
            #     cursor.is_definition(),
            #     fn_def_handling,
            # )

            # When we run this pass before expanding the preprocessor,
            # cursor.location can reflect header file locations.
            if (
                cursor.kind.is_declaration()
                and cursor.location.file
                and path_of_interest(cursor.location.file.name)
            ):
                q = c_refact_type_mod_replicator.quss(cursor, None)
                # print("Recording declaration:", cursor.spelling)
                # print("   Kind:", cursor.kind)
                # print("   QUSS:", q)
                # print("   From location:", cursor.location.file.name)
                # print("   found in TU:", tu_path)
                cursor_end_offset = cursor.extent.end.offset
                if cursor.kind == CursorKind.FUNCTION_DECL and cursor.is_definition():
                    if fn_def_handling == FnDefHandling.EXCLUDE:
                        continue
                    if fn_def_handling == FnDefHandling.INCLUDE_DECL_ONLY:
                        cursor_end_offset = function_signature_span_end(cursor)
                else:
                    # Include macro instantiations adjacent to the end of the definition.
                    # This is intended to handle cases like the argument list of a function
                    # declaration being wrapped in a macro, as seen in `zlib.h`.
                    # It will break on code which hides declaration separators inside macro expansions.
                    cursor_end_loc = (cursor_end_offset, cursor.location.file.name)
                    if cursor_end_loc in macro_inst_ranges:
                        cursor_end_offset = macro_inst_ranges[cursor_end_loc]

                relative = Path(cursor.location.file.name).relative_to(current_codebase)
                decls_by_rel_tu.setdefault(rel_tu_path, {}).setdefault(q, []).append((
                    relative.as_posix(),
                    cursor.extent.start.offset,
                    cursor_end_offset,
                    header_contents.get_bytes(cursor.location.file.name)[
                        cursor.extent.start.offset : cursor_end_offset
                    ].decode("utf-8"),
                    cursor.is_definition(),
                ))
    return decls_by_rel_tu


def organize_decls_by_headers(
    decls_by_rel_tu: dict[
        RelativeFilePathStr,
        dict[QUSS, list[tuple[FilePathStr, int, int, FileContentsStr, QUSS_is_defn]]],
    ],
) -> dict[FilePathStr, dict[QUSS, list[tuple[int, int, FileContentsStr, QUSS_is_defn]]]]:
    if not decls_by_rel_tu:
        return {}

    # decls_by_tu is indexed by TU path, and can contain entry[0] values that are not TU paths,
    # e.g. header file paths.
    #
    # We want to construct a mapping indexed by header file path,
    # containing entries for declarations that are consistent across all TUs
    # (in which those decls appear, at least).
    every_quss: set[QUSS] = set()
    for rel_tu_path, tu_decls in decls_by_rel_tu.items():
        tu_quss = set(tu_decls.keys())
        every_quss = every_quss.union(tu_quss)

    rel_tu_paths = list(decls_by_rel_tu.keys())
    decls_by_header: dict[
        RelativeFilePathStr, dict[QUSS, list[tuple[int, int, FileContentsStr, QUSS_is_defn]]]
    ] = {}
    for q in every_quss:
        entries_defns = []
        entries_nondefns = []
        for rel_tu_path in rel_tu_paths:
            tu_decls = decls_by_rel_tu[rel_tu_path]
            if q in tu_decls:
                # We have to separate definitions from non-definitions,
                # since their contents are expected to differ, and we
                # only want to compare like with like.
                entries_defns.extend([e for e in tu_decls[q] if e[4]])
                entries_nondefns.extend([e for e in tu_decls[q] if not e[4]])

        expanded_entries_set = expand_overlapping_decl_header_entries(set(entries_defns))
        expanded_nondefns_entries_set = expand_overlapping_decl_header_entries(
            set(entries_nondefns)
        )

        if len(expanded_entries_set) > 1:
            if len(set(e[3] for e in expanded_entries_set)) > 1:
                print("ERROR: Declaration contents differ between TUs for QUSS:", q)
                for rel_tu_path in rel_tu_paths:
                    if q in decls_by_rel_tu[rel_tu_path]:
                        entry = decls_by_rel_tu[rel_tu_path][q]
                        print(f"  In TU {rel_tu_path}: {entry}")
                print()
                # continue  # skip inconsistent declarations
                raise ValueError("ERROR: Declaration contents differ between TUs for QUSS:" + q)

        assert len(expanded_nondefns_entries_set) <= 1

        # All declarations have same contents
        for exp_entry in expanded_entries_set:
            header_path_str = exp_entry[0]
            header_path = Path(header_path_str)
            if header_path.is_absolute():
                raise ValueError(
                    f"Expected relative header path, got absolute path: {header_path_str}"
                )
            decls_by_header.setdefault(header_path_str, {}).setdefault(q, []).append(exp_entry[1:])

        for exp_entry in expanded_nondefns_entries_set:
            header_path_str = exp_entry[0]
            header_path = Path(header_path_str)
            if header_path.is_absolute():
                raise ValueError(
                    f"Expected relative header path, got absolute path: {header_path_str}"
                )
            decls_by_header.setdefault(header_path_str, {}).setdefault(q, []).append(exp_entry[1:])

    return decls_by_header


def expand_overlapping_decl_header_entries(
    entries_set: set[tuple[FilePathStr, int, int, FileContentsStr, QUSS_is_defn]],
) -> set[tuple[FilePathStr, int, int, FileContentsStr, QUSS_is_defn]]:
    """
    Suppose we have a bunch of declarations collected from headers, e.g.:
        "src/spell.c"  => ('<VIM_9_1>/src/globals.h', 37789, 37826, 'EXTERN tabpage_T    *lastused_tabpage')
        "src/window.c" => ('<VIM_9_1>/src/globals.h', 37789, 37826, 'EXTERN tabpage_T    *lastused_tabpage')
        "src/main.c"   => ('<VIM_9_1>/src/globals.h', 37796, 37826,        'tabpage_T    *lastused_tabpage')
    Due to macro usage, main.c sees a declaration whose extent is subsumed by the others.
    We want to expand what we consider to be its declaration to match the others.
    More generally, when two declarations overlap, we want to expand them to the largest
    declaration that subsumes them both.

    The input `entries_set` is the set of the tuples as shown (of size 2 in this example).
    """
    if len(entries_set) <= 1:
        return entries_set

    by_header: dict[
        FilePathStr, list[tuple[FilePathStr, int, int, FileContentsStr, QUSS_is_defn]]
    ] = {h: [] for h in set(e[0] for e in entries_set)}
    for entry in entries_set:
        by_header[entry[0]].append(entry)

    expanded_entries = set()

    for header_path, header_entries in by_header.items():
        # Sort entries within each header by start offset, so that overlaps will be adjacent.
        header_entries.sort(key=lambda e: (e[1], e[2]))

        overlap_buckets = [[header_entries[0]]]
        for entry in header_entries[1:]:
            last_bucket = overlap_buckets[-1]
            last_entry = last_bucket[-1]
            if entry[1] < last_entry[2]:
                # Overlaps with last entry, add to current bucket
                last_bucket.append(entry)
            else:
                # No overlap, start a new bucket
                overlap_buckets.append([entry])

        for bucket_list in overlap_buckets:
            if len(bucket_list) <= 1:
                continue  # no overlaps to expand

            # Find the min start and max end offsets. We can get away with this simple
            # approach because everything in the bucket pairwise overlaps.
            min_start = min(entry[1] for entry in bucket_list)
            max_end = max(entry[2] for entry in bucket_list)
            is_defn = any(entry[4] for entry in bucket_list)

            # In general, no single entry will have the complete text contents, so we
            # may need to reconstruct it character-by-character.
            full_text = ""
            for entry in bucket_list:
                if entry[1] == min_start and entry[2] == max_end:
                    full_text = entry[3]
                    break
            if not full_text:
                full_text_chars = {}
                for entry in bucket_list:
                    entry_start = entry[1]
                    entry_end = entry[2]
                    entry_text = entry[3]
                    for i in range(entry_start, entry_end):
                        char = entry_text[i - entry_start]
                        full_text_chars[i] = char
                full_text = "".join(full_text_chars[i] for i in range(min_start, max_end))

            # Now all the entries in this bucket collapse to a single full range entry.
            expanded_entries.add((header_path, min_start, max_end, full_text, is_defn))

    return expanded_entries


@dataclasses.dataclass
class PrepPassResultStore:
    items_defined_by_headers: dict[
        RelativeFilePathStr, dict[QUSS, list[tuple[int, int, FileContentsStr, QUSS_is_defn]]]
    ]
    items_defined_after_pp: dict[
        RelativeFilePathStr,
        dict[QUSS, list[tuple[FilePathStr, int, int, FileContentsStr, QUSS_is_defn]]],
    ]
    build_info: BuildInfo


def run_preparation_passes(
    original_codebase: Path,
    resultsdir: Path,
    tracker: ingest_tracking.TimingRepo,
    guidance: dict,
    do_not_refactor_headers_within: list[ResolvedPath],
    buildcmd: str | None = None,
) -> tuple[Path, BuildInfo]:
    """Returns the path to the final prepared codebase directory,
    along with information about the build structure."""

    def can_refactor_header(p: Path) -> bool:
        rp = p.resolve()
        return not any(rp.is_relative_to(excluded) for excluded in do_not_refactor_headers_within)

    def prep_00_copy_pristine_codebase(pristine: Path, newdir: Path, store: PrepPassResultStore):
        copy_codebase(pristine, newdir)

    def prep_01_intercept_build(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        builddir = resultsdir / "_build_1"
        compute_build_info_in(builddir, current_codebase, buildcmd, tracker, store.build_info)

        merged_defs = defaultdict(list)

        for target in store.build_info.get_all_targets():
            compdb_for_target = store.build_info.compdb_for_target_within(
                target.name, current_codebase
            )
            defs = compilation_database.extract_preprocessor_definitions_from_compile_commands(
                compdb_for_target,
                current_codebase,
            )
            for k, v in defs.items():
                merged_defs[k].extend(v)

        tracker.set_preprocessor_definitions(dict(merged_defs))

        # Writing out the user-provided guidance can go pretty much wherever in the early stages.
        json.dump(
            guidance,
            open(current_codebase / XJ_GUIDANCE_FILENAME, "w", encoding="utf-8"),
            indent=2,
        )

    def prep_02_build_coverage(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        # Rebuild all targets with coverage instrumentation flags.

        # Note that this will compile the source files from the current_codebase,
        # but the built artifacts will remain in the original build directory.
        profile_compdb = store.build_info.compdb_for_profiled_build(current_codebase)

        original_builddir = resultsdir / "_build_1"
        # Since we want to preserve the original build directory contents,
        # we'll first copy it to a new location. Then we'll re-execute the
        # intercepted build commands, and then swap out the resulting
        # artifacts, restoring the original build directory contents.
        preserved_builddir = original_builddir.with_name("_build_1_preserved")
        shutil.copytree(original_builddir, preserved_builddir, dirs_exist_ok=True)

        cmd_for_output: dict[str, compilation_database.CompileCommand] = {}

        def runcmd(cmd: compilation_database.CompileCommand):
            assert cmd.arguments
            hermetic.run(
                cmd.arguments,
                cwd=cmd.directory,
                check=True,
            )
            assert cmd.output is not None
            cmd_for_output[cmd.output] = cmd

        # First, build all object files
        for cmd in profile_compdb.commands:
            if cmd.output is not None and cmd.output.endswith(".o"):
                runcmd(cmd)

        # Then, all shared libraries
        for cmd in profile_compdb.commands:
            if cmd.output is not None and (
                cmd.output.endswith(".so")
                or cmd.output.endswith(".dylib")
                or cmd.output.endswith(".dll")
            ):
                runcmd(cmd)

        # Then all remaining commands, which are assumed to be executables
        for cmd in profile_compdb.commands:
            if cmd.output is not None and cmd.output not in cmd_for_output:
                runcmd(cmd)

        # TODO: run the above steps in parallel batches

        # cov_outputs = set(
        #     cmd.output
        #     for cmd in profile_compdb.commands
        #     if cmd.output is not None and not cmd.output.endswith(".o")
        # )
        cov_output_names = [Path(p).name for p in cmd_for_output.keys()]
        assert len(cov_output_names) == len(set(cov_output_names)), (
            f"Expected unique exe/lib file names, got duplicates: {cov_output_names}"
        )

        built_cov = Path(current_codebase.parent / "_built_cov")
        built_cov.mkdir(exist_ok=True)
        for o, cmd in cmd_for_output.items():
            po = Path(o)
            if not po.is_absolute():
                po = Path(cmd.directory) / o
            assert po.exists(), f"Expected built coverage output file not found: {o}"
            assert po.is_relative_to(original_builddir), (
                f"Expected built coverage output {o} to be within original builddir\n{original_builddir},"
                f" got\n{po}"
            )
            rel = po.relative_to(original_builddir)
            shutil.move(po, built_cov / po.name)
            shutil.move(preserved_builddir / rel, po)

        shutil.rmtree(preserved_builddir)

    def prep_uniquify_built_files(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        # Some codebases will compile the same source file multiple times with different
        # flags. It's really convenient for us if each source file is only compiled once.
        # So we duplicate source files as needed and adjust the compilation database.

        intercepted_commands = store.build_info._intercepted_commands
        outputs = [cmd.output for cmd in intercepted_commands if cmd.output is not None]
        assert len(outputs) == len(set(outputs)), f"Output files are not unique: {outputs}"

        # Step 1: Find files which are compiled multiple times
        cc_commands_for_path: defaultdict[Path, list[targets_from_intercept.InterceptedCommand]] = (
            defaultdict(list)
        )
        for cmd in intercepted_commands:
            if not cmd.compile_only:
                continue
            inp = cmd.c_inputs[0] if len(cmd.c_inputs) == 1 else None
            assert inp, f"Expected single C input file for compilation command {cmd}"

            stale_abs_path = cmd.abs_path(Path(inp))
            cc_commands_for_path[stale_abs_path].append(cmd)

        # Step 2: Duplicate files in current_codebase, duplicate commands (but with stale refs).
        new_commands = []
        for stale_abs_path, cmds in cc_commands_for_path.items():
            stale_abs_path = Path(stale_abs_path)
            if len(cmds) == 1:
                # Only one command for this file, keep as-is
                new_commands.append(cmds[0])
                continue

            # Multiple commands for same file - need to duplicate all of them
            for idx, cmd in enumerate(cmds):
                # Create unique file name: foo.c -> foo_xjdup_0.c, foo_xjdup_1.c, ...
                stem = stale_abs_path.stem
                suffix = stale_abs_path.suffix
                new_filename = f"{stem}_xjdup_{idx}{suffix}"
                stale_dedup_path = stale_abs_path.parent / new_filename
                curr_dedup_path = current_codebase / new_filename

                # Copy the source file to the new file.
                assert stale_abs_path.exists()
                shutil.copyfile(stale_abs_path, curr_dedup_path)

                # Cache this to reduce the number of repeated resolve() calls.
                resolved_stale_path = stale_abs_path.resolve()

                def update_arg(arg: str) -> str:
                    try:
                        if cmd.abs_path(Path(arg)).resolve() == resolved_stale_path:
                            return stale_dedup_path.as_posix()
                    except OSError:
                        # Path resolution can fail for non-existent paths
                        pass
                    return arg

                new_commands.append(cmd.fmap_input_paths(update_arg))

        # compile_commands_for_path omits fake link thingy commands,
        # so we need to add them back in.
        for cmd in intercepted_commands:
            if not cmd.compile_only:
                new_commands.append(cmd)

        store.build_info.set_intercepted_commands(new_commands)

        # Synthesize new compile_commands.json in current_codebase.
        # (this is mostly for debugging purposes, tested in the triplicated smoke test)
        store.build_info.compdb_for_all_targets_within(current_codebase).to_json_file(
            current_codebase / "compile_commands.json"
        )

    def prep_localize_mutable_globals(
        prev: Path, current_codebase: Path, store: PrepPassResultStore
    ):
        # XREF:NON_TRIVIAL_REFACTORING_PRECONDITIONS
        # Preconditions for localizing mutable globals:
        # A. The project must consist of a single executable target, OR
        # B. The project must have one or more executable targets, each of which
        #    has completely disjoint modifiable input files (including headers).
        #
        # In case A, we are free to update all local files as needed.
        #
        # In case B, we cannot update headers shared between executable and
        # library targets, because we cannot generally localize globals in
        # shared libraries which need to maintain a stable C ABI.
        #
        # We could in principle modify headers shared only between executable targets, but
        # only if every target makes the same modifications to those headers.
        all_targets = store.build_info.get_all_targets()
        if len(all_targets) == 1 and all_targets[0].type == TargetType.EXECUTABLE:
            # Case A
            compdb = store.build_info.compdb_for_target_within(
                all_targets[0].name, current_codebase
            )

            c_refact.localize_mutable_globals(
                current_codebase / "xj-cclyzer.json", compdb, prev, current_codebase
            )
        else:
            # Case B
            print(
                "TENJIN: NOTE: Skipping localization of mutable globals for multi-target codebase."
            )

    def prep_lift_subfield_args(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        # Lifting of subfield arguments is idempotent, so we apply it to all targets
        # without concern for overlapping source files.
        for target in store.build_info.get_all_targets():
            compdb_for_target = store.build_info.compdb_for_target_within(
                target.name, current_codebase
            )
            c_refact_arglifter.lift_subfield_args(compdb_for_target)

    def run_cc2json_or_cached(bitcode_module_path: Path, current_codebase: Path) -> None:
        assert bitcode_module_path.exists()

        cp = hermetic.run(
            ["llvm-nm", str(bitcode_module_path)],
            capture_output=True,
            check=True,
        )

        main_func_defined = cp.stdout.decode().find(" T main") != -1
        # TODO: could also accept guidance to override this decision
        internalize_globals_flag = ["--internalize-globals"] if main_func_defined else []

        json_out_path = current_codebase / "xj-cclyzer.json"

        cache_relevant_cc2json_flags = [
            "--datalog-analysis=unification",
            "--debug-datalog=false",
            "--context-sensitivity=insensitive",
            "--entrypoints=library",  # consider all functions to be reachable
            *internalize_globals_flag,
        ]

        bitcode_hash = hashlib.sha256(bitcode_module_path.read_bytes()).hexdigest()
        cache_signature = [bitcode_hash, WANT["10j-more-deps"], *cache_relevant_cc2json_flags]

        xj_cclyzer_results_cache_path = repo_root.localdir() / "xj-cclyzer-cache.json"
        if xj_cclyzer_results_cache_path.exists():
            cache_data = json.load(open(xj_cclyzer_results_cache_path, "r", encoding="utf-8"))
            print("cached  signature:", cache_data["signature"])
            print("current signature:", cache_signature)
            if cache_data["signature"] == cache_signature:
                print("Reusing cached cclyzer++ analysis results...")
                json.dump(
                    cache_data["contents"], open(json_out_path, "w", encoding="utf-8"), indent=2
                )

                return
            if cache_data["signature"][0] == cache_signature[0]:
                print(
                    "Bitcode matches cached cclyzer++ results, but flags differ; recomputing analysis..."
                )
            else:
                print("Bitcode differs from cached cclyzer++ results; recomputing analysis...")

        print("Running cclyzer++ analysis, this can take a while for larger programs...")
        hermetic.run_command_with_progress(
            [
                "cc2json-llvm14",
                str(bitcode_module_path),
                *cache_relevant_cc2json_flags,
                f"--json-out={json_out_path}",
            ],
            current_codebase / "xj-cc2json-stdout.txt",
            current_codebase / "xj-cc2json-stderr.txt",
            # check=True,
            env_ext={"XJ_USE_LLVM14": "1"},
        )

        contents = json.load(open(json_out_path, "r", encoding="utf-8"))
        json.dump(
            {"signature": cache_signature, "contents": contents},
            open(xj_cclyzer_results_cache_path, "w", encoding="utf-8"),
            indent=2,
        )

    def prep_run_cclzyerpp_analysis(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        # For now, we restrict analysis to single-target projects,
        # although this is not a fundamental limitation.
        all_build_targets = store.build_info.get_all_targets()
        if len(all_build_targets) != 1:
            print("TENJIN: NOTE: Skipping cclyzer++ analysis for multi-target codebase.")
            return

        curr_compdb = store.build_info.compdb_for_target_within(
            all_build_targets[0].name, current_codebase
        )
        print("compdb for target:", all_build_targets[0].name)
        print(curr_compdb)
        print()
        print()
        print(store.build_info)

        # Compile and link LLVM bitcode module
        bitcode_module_path = current_codebase / "linked_module.bc"

        llvm_bitcode_linking.compile_and_link_bitcode(
            curr_compdb, bitcode_module_path, use_llvm14=True
        )

        run_cc2json_or_cached(bitcode_module_path, current_codebase)

    def prep_uniquify_statics(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        # For now, we restrict analysis to single-target projects,
        # although this is not a fundamental limitation.
        all_build_targets = store.build_info.get_all_targets()
        if len(all_build_targets) != 1:
            print("TENJIN: NOTE: Skipping static-uniquification for multi-target codebase.")
            return

        compdb = store.build_info.compdb_for_target_within(
            all_build_targets[0].name, current_codebase
        )

        all_pgs_cursors = c_refact.compute_globals_and_statics_for_project(
            compdb, statics_only=True
        )
        all_pgs = [c_refact.mk_NamedDeclInfo(c) for c in all_pgs_cursors]
        current_codebase_dir = current_codebase.as_posix()
        for g_s in all_pgs:
            assert g_s.file_path is not None, f"Expected file_path for global/static: {g_s}"
            assert g_s.file_path.startswith(current_codebase_dir)

        all_global_names = set(g_s.spelling for g_s in all_pgs)
        uniquifiers: dict[str, int] = {}

        def mk_unique_name(base: str) -> str:
            while True:
                n = uniquifiers.get(base, 0)
                uniquifiers[base] = n + 1
                candidate = f"{base}_xjtr_{n}"
                if candidate not in all_global_names:
                    return candidate

        rewrites_per_file: dict[str, dict[int, tuple[int, str, str]]] = {}
        pgs_in_determinstic_order = sorted(
            all_pgs, key=lambda g: (g.file_path or "", g.decl_start_byte_offset)
        )
        for g_s in pgs_in_determinstic_order:
            rewrites_per_file.setdefault(g_s.file_path or "", {})[g_s.decl_start_byte_offset] = (
                g_s.decl_end_byte_offset,
                mk_unique_name(g_s.spelling),
                g_s.spelling,
            )

        for srcfile, editdict in rewrites_per_file.items():
            contents = Path(srcfile).read_bytes()
            edits = []
            for decl_start_byte_offset, (
                decl_end_byte_offset,
                new_name,
                old_name,
            ) in editdict.items():
                name_bytes = old_name.encode("utf-8")

                def find_variant(name_bytes, prefix_bytes: bytes) -> int:
                    idx = contents.find(
                        prefix_bytes + name_bytes, decl_start_byte_offset, decl_end_byte_offset
                    )
                    if idx != -1:
                        return idx + len(prefix_bytes)
                    return -1

                # The variable name might occur in the type name, so we search for it
                # prefixed by something that would count as a token separator.
                separators = [b" ", b"*", b"&", b"\n", b"\t", b"(", b")", b"[", b"]", b","]
                name_byte_offset = -1
                for sep in separators:
                    name_byte_offset = find_variant(name_bytes, sep)
                    if name_byte_offset != -1:
                        break
                if name_byte_offset == -1:
                    # Possibly it's at the start of the range, with a separator after it.
                    if contents.startswith(name_bytes, decl_start_byte_offset):
                        nextbyte = contents[decl_start_byte_offset + len(name_bytes)]
                        if chr(nextbyte).encode() in separators:
                            name_byte_offset = decl_start_byte_offset
                assert name_byte_offset != -1, (
                    f"Could not find bytes for '{old_name}' in source file range: {contents[decl_start_byte_offset:decl_end_byte_offset]!r}"
                )
                edits.extend(["--offset", str(name_byte_offset), "--new-name", new_name])
            hermetic.run(
                [
                    "clang-rename",
                    "-i",  # inplace
                    *edits,
                    srcfile,
                ],
                cwd=current_codebase,
                check=True,
            )

    def prep_split_joined_decls(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        j = c_refact.run_xj_locate_joined_decls(current_codebase, store.build_info)
        c_refact_decl_splitter.apply_decl_splitting_rewrites(current_codebase, j)

    def prep_pre_refold_consolidation(
        prev: Path, current_codebase: Path, store: PrepPassResultStore
    ):
        # store.items_defined_by_headers has, for each header, a collection of
        # (quss-ident, (start_offset, end_offset, source_text, is_defn)) entries.
        # For each translation unit, we'll find the source text of each item
        # and compare with the header's version.
        #   (Assumes that each item is defined in at most one header file,
        #    in at most one place.)
        # If the header's version is different, we'll record that this TU
        # modified the item.
        #
        # If every TU modifies the same header-defined item in the same way,
        # we will replace each TU's version with the header's version,
        # and replace the header's version with the modified version.
        # (The former allows refolding to work better, the latter preserves
        # the intended semantics of the program. We need to handle changes to
        # both declarations and definitions, since headers can define either.)
        #
        # Since we run this pass before refolding, locations refer to .i files,
        # not .c or .h files.

        # XREF:NON_TRIVIAL_REFACTORING_PRECONDITIONS
        all_build_targets = store.build_info.get_all_targets()
        if len(all_build_targets) != 1:
            print("TENJIN: NOTE: Skipping static-uniquification for multi-target codebase.")
            return

        compdb = store.build_info.compdb_for_target_within(
            all_build_targets[0].name, current_codebase
        )

        items_src_by_tu: dict[FilePathStr, dict[QUSS_and_defn, FileContentsStr]] = {}
        tus_modifying_decls: dict[QUSS_and_defn, set[FilePathStr]] = {}

        index = c_refact.create_xj_clang_index()
        tus = c_refact.parse_project(index, compdb)

        # Build a map from QUSS to the expected (original) source text from headers
        qd_to_header_src: dict[tuple[QUSS, bool], FileContentsStr] = {}
        for header_items in store.items_defined_by_headers.values():
            for q, hdr_details in header_items.items():
                for start_offset, end_offset, source_text, _is_defn in hdr_details:
                    qd = (q, _is_defn)
                    qd_to_header_src[qd] = source_text

        def combine_source_texts_for_header(t1: str, t2: str) -> str | None:
            if t1 == t2:
                return t1
            # If one of them ends with an initializer, strip it off.
            t1 = t1.split("=")[0].rstrip()
            t2 = t2.split("=")[0].rstrip()
            # If one is a substring of the other, return the longer one,
            # which probably starts with 'extern'. (Note that, at least in
            # theory, 'extern' can appear anywhere in the declaration specifier
            # list, e.g. after 'const' or 'int').
            if t1 in t2:
                return t2
            if t2 in t1:
                return t1
            # If both end with a close paren, they're likely function declarations.
            # They might differ in presence or absence of 'extern' and/or parameter names,
            # so we'll use Clang to parse them and render a normalized form.
            if t1.endswith(")") and t2.endswith(")"):
                t1_norm = cindex_helpers.render_normalized_declaration(t1)
                t2_norm = cindex_helpers.render_normalized_declaration(t2)
                if t1_norm == t2_norm:
                    return t1_norm
                elif t1_norm.replace("const ", "") == t2_norm.replace("const ", ""):
                    # Ignore 'const' differences in function declarations,
                    # preferring the version with more consts in it.
                    return t1_norm if len(t1_norm) >= len(t2_norm) else t2_norm
                else:
                    print("PPRC: WARNING: Function declaration source differs after normalization:")
                    print("  Source 1:", t1)
                    print("  Source 2:", t2)

                    print("  Normalized 1 sans consts:", t1_norm.replace("const ", ""))
                    print("  Normalized 2 sans consts:", t2_norm.replace("const ", ""))
            return None

        def collect_non_outermost_children(tu_cursor: Cursor) -> set[QUSS]:
            # The children of a translation unit cursor include both top-level
            # declarations and (at least some) nested declarations, such as
            # `typedef struct { ... } Foo;` which produces both a StructDecl
            # and a TypedefDecl as children of the TU cursor.
            #
            # We don't want to generate overlapping edits for nested declarations,
            # which can cause corruption if not handled carefully.
            # Example:
            # - Start with AAABBBBCC where e.g. BBBB is a nested decl within AAAA.
            # - We'll generate simultaneous edits to replace BBBB with XX and
            #   AAABBBB with YYXX.
            # - Since BBBB comes last, we first replace BBBB with XX, yielding AAAXXCC.
            # - Then we try to replace (what used to be) AAABBBB with YYXX,
            #   but the offsets are now wrong, so we end up with YYXX instead of YYXXCC.
            all_children_sorted_by_location = sorted(
                tu_cursor.get_children(), key=lambda c: c.extent.start.offset
            )
            nested_children: set[QUSS] = set()
            for idx in range(len(all_children_sorted_by_location) - 1):
                c1 = all_children_sorted_by_location[idx]
                c2 = all_children_sorted_by_location[idx + 1]
                if c2.extent.start.offset < c1.extent.end.offset:
                    q2 = c_refact_type_mod_replicator.quss(c2, None)
                    nested_children.add(q2)
            return nested_children

        remove_expanded_src_for: set[QUSS_and_defn] = set()
        quss_to_expanded_src: dict[QUSS_and_defn, FileContentsStr] = {}
        for expanded_decls in store.items_defined_after_pp.values():
            for q, exp_details in expanded_decls.items():
                for rel_path, start_offset, end_offset, source_text, _is_defn in exp_details:
                    qd = (q, _is_defn)

                    if q in quss_to_expanded_src and qd not in remove_expanded_src_for:
                        # Already recorded from another TU
                        combined = combine_source_texts_for_header(
                            quss_to_expanded_src[qd], source_text
                        )
                        if combined is None:
                            print(
                                "PPRC: WARNING: Expanded header source differs between TUs for QUSS:",
                                q,
                            )
                            print("  Previous source:", quss_to_expanded_src[qd])
                            print("  New source:     ", source_text)
                            print("    from:", rel_path)
                            remove_expanded_src_for.add(qd)
                        else:
                            quss_to_expanded_src[qd] = combined
                        continue
                    quss_to_expanded_src[qd] = source_text

        for qd in remove_expanded_src_for:
            quss_to_expanded_src.pop(qd, None)

        defn_offsets_by_tu: dict[FilePathStr, dict[QUSS_and_defn, tuple[int, int]]] = {}

        # For each TU, collect the actual source text for each QUSS
        for tu_path, tu in tus.items():
            assert Path(tu_path).is_relative_to(current_codebase)
            rel_tu_path = Path(tu_path).relative_to(current_codebase).as_posix()
            # quss_to_expanded_src: dict[QUSS, FileContentsStr] = {}
            # print(f"PPRC: Analyzing TU for modified declarations: {tu_path}")
            # print(f"PPRC: decls_defined_after_pp has len ({len(store.decls_defined_after_pp)})")
            # for q, header_decl in store.decls_defined_after_pp.get(tu_path, {}).items():
            #     (start_offset, end_offset, source_text) = header_decl
            #     quss_to_expanded_src[q] = source_text

            nested_children = collect_non_outermost_children(tu.cursor)

            tu_file_contents_str = Path(tu_path).read_text(encoding="utf-8")
            for cursor in tu.cursor.get_children():
                if cursor.kind.is_declaration():
                    if (
                        cursor.kind
                        in (
                            CursorKind.STRUCT_DECL,
                            CursorKind.UNION_DECL,
                            CursorKind.ENUM_DECL,
                            CursorKind.TYPEDEF_DECL,
                        )
                        and not cursor.is_definition()
                    ):
                        # Skip forward declarations
                        continue

                    q = c_refact_type_mod_replicator.quss(cursor, None)
                    if q in nested_children:
                        continue

                    qd = (q, cursor.is_definition())
                    if qd in qd_to_header_src:
                        start_offset = cursor.extent.start.offset
                        end_offset = cursor.extent.end.offset

                        newest_tu_src = tu_file_contents_str[start_offset:end_offset]
                        items_src_by_tu.setdefault(tu_path, {})[qd] = newest_tu_src
                        defn_offsets_by_tu.setdefault(tu_path, {})[qd] = (start_offset, end_offset)

                        items_of_like_status = [
                            entry
                            for entry in store.items_defined_after_pp.get(rel_tu_path, {}).get(
                                q, []
                            )
                            if entry[4] == qd[1]
                        ]
                        expanded_tu_src = (
                            items_of_like_status[0][3] if items_of_like_status else None
                        )

                        # Check if TU's version differs from header's expanded version
                        if not expanded_tu_src:
                            print(
                                f"PPRC: WARNING: No expanded header source recorded for QUSS: {q} in TU:",
                                (rel_tu_path in store.items_defined_after_pp),
                                rel_tu_path,
                            )
                        elif newest_tu_src != expanded_tu_src:
                            # print(
                            #     f"PPRC: Found MODIFIED {q} declaration in TU:",
                            #     rel_tu_path,
                            # )
                            # print("PPRC: Source text:", newest_tu_src)
                            # print("PPRC: extent offsets:", start_offset, end_offset)
                            # print("PPRC: extent: ", cursor.extent)
                            # print("expanded_tu_src: ", expanded_tu_src)

                            tus_modifying_decls.setdefault(qd, set()).add(tu_path)

        with batching_rewriter.BatchingRewriter() as rewriter:
            # Remove the forward declaration of 'struct XjGlobals' from the
            # start of each TU when it doesn't seem to be needed.
            for tu_path, tu in tus.items():
                # The trailing newline matters!
                forward_decl = b"struct XjGlobals;\n"
                fwd_decl_needed = True
                tu_file_contents = rewriter.get_content(tu_path)
                include_offset = tu_file_contents.find(b'#include "xj_globals.h"')
                if include_offset == -1:
                    fwd_decl_needed = tu_file_contents.count(b"XjGlobals") > 1
                else:
                    fwd_decl_needed = tu_file_contents[0:include_offset].count(b"XjGlobals") > 1

                if not fwd_decl_needed:
                    fwd_decl_offset = tu_file_contents.find(forward_decl)
                    if fwd_decl_offset != -1:
                        rewriter.add_rewrite(
                            tu_path,
                            fwd_decl_offset,
                            len(forward_decl),
                            "",
                        )

            # Find declarations that are modified identically in all TUs
            for qd, modifying_tus in tus_modifying_decls.items():
                q = qd[0]
                # Check if all TUs modify it in the same way
                modified_versions_for_qd = set()
                for tu_path in modifying_tus:
                    if qd in items_src_by_tu[tu_path]:
                        modified_versions_for_qd.add(items_src_by_tu[tu_path][qd])

                if len(modified_versions_for_qd) != 1:
                    print("PPRC: Declaration modified differently between TUs for QUSS:", q)
                    continue  # TUs modify it in different ways

                # All TUs modify this declaration identically
                # Apply the consolidation: replace TU versions with header version,
                # and update header with the modified version
                modified_version = modified_versions_for_qd.pop()
                # original_header_version = qd_to_header_src[q]
                expanded_header_version = quss_to_expanded_src[qd]

                if modified_version == expanded_header_version:
                    # print(
                    #     "PPRC: Skipping consolidation for QUSS, modified version matches expanded header version:",
                    #     q,
                    # )
                    continue  # No change needed

                # print(f"PPRC: Consolidating declaration {q}:")
                # print(f"  Original (header):\n{original_header_version}")
                # print("----------------------------")
                # if original_header_version == expanded_header_version:
                #     print("  Expanded (header): <same as original>")
                # else:
                #     print(f"  Expanded (header):\n{expanded_header_version}")
                # print("----------------------------")
                # if expanded_header_version == modified_version:
                #     print("  Modified (TUs): <same as expanded>")
                # else:
                #     print(f"  Modified (TUs):\n{modified_version}")
                # print("----------------------------")
                # print("Modifying TUs:")
                # print(modifying_tus)
                # print("----------------------------")

                # Replace in each TU: modified_version -> expanded_header_version
                for tu_path in modifying_tus:
                    assert tu_path in defn_offsets_by_tu
                    if qd in defn_offsets_by_tu[tu_path]:
                        tu_file_path = Path(tu_path)
                        # We cannot use simple string replacement here because we
                        # do not want to modify the signatures of function definitions,
                        # only the declarations.
                        start_offset, end_offset = defn_offsets_by_tu[tu_path][qd]
                        length = end_offset - start_offset
                        rewriter.add_rewrite(
                            tu_file_path.as_posix(), start_offset, length, expanded_header_version
                        )

                # Update the header with the modified version
                for rel_header_path_str, header_items in store.items_defined_by_headers.items():
                    if q in header_items:
                        for start_offset, end_offset, _contents, entry_is_defn in header_items[q]:
                            # We don't want to overwrite a forward declaration with a definition,
                            # or vice versa.
                            if entry_is_defn != qd[1]:
                                continue

                            # TODO handle nested header locations
                            rel_header_path = Path(rel_header_path_str)
                            assert not rel_header_path.is_absolute(), (
                                f"Expected relative header path {rel_header_path_str}"
                            )
                            header_file_path = current_codebase / rel_header_path
                            assert header_file_path.exists()
                            length = end_offset - start_offset
                            print(
                                "PPRC: Updating header:",
                                header_file_path,
                                " offsets:",
                                start_offset,
                                end_offset,
                                "with modified version:",
                                modified_version,
                            )
                            rewriter.add_rewrite(
                                header_file_path.as_posix(), start_offset, length, modified_version
                            )

            # Relocate includes of xj_globals.h from TU to header, if possible.
            for tu_path, tu in tus.items():
                tu_file_contents = rewriter.get_content(tu_path)
                start_include_block = tu_file_contents.find(
                    b"\n// Type definitions needed for XjGlobals"
                )
                end_include_block_tag = b" end include block for XjGlobals */\n"
                end_include_block_start = tu_file_contents.find(
                    end_include_block_tag, start_include_block
                )
                if start_include_block == -1 or end_include_block_start == -1:
                    continue

                target_q_idx = tu_file_contents.rfind(
                    b"@", start_include_block, end_include_block_start
                )
                if target_q_idx == -1:
                    continue

                end_include_block = end_include_block_start + len(end_include_block_tag)
                target_q_end_idx = tu_file_contents.find(b" ", target_q_idx, end_include_block)
                if target_q_end_idx == -1:
                    continue

                # +1 to skip the '@'
                target_q_bytes = tu_file_contents[target_q_idx + 1 : target_q_end_idx]
                target_q = target_q_bytes.decode("utf-8")

                for rel_hdr_path_str, header_decls in store.items_defined_by_headers.items():
                    details_list = header_decls.get(target_q, [])
                    for details in details_list:
                        if not details[3]:
                            # Not a definition
                            continue

                        # Remove the include block from the TU
                        rewriter.add_rewrite(
                            tu_path,
                            start_include_block,
                            end_include_block - start_include_block,
                            "",
                        )
                        # Add the include block to the header
                        q_start_offset, _q_end_offset, _q_contents, _q_is_defn = details
                        rewriter.add_rewrite(
                            (current_codebase / rel_hdr_path_str).as_posix(),
                            q_start_offset,
                            0,
                            tu_file_contents[start_include_block:end_include_block].decode("utf-8"),
                        )
                        break

    def prep_expand_preprocessor(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        # XREF:NON_TRIVIAL_REFACTORING_PRECONDITIONS
        all_build_targets = store.build_info.get_all_targets()
        if len(all_build_targets) != 1:
            print("TENJIN: NOTE: Skipping preprocessor expansion for multi-target codebase.")
            return

        compdb = store.build_info.compdb_for_target_within(
            all_build_targets[0].name, current_codebase
        )

        # We want to capture decl information after the header contents have stabilized,
        # so just before preprocessor expansion is a good time.
        local_header_paths = set(
            p.as_posix() for p in current_codebase.glob("**/*.h") if can_refactor_header(p)
        )

        store.items_defined_by_headers = organize_decls_by_headers(
            collect_decls_by_rel_tu(
                current_codebase,
                compdb,
                restricted_to_files=local_header_paths,
                fn_def_handling=FnDefHandling.INCLUDE_BODY,
            )
        )

        # Miscellaneous tasks over, onwards with preprocessor expansion!
        c_refact.preprocess_build(store.build_info, all_build_targets[0], current_codebase)
        # build_info now marked to use preprocessed files, so re-generate compdb
        new_compdb: compilation_database.CompileCommands = (
            store.build_info.compdb_for_target_within(all_build_targets[0].name, current_codebase)
        )

        for cmd in new_compdb.commands:
            assert cmd.file_path.suffix == ".i", "Expected preprocessed .i files"

        store.items_defined_after_pp = collect_decls_by_rel_tu(
            current_codebase,
            new_compdb,
            fn_def_handling=FnDefHandling.INCLUDE_BODY,
        )
        print(
            f"Collected declarations after preprocessing: {len(store.items_defined_after_pp)} TUs"
        )

    def prep_refold_preprocessor(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        # XREF:NON_TRIVIAL_REFACTORING_PRECONDITIONS
        all_build_targets = store.build_info.get_all_targets()
        if len(all_build_targets) != 1:
            print("TENJIN: NOTE: Skipping preprocessor refolding for multi-target codebase.")
            return

        c_refact.refold_build(store.build_info, all_build_targets[0], current_codebase)
        # build_info now marked to use refolded files, for future steps

    preparation_passes: list[
        tuple[str, Callable[[Path, Path, PrepPassResultStore], CompletedProcess | None]]
    ] = [
        ("copy_pristine_codebase", prep_00_copy_pristine_codebase),
        ("intercept_build", prep_01_intercept_build),
        ("build_coverage", prep_02_build_coverage),
        ("uniquify_built", prep_uniquify_built_files),
        ("split_joined_decls", prep_split_joined_decls),
        ("expand_preprocessor", prep_expand_preprocessor),
        ("uniquify_statics", prep_uniquify_statics),
        ("run_cclzyerpp_analysis", prep_run_cclzyerpp_analysis),
        ("localize_mutable_globals", prep_localize_mutable_globals),
        ("lift_subfield_args", prep_lift_subfield_args),
    ]

    if os.environ.get("XJ_EXTRA_PREPARATION_PASSES") == "1":
        preparation_passes.extend([
            # Refolding and pre-refolding should always go together.
            # They are separate steps to allow inspection of the intermediate codebase.
            ("pre_refold_consolidation", prep_pre_refold_consolidation),
            ("refold_preprocessor", prep_refold_preprocessor),
        ])

    prev = original_codebase.absolute()
    resultsdir_abs = resultsdir.absolute()

    store = PrepPassResultStore(
        items_defined_by_headers={}, items_defined_after_pp={}, build_info=BuildInfo()
    )

    # Note: the original codebase may be a file or directory,
    # but after the first round, `prev` always refers to a directory.
    for counter, (tag, func) in enumerate(preparation_passes, start=0):
        newdir = resultsdir_abs / f"c_{counter:02d}_{tag}"
        with tracker.tracking(f"preparation_pass_{counter:02d}_{tag}", newdir) as step:
            start_ns = time.perf_counter_ns()
            if counter > 0:
                copy_codebase_dir(prev, newdir)
            cp_or_None: CompletedProcess | None = func(prev, newdir, store)
            if cp_or_None is not None:
                step.update_sub(cp_or_None)
            end_ns = time.perf_counter_ns()

            elapsed_ms = round(elapsed_ms_of_ns(start_ns, end_ns))
            print(f"Preparation pass {counter} ({tag}) took {elapsed_ms} ms")

            prev = newdir

    return prev, store.build_info
