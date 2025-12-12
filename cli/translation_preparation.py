import os
import json
import shutil
import time
import tempfile
from pathlib import Path
from typing import Callable
from subprocess import CompletedProcess
import hashlib
from dataclasses import dataclass
from enum import Enum

from clang.cindex import CursorKind  # type: ignore
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
import targets_from_cmake
from caching_file_contents import FilePathStr, CachingFileContents
from constants import WANT, XJ_GUIDANCE_FILENAME


def elapsed_ms_of_ns(start_ns: int, end_ns: int) -> float:
    """Calculate elapsed time in milliseconds from nanoseconds."""
    return (end_ns - start_ns) / 1_000_000.0


def materialize_compilation_database_in(
    builddir: Path,
    codebase: Path,
    buildcmd: str | None,
    tracker: ingest_tracking.TimingRepo,
    mut_cmake_exe_targets: list[str] | None = None,
):
    """Leaves a copy of a provided-or-generated compile_commands.json file
    in the given build directory.

    The compile_commands.json file will refer to paths in the original codebase,
    not the build directory.

    The build directory may or may not end up with a copy of the input codebase,
    and in-place build artifacts,
    depending on whether a build was required to produce the compilation database.
    To account for builds that generate files in the source tree, such as `Vim`,
    any newly created files ending in `.h`, `.c`, or `.inc` will be copied back
    to the original codebase. Otherwise, builds would fail due to missing files.
    """
    provided_compdb = codebase / "compile_commands.json"
    provided_cmakelists = codebase / "CMakeLists.txt"

    if provided_cmakelists.exists() and not provided_compdb.exists():
        # If we have a CMakeLists.txt, we can generate the compile_commands.json
        cmake_project = CMakeProject(str(builddir), str(codebase), api_version=1)
        cmake_project.cmake_file_api.instrument_all()
        cc_launcher = str(
            repo_root.find_repo_root_dir_Path() / "cli" / "sh" / "cc-ld-intercept" / "cc"
        )
        ld_launcher = str(
            repo_root.find_repo_root_dir_Path() / "cli" / "sh" / "cc-ld-intercept" / "ld"
        )
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
        buildcmds = codebase / ".xj-build-commands"
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

        entries = []
        for json_file in buildcmds.glob("*.json"):
            with open(json_file, "r", encoding="utf-8") as f:
                entry = json.load(f)
                # if entry["type"] != "cc":
                #     continue  # FIXME
                entries.append(entry)
        parsed_entries = targets_from_cmake.convert_json_entries(entries)
        # print("parsed entries:")
        # pprint(parsed_entries)

        # entry_infos = targets_from_cmake.cmake_project_to_entry_infos(cmake_project)
        # print("entry infos:")
        # pprint(entry_infos)

        targets_from_cmake.collect_executable_target_names(cmake_project, mut_cmake_exe_targets)

        new_commands = targets_from_cmake.extract_link_compile_commands(
            parsed_entries, codebase, builddir
        )
        # print("link commands:")
        # pprint(new_commands)

        shutil.copyfile(
            builddir / "compile_commands.json", codebase / "compile_commands.cmake.json"
        )

        Path(codebase / "compile_commands.withlinks.json").write_text(
            json.dumps(new_commands, indent=2), encoding="utf-8"
        )
        shutil.copyfile(
            codebase / "compile_commands.withlinks.json", builddir / "compile_commands.json"
        )
        compilation_database.rebase_compile_commands_from_to(
            builddir / "compile_commands.json", builddir, codebase
        )

    elif codebase.is_file() and codebase.suffix == ".c":
        # If we have a single C file, we can trivially generate a compile_commands.json
        # with a single entry for it.
        compilation_database.write_synthetic_compile_commands_to(
            builddir / "compile_commands.json", codebase, codebase.parent
        )
    elif buildcmd:
        # If we have a build command, use it to generate a compile_commands.json file
        # by invoking the build command from a temporary directory with a copy of the
        # input codebase.
        shutil.copytree(codebase, builddir, dirs_exist_ok=True)
        hermetic.run(f"intercept-build {buildcmd}", cwd=builddir, shell=True, check=True)
        copy_new_source_files_back(codebase, builddir)
        # intercept-build will have generated this file, if all went well
        extracted_compdb = builddir / "compile_commands.json"
        extracted_compdb_bytes = extracted_compdb.read_bytes()
        if extracted_compdb_bytes == b"[]":
            # Perhaps the build command failed, or it cut off early,
            # for example if the target binary already existed.
            raise ValueError("Extracted compile_commands.json is empty")
        else:
            compilation_database.rebase_compile_commands_from_to(
                extracted_compdb, builddir, codebase
            )

    elif provided_compdb.exists():
        # Otherwise, we assume the compile_commands.json is already present.
        # We must make a copy to freely munge without affecting the original.
        shutil.copyfile(provided_compdb, builddir / "compile_commands.json")
    else:
        # If the codebase is a directory containing a single C file,
        # we assume it's the intended source file to compile.
        c_files = list(codebase.glob("*.c"))
        if len(c_files) == 1:
            compilation_database.write_synthetic_compile_commands_to(
                builddir / "compile_commands.json", c_files[0], codebase
            )
        else:
            raise FileNotFoundError(
                f"No compile_commands.json found in {codebase}, "
                "and unable to generate one automatically."
            )


def compdb_path_in(dir: Path) -> Path:
    return dir / "compile_commands.json"


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
    if compdb_path_in(dst).exists():
        compilation_database.rebase_compile_commands_from_to(compdb_path_in(dst), src, dst)

        # print("Rebased compile_commands.json")
        # print("from ", src)
        # print("to   ", dst)
        # print(compdb_path_in(dst).read_text())


type QUSS = str
type FileContentsStr = str
type RelativeFilePathStr = str


class FnDefHandling(Enum):
    EXCLUDE = 1
    INCLUDE_BODY = 2
    INCLUDE_DECL_ONLY = 3


def collect_decls_by_rel_tu(
    current_codebase: Path,
    restricted_to_files: set[FilePathStr] | None = None,
    fn_def_handling: FnDefHandling = FnDefHandling.EXCLUDE,
) -> dict[RelativeFilePathStr, dict[QUSS, tuple[RelativeFilePathStr, int, int, FileContentsStr]]]:
    def path_of_interest(p: FilePathStr) -> bool:
        if not Path(p).is_relative_to(current_codebase):
            assert not Path(p).is_relative_to(current_codebase.parent)
            return False
        return restricted_to_files is None or p in restricted_to_files

    compdb = compilation_database.CompileCommands.from_json_file(compdb_path_in(current_codebase))

    header_contents = CachingFileContents()

    decls_by_rel_tu: dict[
        RelativeFilePathStr, dict[QUSS, tuple[RelativeFilePathStr, int, int, FileContentsStr]]
    ] = {}

    # Note that the two FilePathStrs here can be different,
    # e.g. the declaration can be in a header file included by the TU file.

    index = c_refact.create_xj_clang_index()
    tus = c_refact.parse_project(index, compdb)
    for tu_path, tu in tus.items():
        assert Path(tu_path).is_relative_to(current_codebase)
        rel_tu_path = Path(tu_path).relative_to(current_codebase).as_posix()
        for cursor in tu.cursor.get_children():
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
                cursor_end = cursor.extent.end
                if cursor.kind == CursorKind.FUNCTION_DECL and cursor.is_definition():
                    if fn_def_handling == FnDefHandling.EXCLUDE:
                        continue
                    if fn_def_handling == FnDefHandling.INCLUDE_DECL_ONLY:
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
                                        cursor_end = prev_tok.extent.end
                                    else:
                                        cursor_end = t.extent.start
                                    break
                                prev_tok = t

                relative = Path(cursor.location.file.name).relative_to(current_codebase)
                decls_by_rel_tu.setdefault(rel_tu_path, {})[q] = (
                    relative.as_posix(),
                    cursor.extent.start.offset,
                    cursor_end.offset,
                    header_contents.get_bytes(cursor.location.file.name)[
                        cursor.extent.start.offset : cursor_end.offset
                    ].decode("utf-8"),
                )
    return decls_by_rel_tu


def organize_decls_by_headers(
    decls_by_rel_tu: dict[
        RelativeFilePathStr, dict[QUSS, tuple[FilePathStr, int, int, FileContentsStr]]
    ],
) -> dict[FilePathStr, dict[QUSS, tuple[int, int, FileContentsStr]]]:
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
    decls_by_header: dict[RelativeFilePathStr, dict[QUSS, tuple[int, int, FileContentsStr]]] = {}
    for q in every_quss:
        entries_dups = []
        for rel_tu_path in rel_tu_paths:
            tu_decls = decls_by_rel_tu[rel_tu_path]
            if q in tu_decls:
                entries_dups.append(tu_decls[q])
        expanded_entries_set = expand_overlapping_decl_header_entries(set(entries_dups))
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
        # All declarations have same contents
        for entry in expanded_entries_set:
            header_path_str = entry[0]
            header_path = Path(header_path_str)
            if header_path.is_absolute():
                raise ValueError(
                    f"Expected relative header path, got absolute path: {header_path_str}"
                )
            decls_by_header.setdefault(header_path_str, {})[q] = entry[1:]

    return decls_by_header


def expand_overlapping_decl_header_entries(
    entries_set: set[tuple[FilePathStr, int, int, FileContentsStr]],
) -> set[tuple[FilePathStr, int, int, FileContentsStr]]:
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

    by_header: dict[FilePathStr, list[tuple[FilePathStr, int, int, FileContentsStr]]] = {
        h: [] for h in set(e[0] for e in entries_set)
    }
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
            expanded_entries.add((header_path, min_start, max_end, full_text))

    return expanded_entries


@dataclass
class PrepPassResultStore:
    decls_defined_by_headers: dict[
        RelativeFilePathStr, dict[QUSS, tuple[int, int, FileContentsStr]]
    ]
    decls_defined_after_pp: dict[
        RelativeFilePathStr, dict[QUSS, tuple[FilePathStr, int, int, FileContentsStr]]
    ]
    cmake_exe_targets: list[str]

def run_preparation_passes(
    original_codebase: Path,
    resultsdir: Path,
    tracker: ingest_tracking.TimingRepo,
    guidance: dict,
    buildcmd: str | None = None,
) -> tuple[Path, list[str]]:
    """Returns the path to the final prepared codebase directory,
    along with a list of CMake executable targets (if applicable)."""

    def prep_00_copy_pristine_codebase(pristine: Path, newdir: Path, store: PrepPassResultStore):
        copy_codebase(pristine, newdir)

    def prep_01_materialize_compdb(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        with tempfile.TemporaryDirectory() as builddirname:
            builddir = Path(builddirname)
            materialize_compilation_database_in(
                builddir, current_codebase, buildcmd, tracker, store.cmake_exe_targets
            )
            # The generated compile_commands.json is in builddir and refers to current_codebase.

            compdb_contents = compdb_path_in(builddir).read_text()
            assert builddirname not in compdb_contents
            assert compdb_contents != "[]", "Generated compile_commands.json is empty"

            shutil.copyfile(compdb_path_in(builddir), compdb_path_in(current_codebase))

        tracker.set_preprocessor_definitions(
            compilation_database.extract_preprocessor_definitions_from_compile_commands(
                compdb_path_in(current_codebase),
                current_codebase,
            ),
        )

        # Writing out the user-provided guidance can go pretty much wherever in the early stages.
        json.dump(
            guidance,
            open(current_codebase / XJ_GUIDANCE_FILENAME, "w", encoding="utf-8"),
            indent=2,
        )

    def prep_uniquify_compdb(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        compdb = compilation_database.CompileCommands.from_json_file(
            compdb_path_in(current_codebase)
        )

        # Some codebases will compile the same source file multiple times with different
        # flags. It's really convenient for us if each source file is only compiled once.
        # So we duplicate source files as needed and adjust the compilation database.

        outputs = [cmd.output for cmd in compdb.commands if cmd.output is not None]
        assert len(outputs) == len(set(outputs)), f"Output files are not unique: {outputs}"

        new_commands: list[compilation_database.CompileCommand] = []
        for abs_path in compdb.get_source_files():
            cmds = compdb.get_commands_for_path(abs_path)
            if len(cmds) == 1:
                # Only one command for this file, keep as-is
                new_commands.append(cmds[0])
                continue

            # Multiple commands for same file - need to duplicate all of them
            for idx, cmd in enumerate(cmds):
                # Create unique file name: foo.c -> foo_xjdup_0.c, foo_xjdup_1.c, ...
                stem = abs_path.stem
                suffix = abs_path.suffix
                new_filename = f"{stem}_xjdup_{idx}{suffix}"
                new_file_path = abs_path.parent / new_filename

                # Copy the source file  to the new file
                shutil.copyfile(abs_path, new_file_path)

                # Update command arguments to reference new file
                new_args = cmd.get_command_parts().copy()
                for i, arg in enumerate(new_args):
                    arg_path = Path(arg)
                    if not arg_path.is_absolute():
                        arg_path = cmd.directory_path / arg_path
                    try:
                        if arg_path.resolve() == abs_path.resolve():
                            new_args[i] = str(new_file_path)
                            break
                    except OSError:
                        # Path resolution can fail for non-existent paths
                        pass

                # Create new compile command with updated file and arguments
                new_commands.append(
                    compilation_database.CompileCommand(
                        directory=cmd.directory,
                        file=str(new_file_path),
                        arguments=new_args if cmd.arguments else None,
                        command=" ".join(new_args) if cmd.command else None,
                        output=cmd.output,
                    )
                )

        # compdb.get_source_files() ignores fake link thingy commands,
        # so we need to add them back in.
        for cmd in compdb.commands:
            if cmd.is_fake_link_thingy:
                new_commands.append(cmd)

        # Write updated compile_commands.json
        compilation_database.CompileCommands(commands=new_commands).to_json_file(
            compdb_path_in(current_codebase)
        )

    def prep_localize_mutable_globals(
        prev: Path, current_codebase: Path, store: PrepPassResultStore
    ):
        compdb = compilation_database.CompileCommands.from_json_file(
            compdb_path_in(current_codebase)
        )
        c_refact.localize_mutable_globals(
            current_codebase / "xj-cclyzer.json", compdb, prev, current_codebase
        )

    def prep_lift_subfield_args(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        compdb = compilation_database.CompileCommands.from_json_file(
            compdb_path_in(current_codebase)
        )
        c_refact_arglifter.lift_subfield_args(compdb)

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
        # Compile and link LLVM bitcode module
        bitcode_module_path = current_codebase / "linked_module.bc"

        llvm_bitcode_linking.compile_and_link_bitcode(
            compdb_path_in(current_codebase), bitcode_module_path, use_llvm14=True
        )

        run_cc2json_or_cached(bitcode_module_path, current_codebase)

    def prep_uniquify_statics(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        compdb = compilation_database.CompileCommands.from_json_file(
            compdb_path_in(current_codebase)
        )
        all_pgs_cursors = c_refact.compute_globals_and_statics_for_project(
            compdb, statics_only=True
        )
        all_pgs = [c_refact.mk_NamedDeclInfo(c) for c in all_pgs_cursors]
        # We do not want to try renaming symbols from outside the current codebase!
        current_codebase_dir = current_codebase.as_posix()
        pgs = [v for v in all_pgs if (v.file_path or "").startswith(current_codebase_dir)]
        all_global_names = set(g_s.spelling for g_s in pgs)
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
            pgs, key=lambda g: (g.file_path or "", g.decl_start_byte_offset)
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
        j = c_refact.run_xj_locate_joined_decls(current_codebase)
        c_refact_decl_splitter.apply_decl_splitting_rewrites(current_codebase, j)

    def prep_pre_refold_consolidation(
        prev: Path, current_codebase: Path, store: PrepPassResultStore
    ):
        # store.decls_defined_by_headers has, for each header, a collection of
        # (quss-ident, (start_offset, end_offset, source_text)) entries.
        # For each translation unit, we'll find the corresonding range for the
        # quss-ident's declaration(s) and compare with the header's version.
        # If the header's version is different, we'll record that this TU
        # modified the declaration.
        # If every TU modifies the same header-defined declaration in the same way,
        # we will replace each TU's version with the header's version,
        # and replace the header's version with the modified version.
        # (The former allows refolding to work better, the latter preserves
        # the intended semantics of the program.)
        #
        # Since we run this pass before refolding, locations refer to .i files,
        # not .c or .h files.
        compdb = compilation_database.CompileCommands.from_json_file(
            compdb_path_in(current_codebase)
        )

        decls_src_by_tu: dict[FilePathStr, dict[QUSS, FileContentsStr]] = {}
        tus_modifying_decls: dict[QUSS, set[FilePathStr]] = {}

        index = c_refact.create_xj_clang_index()
        tus = c_refact.parse_project(index, compdb)

        # Build a map from QUSS to the expected (original) source text from headers
        quss_to_header_src: dict[QUSS, FileContentsStr] = {}
        for header_decls in store.decls_defined_by_headers.values():
            for q, (start_offset, end_offset, source_text) in header_decls.items():
                quss_to_header_src[q] = source_text

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

        remove_expanded_src_for: set[QUSS] = set()
        quss_to_expanded_src: dict[QUSS, FileContentsStr] = {}
        for expanded_decls in store.decls_defined_after_pp.values():
            for q, (rel_path, start_offset, end_offset, source_text) in expanded_decls.items():
                if q in quss_to_expanded_src and q not in remove_expanded_src_for:
                    # Already recorded from another TU
                    combined = combine_source_texts_for_header(quss_to_expanded_src[q], source_text)
                    if combined is None:
                        print(
                            "PPRC: WARNING: Expanded header source differs between TUs for QUSS:",
                            q,
                        )
                        print("  Previous source:", quss_to_expanded_src[q])
                        print("  New source:     ", source_text)
                        print("    from:", rel_path)
                        remove_expanded_src_for.add(q)
                    else:
                        quss_to_expanded_src[q] = combined
                    continue
                quss_to_expanded_src[q] = source_text

        for q in remove_expanded_src_for:
            del quss_to_expanded_src[q]

        defn_offsets_by_tu: dict[FilePathStr, dict[QUSS, tuple[int, int]]] = {}

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

            tu_file_contents_str = Path(tu_path).read_text(encoding="utf-8")
            for cursor in tu.cursor.get_children():
                if cursor.kind.is_declaration():
                    if cursor.kind == CursorKind.FUNCTION_DECL and cursor.is_definition():
                        # Skip function definitions
                        continue

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
                    if q in quss_to_header_src:
                        start_offset = cursor.extent.start.offset
                        end_offset = cursor.extent.end.offset
                        newest_tu_src = tu_file_contents_str[start_offset:end_offset]
                        decls_src_by_tu.setdefault(tu_path, {})[q] = newest_tu_src
                        defn_offsets_by_tu.setdefault(tu_path, {})[q] = (start_offset, end_offset)

                        expanded_tu_src = store.decls_defined_after_pp.get(rel_tu_path, {}).get(
                            q, (None, None, None)
                        )[2]
                        # Check if TU's version differs from header's expanded version
                        if not expanded_tu_src:
                            print(
                                f"PPRC: WARNING: No expanded header source recorded for QUSS: {q} in TU:",
                                (rel_tu_path in store.decls_defined_after_pp),
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

                            tus_modifying_decls.setdefault(q, set()).add(tu_path)

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
            for q, modifying_tus in tus_modifying_decls.items():
                # Check if all TUs modify it in the same way
                modified_versions = set()
                for tu_path in modifying_tus:
                    if q in decls_src_by_tu[tu_path]:
                        modified_versions.add(decls_src_by_tu[tu_path][q])

                if len(modified_versions) != 1:
                    print("PPRC: Declaration modified differently between TUs for QUSS:", q)
                    continue  # TUs modify it in different ways

                # All TUs modify this declaration identically
                # Apply the consolidation: replace TU versions with header version,
                # and update header with the modified version
                modified_version = modified_versions.pop()
                # original_header_version = quss_to_header_src[q]
                expanded_header_version = quss_to_expanded_src[q]

                # print(f"PPRC: Consolidating declaration {q}:")
                # print(f"  Original (header):\n{original_header_version}")
                # print("----------------------------")
                # print(f"  Modified (TUs):\n{modified_version}")
                # print("----------------------------")

                # Replace in each TU: modified_version -> expanded_header_version
                for tu_path in modifying_tus:
                    assert tu_path in defn_offsets_by_tu
                    if q in defn_offsets_by_tu[tu_path]:
                        tu_file_path = Path(tu_path)
                        # We cannot use simple string replacement here because we
                        # do not want to modify the signatures of function definitions,
                        # only the declarations.
                        start_offset, end_offset = defn_offsets_by_tu[tu_path][q]
                        length = end_offset - start_offset
                        rewriter.add_rewrite(
                            tu_file_path.as_posix(), start_offset, length, expanded_header_version
                        )

                # Update the header with the modified version
                for rel_header_path_str, header_decls in store.decls_defined_by_headers.items():
                    if q in header_decls:
                        start_offset, end_offset, _ = header_decls[q]
                        # TODO handle nested header locations
                        rel_header_path = Path(rel_header_path_str)
                        assert not rel_header_path.is_absolute(), (
                            f"Expected relative header path {rel_header_path_str}"
                        )
                        header_file_path = current_codebase / rel_header_path
                        assert header_file_path.exists()
                        length = end_offset - start_offset
                        rewriter.add_rewrite(
                            header_file_path.as_posix(), start_offset, length, modified_version
                        )

    def prep_expand_preprocessor(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        # We want to capture decl information after the header contents have stabilized,
        # so just before preprocessor expansion is a good time.
        local_header_paths = set(p.as_posix() for p in current_codebase.glob("**/*.h"))

        store.decls_defined_by_headers = organize_decls_by_headers(
            collect_decls_by_rel_tu(current_codebase, restricted_to_files=local_header_paths)
        )

        # Miscellaneous tasks over, onwards with preprocessor expansion!

        compdb = compilation_database.CompileCommands.from_json_file(
            compdb_path_in(current_codebase)
        )
        new_compdb: compilation_database.CompileCommands = (
            c_refact.preprocess_and_create_new_compdb(
                compdb,
                current_codebase.as_posix(),
            )
        )
        # new_compdb already written to current_codebase
        for cmd in new_compdb.commands:
            assert cmd.file_path.suffix == ".i", "Expected preprocessed .i files"

        store.decls_defined_after_pp = collect_decls_by_rel_tu(current_codebase)
        print(
            f"Collected declarations after preprocessing: {len(store.decls_defined_after_pp)} TUs"
        )

    def prep_refold_preprocessor(prev: Path, current_codebase: Path, store: PrepPassResultStore):
        compdb = compilation_database.CompileCommands.from_json_file(
            compdb_path_in(current_codebase)
        )
        new_compdb: compilation_database.CompileCommands = (
            c_refact.refold_preprocess_and_create_new_compdb(compdb, current_codebase.as_posix())
        )
        # new_compdb already written to current_codebase
        for cmd in new_compdb.commands:
            assert cmd.file_path.suffix == ".c", "Expected un-preprocessed .c files"

    preparation_passes: list[
        tuple[str, Callable[[Path, Path, PrepPassResultStore], CompletedProcess | None]]
    ] = [
        ("copy_pristine_codebase", prep_00_copy_pristine_codebase),
        ("materialize_compdb", prep_01_materialize_compdb),
        ("uniquify_compdb", prep_uniquify_compdb),
    ]

    if os.environ.get("XJ_EXTRA_PREPARATION_PASSES") == "1":
        preparation_passes.extend([
            ("uniquify_statics", prep_uniquify_statics),
            ("split_joined_decls", prep_split_joined_decls),
            ("expand_preprocessor", prep_expand_preprocessor),
            ("run_cclzyerpp_analysis", prep_run_cclzyerpp_analysis),
            ("localize_mutable_globals", prep_localize_mutable_globals),
            ("lift_subfield_args", prep_lift_subfield_args),
            # # Refolding and pre-refolding should always go together.
            # # They are separate steps to allow inspection of the intermediate codebase.
            # ("pre_refold_consolidation", prep_pre_refold_consolidation),
            # ("refold_preprocessor", prep_refold_preprocessor),
        ])

    prev = original_codebase.absolute()
    resultsdir_abs = resultsdir.absolute()

    store = PrepPassResultStore(
        decls_defined_by_headers={}, decls_defined_after_pp={}, cmake_exe_targets=[]
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

    return prev, store.cmake_exe_targets
