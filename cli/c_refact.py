import json
import time
from dataclasses import dataclass
import platform
from pathlib import Path
import subprocess
import shutil
from typing import TypedDict
import pprint

from clang.cindex import (  # type: ignore
    Index,
    CursorKind,
    Config,
    Diagnostic,
    LinkageKind,
    StorageClass,
    TranslationUnit,
    CompilationDatabase,
    TypeKind,
    Cursor,
)

import hermetic
import repo_root
import compilation_database
import batching_rewriter
from cindex_helpers import render_declaration_sans_qualifiers, yield_matching_cursors
import c_refact_type_mod_replicator
from constants import XJ_GUIDANCE_FILENAME
import targets


def create_xj_clang_index() -> Index:
    """Create a clang Index configured to use the hermetic xj-llvm installation."""

    if not Config.loaded:
        xj_llvm = hermetic.xj_llvm_root(repo_root.localdir())

        if platform.system() == "Darwin":
            libclang_path = xj_llvm / "lib" / "libclang.dylib"
        else:
            # Keep this version in sync with the one embedded in pyproject.toml
            libclang_path = xj_llvm / "lib" / "libclang.so.18.1.8"

        Config.set_library_file(libclang_path)

    return Index.create()


def xj_comp_db_from_directory(dir: str) -> compilation_database.CompileCommands:
    return compilation_database.CompileCommands.from_json_file(f"{dir}/compile_commands.json")


def clang_comp_db_from_directory(dir: str) -> CompilationDatabase:
    return CompilationDatabase.fromDirectory(dir)


def parse_translation_unit_with_args(
    index: Index, path: str, args: list[str], in_dir: str | None = None
) -> TranslationUnit:
    """Parse a translation unit with given arguments."""
    args_matching_path = []
    for arg in args:
        if path == arg:
            args_matching_path.append(arg)
        elif in_dir and path == str(Path(in_dir) / arg):
            args_matching_path.append(arg)
        elif in_dir and arg == str(Path(path).relative_to(Path(in_dir), walk_up=True)):
            args_matching_path.append(arg)

    if len(args_matching_path) == 0:
        raise ValueError(
            f"Could not find source file path '{path}' in args: {args}\nwith {in_dir=}"
        )

    if len(args_matching_path) > 1:
        raise ValueError(f"Multiple matching source file paths for '{path}' in args: {args}")

    args_sans_path = [arg for arg in args if arg != args_matching_path[0]]

    # We cannot use the args list as-is with `index.parse` method for two reasons:
    #
    # 1. It takes the source file as a separate argument, not in the args list,
    #    and errors if it's present in both places. We can remove it if it's
    #    unambiguously present in the args, but in general we can't distinguish
    #    occurrences as being direct parameters to clang vs flag arguments.
    # 2. When invoking clang directly, it automatically picks up the sysroot
    #    from its configuration file, and automatically uses its own include paths.
    #    When using libclang, we have to specify that information explicitly.
    #
    # An alternative approach, possibly more robust, would be to invoke `clang`
    # to emit a .ast file, which ensures that the AST we end up with is exactly
    # what `clang` would be working with.

    xj_llvm = hermetic.xj_llvm_root(repo_root.localdir())
    sysroot_dir = xj_llvm / "sysroot"

    return index.parse(
        path=path,
        args=[
            f"--sysroot={sysroot_dir.as_posix()}",
            "-isystem",
            (xj_llvm / "lib" / "clang" / "18" / "include").as_posix(),
            *args_sans_path,
        ],
        options=TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
    )


def parse_project(
    index: Index,
    compdb: compilation_database.CompileCommands,
) -> dict[str, TranslationUnit]:
    """Parse all translation units in the compilation database.

    Returns a mapping from absolute source file path to TranslationUnit."""
    tus = {}
    for srcfile in compdb.get_source_files():
        cmds = compdb.get_commands_for_path(srcfile)
        if not srcfile.is_absolute():
            srcfile = (cmds[0].directory_path / srcfile).resolve()

        # Some build systems in practice do end up compiling the same
        # source file with different command lines, putting results
        # in different output files. We don't really know here if it
        # is significant (i.e. preprocessor defines differ so the ASTs
        # might differ) or if the build is just doing some redundant
        # work, in which case we can safely ignore the extra commands.
        if srcfile.name.startswith("antlr3"):
            pass
        else:
            assert len(cmds) == 1, (
                f"Expected exactly one compile command for {srcfile}, got {pprint.pformat(cmds)}"
            )
        parts = cmds[0].get_command_parts()[1:]  # Skip compiler executable
        tu = parse_translation_unit_with_args(
            index,
            srcfile.as_posix(),
            parts,
            in_dir=cmds[0].directory_path.as_posix(),
        )
        if srcfile.is_absolute():
            abs_path = srcfile.resolve()
        else:
            abs_path = (cmds[0].directory_path / srcfile).resolve()
        tus[abs_path.as_posix()] = tu
    return tus


def preprocess_build(b: targets.BuildInfo, t: targets.BuildTarget, target_dir: Path) -> None:
    """
    For each TU, run clang -E to preprocess it into target_dir.
    """
    target_dir.mkdir(parents=True, exist_ok=True)

    clang_path = hermetic.xj_llvm_root(repo_root.localdir()) / "bin" / "clang"

    compdb = b.compdb_for_target_within(t.name, target_dir)
    for cmd in compdb.commands:
        # 1. Determine paths
        abs_src_path = cmd.absolute_file_path
        try:
            rel_src_path = abs_src_path.relative_to(target_dir)
        except ValueError:
            rel_src_path = Path(abs_src_path.name)

        preprocessed_file_path = (target_dir / rel_src_path).with_suffix(".nolines.i")
        preprocessed_file_path.parent.mkdir(parents=True, exist_ok=True)

        # 2. Run preprocessor
        original_args = cmd.get_command_parts()
        compiler_args = original_args[1:]

        # Remove output file from args
        try:
            o_index = compiler_args.index("-o")
            del compiler_args[o_index : o_index + 2]
        except ValueError:
            pass

        if "-c" in compiler_args:
            compiler_args.remove("-c")

        # Remove source file from args
        temp_args = []
        for arg in compiler_args:
            arg_path = Path(arg)
            if not arg_path.is_absolute():
                arg_path = cmd.directory_path / arg_path

            if arg_path.resolve() != abs_src_path.resolve():
                temp_args.append(arg)
        compiler_args = temp_args

        base_pp_command = [
            str(clang_path),
            "-E",
            str(abs_src_path),
            *compiler_args,
        ]

        refold_map_path = preprocessed_file_path.with_suffix(".refoldmap.json")
        cp = subprocess.run(
            [
                *base_pp_command,
                f"--refold-map={refold_map_path.as_posix()}",
                "--no-line-commands",
                "-o",
                str(preprocessed_file_path),
            ],
            check=False,
            cwd=cmd.directory,
            capture_output=True,
        )
        preprocessed_file_path.with_suffix(".refold-stdout.txt").write_bytes(cp.stdout)
        preprocessed_file_path.with_suffix(".refold-stderr.txt").write_bytes(cp.stderr)
        cp.check_returncode()

        shutil.copyfile(preprocessed_file_path, preprocessed_file_path.with_suffix(".unmodified.i"))
    b._use_preprocessed_files = True


def refold_build(b: targets.BuildInfo, t: targets.BuildTarget, target_dir_path: Path) -> None:
    """
    For each TU in compdb, run clang-refold to produce .c files from modified .i files
    """
    target_dir_path.mkdir(parents=True, exist_ok=True)
    compdb = b.compdb_for_target_within(t.name, target_dir_path)
    for cmd in compdb.commands:
        abs_src_path = cmd.absolute_file_path

        assert abs_src_path.suffixes[-2:] == [".nolines", ".i"]
        abs_src_path_base = abs_src_path.with_suffix("")
        c_path = abs_src_path_base.with_suffix(".c")
        refold_map_path = abs_src_path_base.with_suffix(".nolines.refoldmap.json")

        print("Refolding", abs_src_path, "to", c_path)
        print(cmd.get_command_parts())
        hermetic.run(
            [
                "clang-refold",
                "-P",  # modified preprocessed file
                abs_src_path,
                "-p",  # unmodified preprocessed file
                abs_src_path.with_suffix(".unmodified.i"),
                "-r",  # refold map
                refold_map_path.as_posix(),
                "-o",
                str(c_path),
            ],
            check=True,
            cwd=cmd.directory,
        )
    b._use_preprocessed_files = False


@dataclass
class NamedDeclInfo:
    spelling: str
    file_path: str | None
    decl_start_byte_offset: int
    decl_end_byte_offset: int
    start_line: int
    start_col: int
    end_line: int
    end_col: int


@dataclass
class TissueFunctionCursorInfo:
    cursor: Cursor
    file: str
    is_definition: bool


@dataclass
class TissueCallSiteInfo:
    caller_func: str
    callee_funcs: list[str]  # currently unused, kept for possible future use/debugging
    i_file_path: str
    line: int
    col: int


def compute_globals_and_statics_for_project(
    compdb: compilation_database.CompileCommands,
    elide_functions: bool = False,
    statics_only: bool = False,
) -> list[Cursor]:
    index = create_xj_clang_index()
    tus = parse_project(index, compdb)
    return compute_globals_and_statics_for_translation_units(
        list(tus.values()), elide_functions, statics_only
    )


def compute_globals_and_statics_for_translation_units(
    translation_units: list[TranslationUnit],
    elide_functions: bool = False,
    statics_only: bool = False,
) -> list[Cursor]:
    combined: list[Cursor] = []
    for tu in translation_units:
        results = compute_globals_and_statics_for_translation_unit(
            tu, elide_functions, statics_only
        )
        combined.extend(results)
    return combined


def mk_NamedDeclInfo(node: Cursor) -> NamedDeclInfo:
    extent = node.extent
    start = extent.start
    end = extent.end

    file_path = start.file.name if start.file else None
    return NamedDeclInfo(
        spelling=node.spelling,
        file_path=file_path,
        decl_start_byte_offset=start.offset,
        decl_end_byte_offset=end.offset,
        start_line=start.line,
        start_col=start.column,
        end_line=end.line,
        end_col=end.column,
    )


# A plain C identifier for a global, or "fnname.ident" for a function-scoped
# static, or something prefixed with ".str." for a string literal
type GlobalNameSpec = str


def demangle_meg(mangled_name: str) -> str:
    if "." not in mangled_name:
        return mangled_name
    return mangled_name.split(".")[1]


def compute_globals_and_statics_for_translation_unit(
    translation_unit: TranslationUnit, elide_functions: bool, statics_only: bool = False
) -> list[Cursor]:
    """Compute globals and static symbols defined in the translation unit."""

    results = []

    def grab(node):
        if elide_functions and node.kind == CursorKind.FUNCTION_DECL:
            return
        results.append(node)

    def visit(node: Cursor):
        if node.kind in (CursorKind.VAR_DECL, CursorKind.FUNCTION_DECL):
            sc = node.storage_class
            # Clang's implementation of `is_definition()` excludes tentative
            # definitions, but we want to include them here.
            is_def_ish = sc != StorageClass.EXTERN and node.linkage != 0
            if is_def_ish:
                top_level_var_decl = node.kind == CursorKind.VAR_DECL and (
                    node.semantic_parent.kind == CursorKind.TRANSLATION_UNIT
                )
                # Include top-level declarations or entities with 'static' storage
                if (top_level_var_decl and not statics_only) or sc == StorageClass.STATIC:
                    grab(node)
        for child in node.get_children():
            visit(child)

    visit(translation_unit.cursor)
    return results


def loc_key(c: Cursor) -> tuple[int, int, str]:
    file_path = c.location.file.name if c.location.file else "<unknown>"
    return (c.location.line, c.location.column, file_path)


def collect_cursors_by_loc(
    tus: dict[str, TranslationUnit],
    cursor_kind_filter: list[CursorKind] = [],
) -> dict[tuple[int, int, str], list[Cursor]]:
    """Group cursors by the (line, col, file) they are located in."""
    by_loc: dict[tuple[int, int, str], list[Cursor]] = {}
    for tu in tus.values():
        for c in tu.cursor.walk_preorder():
            # When looking for CALL_EXPR nodes in github.com/Old-Man-Programmer/tree
            # the filter reduces time taken from 1.2s to 0.3s
            if cursor_kind_filter and c.kind not in cursor_kind_filter:
                continue
            key = loc_key(c)
            if key not in by_loc:
                by_loc[key] = []
            by_loc[key].append(c)
    return by_loc


XJG_PLACEHOLDER = "((struct XjGlobals*)0)"


@dataclass
class LocalizeMutableGlobalsPhase1Results:
    ineligible_for_lifting: set[str]
    nonmain_tissue_functions: set[str]
    all_function_names: set[str]
    mutd_global_names: set[str]
    escd_global_names: set[str]
    globals_without_initializers: set[str]
    higher_order_potentially_modified_fn_ptr_type_locs: dict[str, list[tuple[int, int]]]
    applied_rewrites: dict[str, list[tuple[int, int, str]]]


def localize_mutable_globals_phase1(
    compdb: compilation_database.CompileCommands,
    j: dict,
    current_codebase: Path,
    prev: Path,
    nonmain_tissue_functions: set[str],
) -> LocalizeMutableGlobalsPhase1Results:
    """
    The first phase modifies function (pointer) types and
    inserts placeholder parameters for XjGlobals.

    Some function pointer types, when used in higher-order ways,
    cannot yet be reliably modified during phase 1, and currently
    must be handled by a separate cleanup pass
    (`speculatively_fix_higher_order_fn_ptr_types`).

    Phase 1 does not:
        * Replace occurrences of mutable global variables
            to use the corresponding lifted struct field.
        * Define the XjGlobals struct in the main TU.
        * Add necessary typedefs for XjGlobals fields.
    """

    phase1index = create_xj_clang_index()
    tus = parse_project(phase1index, compdb)

    all_function_names, nonmain_tissue_function_cursors = extract_function_info(
        tus, nonmain_tissue_functions
    )

    fpd_output = run_xj_prepare_findfnptrdecls(
        current_codebase, nonmain_tissue_functions, all_function_names
    )

    ineligible_for_lifting = set()
    for refs in j.get("global_initializer_references", {}).values():
        for r in refs:
            if r.startswith(".str"):
                continue
            ineligible_for_lifting.add(demangle_meg(r))

    print("ineligible_lifting:", list(ineligible_for_lifting))

    # Entries in this list are either plain global names, or for function-scoped statics,
    # they are in the format "function_name.var_name_xjtr_N"
    mangled_mutated_globals = j.get("mutated_globals", [])
    mangled_escaped_globals = j.get("escaped_globals", [])

    mutd_global_names_list = [demangle_meg(name) for name in mangled_mutated_globals]
    mutd_global_names = set(mutd_global_names_list)
    assert len(mutd_global_names) == len(mutd_global_names_list), (
        "Expected all mutated global names to be unique after demangling, "
        + f"but got duplicates within: {mutd_global_names_list}"
    )
    print("mutated_globals:", list(mutd_global_names))

    escd_global_names_list = [demangle_meg(name) for name in mangled_escaped_globals]
    escd_global_names = set(escd_global_names_list)
    assert len(escd_global_names) == len(escd_global_names_list), (
        "Expected all escaped global names to be unique after demangling, "
        + f"but got duplicates within: {escd_global_names_list}"
    )
    print("escaped_globals:", list(escd_global_names))

    # globals_and_statics = compute_globals_and_statics_for_translation_units(
    #     list(tus.values()), elide_functions=True
    # )
    # liftable_mutated_globals_and_statics = [
    #     c
    #     for c in globals_and_statics
    #     if c.spelling in mutd_global_names and c.spelling not in ineligible_for_lifting
    # ]

    call_sites_from_json = get_call_sites_from_json(
        prev, current_codebase, j, nonmain_tissue_functions, escd_global_names
    )

    results = LocalizeMutableGlobalsPhase1Results(
        all_function_names=all_function_names,
        nonmain_tissue_functions=nonmain_tissue_functions,
        mutd_global_names=mutd_global_names,
        escd_global_names=escd_global_names,
        ineligible_for_lifting=ineligible_for_lifting,
        higher_order_potentially_modified_fn_ptr_type_locs=fpd_output[
            "higher_order_potentially_modified_fn_ptr_type_locs"
        ],
        globals_without_initializers=set(fpd_output["globals_without_initializers"]),
        applied_rewrites={},
    )

    cbl_start = time.time()
    call_expr_cursors_by_loc = collect_cursors_by_loc(tus, [CursorKind.CALL_EXPR])
    cbl_elapsed = time.time() - cbl_start
    print(f"  collect_cursors_by_loc took {cbl_elapsed:.3f} seconds")

    with batching_rewriter.BatchingRewriter() as rewriter:
        # In each translation unit,
        #   for each identified function pointer type,
        #       modify it to add 'struct XjGlobals *' as first parameter.
        # This may require removing 'void' if it's the only parameter.
        # In non-empty parameter lists, we must add a trailing comma.
        print("phase1, adding rewrites for modified_fn_ptr_type_locs")
        for filepath_str, ranges in fpd_output["modified_fn_ptr_type_locs"].items():
            content = rewriter.get_content(filepath_str)

            for start_offset, end_offset in ranges:
                original_text = content[start_offset:end_offset].decode("utf-8")
                between_parens = original_text[1:].strip()
                add_trailing_comma = False
                if between_parens == "void" or not between_parens:
                    target_offset = end_offset  # replace void (and whitespace)
                else:
                    add_trailing_comma = True
                    target_offset = start_offset + 1

                after_opening_paren = start_offset + 1
                rewriter.add_rewrite(
                    filepath_str,
                    after_opening_paren,
                    target_offset - after_opening_paren,
                    f"struct XjGlobals *{', ' if add_trailing_comma else ''}",
                )

        print("phase1, adding rewrites for unmod_fn_occ_wrappers")
        for filepath_str, wrappers in fpd_output["unmod_fn_occ_wrappers"].items():
            for combined_wrapper in wrappers:
                # Add the wrapper function definition to the file
                rewriter.add_rewrite(
                    filepath_str,
                    combined_wrapper["decl_post_offset"],
                    0,
                    "\n" + combined_wrapper["wrapper_defn"] + "\n",
                )

                # For each occurrence of an unmodified function name,
                # in a position where we need to pass a wrapper instead,
                # modify the occurrence to refer to the wrapper.
                for occ in combined_wrapper["occ_offsets"]:
                    name_end = occ + len(combined_wrapper["name"])
                    rewriter.add_rewrite(filepath_str, name_end, 0, combined_wrapper["suffix"])

        # For each non-main tissue function, add 'struct XjGlobals *xjg' as first parameter
        print("phase1, adding rewrites for  nonmain_tissue_function_cursors")
        for func_cursors in nonmain_tissue_function_cursors.values():
            for func_info in func_cursors:
                # Find the position after the opening parenthesis
                content = rewriter.get_content(func_info.file)

                # Find the opening parenthesis
                range_start = func_info.cursor.extent.start.offset
                paren_pos = content.find(b"(", range_start)
                if paren_pos == -1:
                    continue

                # Check if there are existing parameters
                closing_paren_pos = content.find(b")", paren_pos)
                if closing_paren_pos == -1:
                    continue

                # Check if there are parameters already
                param_section = content[paren_pos + 1 : closing_paren_pos].strip()

                overwrite_len = 0
                if param_section == b"" or param_section == b"void":
                    # No parameters, just add our parameter
                    insert_offset = paren_pos + 1
                    insert_text = "struct XjGlobals *xjg"
                    overwrite_len = len(b"void") if param_section == b"void" else 0
                else:
                    # Has parameters, add as first parameter with comma
                    insert_offset = paren_pos + 1
                    insert_text = "struct XjGlobals *xjg, "

                rewriter.add_rewrite(func_info.file, insert_offset, overwrite_len, insert_text)

        # Step 6: Modify call sites to pass placeholder-for-xjg (using JSON call site info)
        print("phase1, adding rewrites for placeholder-for-xjg")
        for call_info in call_sites_from_json:
            caller_func = call_info.caller_func
            i_file_path = call_info.i_file_path
            line = call_info.line
            col = call_info.col

            # Working with .i files (in particular, ones without line markers)
            # allows us to reliably edit call sites. Otherwise, we'd have to contend
            # with call sites that are synthesized by the preprocessor in horrific ways.
            assert i_file_path.endswith(".nolines.i"), (
                f"Expected .nolines.i file, got {i_file_path}\n{compdb=}"
            )
            assert i_file_path in tus

            param_to_pass = XJG_PLACEHOLDER

            # Find the call expression at the given location
            # We need to use libclang to find the exact offset
            found_call = False
            for cursor in call_expr_cursors_by_loc.get((line, col, i_file_path), []):
                # Read file to find parenthesis
                content = rewriter.get_content(i_file_path)

                callee_expr = next(cursor.get_children(), None)
                if callee_expr:
                    # Ensure we skip past the callee when we look for
                    # the opening parenthesis.
                    call_start_offset = callee_expr.extent.end.offset
                else:
                    call_start_offset = cursor.extent.start.offset

                paren_pos = content.find(b"(", call_start_offset)
                if paren_pos == -1:
                    break

                # Check if there are existing arguments
                closing_paren_pos = content.find(b")", paren_pos)
                if closing_paren_pos == -1:
                    break

                args_section = content[paren_pos + 1 : closing_paren_pos].strip()

                if args_section == b"":
                    # No arguments
                    insert_offset = paren_pos + 1
                    insert_text = param_to_pass
                else:
                    # Has arguments, add as first argument with comma
                    insert_offset = paren_pos + 1
                    insert_text = param_to_pass + ", "

                rewriter.add_rewrite(i_file_path, insert_offset, 0, insert_text)
                found_call = True
                break

            if not found_call:
                raise ValueError(
                    f"  WARNING: Could not find call from {caller_func} at {i_file_path}:{line}:{col}"
                )

        print("phase1, finding main()")
        if True:
            # Find the file containing main() and its main function cursor
            main_file = None
            for abs_path, tu in tus.items():
                for cursor in tu.cursor.walk_preorder():
                    if cursor.kind == CursorKind.FUNCTION_DECL and cursor.spelling == "main":
                        main_file = abs_path
                        break
                if main_file:
                    break

        if True:
            # Add forward declaration to every translation unit, just in case
            for file_path_str in tus.keys():
                fwd_decl_text = "struct XjGlobals;\n"
                rewriter.add_rewrite(file_path_str, 0, 0, fwd_decl_text)

        # Replicate edits to type definitions across translation units
        equiv_classes = c_refact_type_mod_replicator.collect_type_definitions(
            list(tus.values()), fpd_output["var_decl_fn_ptr_arg_lparen_locs"]
        )
        pprint.pprint(
            equiv_classes,
            indent=2,
            stream=open(current_codebase / "xj-type_equiv_classes.txt", "w", encoding="utf-8"),
        )
        print("phase1, replicating type modifications across TUs")
        ext_rewrites = c_refact_type_mod_replicator.replicate_type_modifications(
            rewriter.get_rewrites(), equiv_classes
        )
        rewriter.replace_rewrites(ext_rewrites)

        print("phase1, appplying rewrites")
        results.applied_rewrites = rewriter.get_rewrites(reverse=False)
    return results


class SingleUnmodFnOccWrapper(TypedDict):
    name: str
    suffix: str
    occ_offset: int
    decl_post_offset: int
    wrapper_defn: str


class CombinedUnmodFnOccWrapper(TypedDict):
    name: str
    suffix: str
    occ_offsets: list[int]
    decl_post_offset: int
    wrapper_defn: str


def combine_unmod_fn_occ_wrappers(
    raws: list[SingleUnmodFnOccWrapper],
) -> list[CombinedUnmodFnOccWrapper]:
    combined_by_decl: dict[int, CombinedUnmodFnOccWrapper] = {}

    for raw in raws:
        decl_post_offset = raw["decl_post_offset"]
        if decl_post_offset not in combined_by_decl:
            combined_by_decl[decl_post_offset] = {
                "name": raw["name"],
                "suffix": raw["suffix"],
                "occ_offsets": [raw["occ_offset"]],
                "decl_post_offset": decl_post_offset,
                "wrapper_defn": raw["wrapper_defn"],
            }
        else:
            combined_by_decl[decl_post_offset]["occ_offsets"].append(raw["occ_offset"])

    return list(combined_by_decl.values())


type ModifiedFnPtrTypeLoc = tuple[int, int]  # start offsets for fn ty param parens


class BaseXjFindPtrDeclsOutput(TypedDict):
    modified_fn_ptr_type_locs: dict[str, list[ModifiedFnPtrTypeLoc]]
    higher_order_potentially_modified_fn_ptr_type_locs: dict[str, list[ModifiedFnPtrTypeLoc]]
    var_decl_fn_ptr_arg_lparen_locs: dict[str, dict[str, int]]
    globals_without_initializers: list[str]


class RawXjFindPtrDeclsOutput(BaseXjFindPtrDeclsOutput):
    unmod_fn_occ_wrappers: dict[str, list[SingleUnmodFnOccWrapper]]


class XjFindPtrDeclsOutput(BaseXjFindPtrDeclsOutput):
    unmod_fn_occ_wrappers: dict[str, list[CombinedUnmodFnOccWrapper]]


def run_xj_prepare_findfnptrdecls(
    current_codebase: Path,
    nonmain_tissue_functions: set[str],
    all_function_names: set[str],
) -> XjFindPtrDeclsOutput:
    builddir = hermetic.xj_prepare_findfnptrdecls_build_dir(repo_root.localdir())
    assert builddir.exists(), (
        f"Build directory {builddir} does not exist, should have been built already"
    )

    mod_fn_names_path = current_codebase / "nonmain_tissue_functions.txt"
    with open(mod_fn_names_path, "w", encoding="utf-8") as f:
        for fn in sorted(nonmain_tissue_functions):
            f.write(fn + "\n")

    unmod_fn_names_path = current_codebase / "unmod_fn_names.txt"
    with open(unmod_fn_names_path, "w", encoding="utf-8") as f:
        for fn in sorted(all_function_names - nonmain_tissue_functions):
            f.write(fn + "\n")
    # Keep in sync with `xj-prepare-findfnptrdecls/CMakeLists.txt`
    binary_path = builddir / "xj-find-fn-ptr-decls"
    xj_find_start = time.time()
    cp = hermetic.run(
        [
            binary_path.as_posix(),
            "--extra-arg=-Wno-zero-length-array",
            "--extra-arg=-Wno-implicit-int-conversion",
            "--extra-arg=-Wno-unused-function",
            "--executor=all-TUs",
            "--execute-concurrency=1",  # avoid race conditions, etc.
            "--modified_fns_file",
            mod_fn_names_path.as_posix(),
            "--unmodified_fns_file",
            unmod_fn_names_path.as_posix(),
            (current_codebase / "compile_commands.json").as_posix(),
        ],
        cwd=current_codebase,
        check=True,
        capture_output=True,
    )
    xj_find_elapsed = time.time() - xj_find_start
    print(f"xj-find-fn-ptr-decls completed in {xj_find_elapsed:.1f} seconds")

    print("xj-find-fn-ptr-decls stderr:")
    print("==========================")
    print(cp.stderr.decode("utf-8"))
    print("==========================")

    print("xj-find-fn-ptr-decls stdout:")
    print("==========================")
    print(cp.stdout.decode("utf-8"))
    print("==========================")
    try:

        def process(k: str, v):
            if k == "unmod_fn_occ_wrappers":
                return {f: combine_unmod_fn_occ_wrappers(occs) for f, occs in v.items()}
            else:
                return v

        raw: RawXjFindPtrDeclsOutput = json.loads(cp.stdout.decode("utf-8"))
        processed: XjFindPtrDeclsOutput = {k: process(k, v) for (k, v) in raw.items()}  # type: ignore
    except:
        print("Failed to parse xj-find-fn-ptr-decls output as JSON:")
        print(cp.stdout.decode("utf-8"))
        raise

    return processed


class XjLocateJoinedDeclsLoc(TypedDict):
    f: str
    b: int
    e: int


class XjLocateJoinedDeclsEdit(TypedDict):
    r: XjLocateJoinedDeclsLoc | None
    cat: str
    prefix: str
    declarators: list[str]


class XjLocateJoinedDeclsOutput(TypedDict):
    edits: list[XjLocateJoinedDeclsEdit]


def run_xj_locate_joined_decls(
    current_codebase: Path,
    build_info: targets.BuildInfo,
) -> XjLocateJoinedDeclsOutput:
    builddir = hermetic.xj_prepare_locatejoineddecls_build_dir(repo_root.localdir())
    assert builddir.exists(), (
        f"Build directory {builddir} does not exist, should have been built already"
    )

    # By default, clang tools use the location of the binary as the seed for
    # the resource directory. We need to override that to point to the
    # hermetic clang resource dir. If we don't, we'll encounter errors due to
    # missing `stddef.h` and other headers.
    # findptrdecls does not need to do this because it runs after expansion.
    xj_clang_resource_dir = (
        hermetic.run("clang -print-resource-dir", shell=True, capture_output=True)
        .stdout.decode("utf-8")
        .strip()
    )

    # Synthesize a compile_commands.json for all TUs in the codebase
    build_info.compdb_for_all_targets_within(current_codebase).to_json_file(
        current_codebase / "compile_commands.json"
    )

    # Keep in sync with `xj-prepare-printdecllocs/CMakeLists.txt`
    binary_path = builddir / "xj-print-decl-locs"
    xj_find_start = time.time()
    cp = hermetic.run(
        [
            binary_path.as_posix(),
            "--extra-arg=-Wno-zero-length-array",
            "--extra-arg=-Wno-implicit-int-conversion",
            "--extra-arg=-Wno-unused-function",
            "--executor=all-TUs",
            "--execute-concurrency=1",  # avoid race conditions, etc.
            "--json-output-path=xj-joined-decls.json",
            f"--extra-arg=-resource-dir={xj_clang_resource_dir}",
            (current_codebase / "compile_commands.json").as_posix(),
        ],
        cwd=current_codebase,
        check=True,
        capture_output=True,
    )
    xj_find_elapsed = time.time() - xj_find_start
    print(f"xj-locate-joined-decls completed in {xj_find_elapsed:.1f} seconds")

    print("xj-locate-joined-decls stderr:")
    print("==========================")
    print(cp.stderr.decode("utf-8"))
    print("==========================")

    print("xj-locate-joined-decls stdout:")
    print("==========================")
    print(cp.stdout.decode("utf-8"))
    print("==========================")
    try:
        raw: XjLocateJoinedDeclsOutput = json.load(
            (current_codebase / "xj-joined-decls.json").open()
        )
    except:
        print("Failed to parse xj-locate-joined-decls output as JSON:")
        print(cp.stdout.decode("utf-8"))
        raise

    return raw


def translate_offset_thru_rewrites(
    original_offset: int, rewrites: list[tuple[int, int, str]]
) -> int:
    new_offset = original_offset
    for rw_start, rw_len, rw_text in rewrites:
        if rw_start >= original_offset:
            break
        if rw_start + rw_len <= original_offset:
            new_offset += len(rw_text) - rw_len
        elif rw_start <= original_offset:
            # Overlap case
            raise ValueError("Cannot translate offset that overlaps with a rewrite")
    return new_offset


def speculatively_fix_higher_order_fn_ptr_types(
    compdb: compilation_database.CompileCommands,
    phase1results: LocalizeMutableGlobalsPhase1Results,
):
    """
    At the end of phase 1, we have inserted placeholders at call sites and
    have modified some but possibly not all function pointer types. Rather
    than implement ad-hoc type inference to identify the function pointer
    types needing modification, we'll use Clang as an oracle. In particular,
    we'll (1) collect diagnostics; (2) assuming we see some "too many arguments"
    errors, from having inserted placeholders without updating the corresponding
    function pointer type, we'll add additional parameters to some pre-identified
    function pointer types that might need them; (3) check again.
    If step 3 still has errors we'll roll back to step 1 and print a warning.
    If step 3 has no errors, we proceed directly to phase 2.
    """
    with batching_rewriter.BatchingRewriter() as rewriter:
        index = create_xj_clang_index()

        def count_possibly_fixable_errors(index: Index) -> tuple[int, int]:
            tus = parse_project(index, compdb)
            tu_possibly_fixable_errors = 0
            total_errors = 0
            for tu in tus.values():
                for x in tu.diagnostics:
                    if x.severity >= Diagnostic.Error:
                        total_errors += 1
                        print(f"Diagnostic {x.location}: {x.spelling} [{x.severity}]")

                    if x.spelling.startswith("too many arguments to function call"):
                        tu_possibly_fixable_errors += 1
                    elif x.option == "-Wincompatible-function-pointer-types":
                        tu_possibly_fixable_errors += 1

            return tu_possibly_fixable_errors, total_errors

        tu_possibly_fixable_errors, total_errors = count_possibly_fixable_errors(index)
        if total_errors > tu_possibly_fixable_errors:
            # raise ValueError(
            print(
                "Detected errors that are not possibly fixable; "
                + "aborting localization of mutable globals."
            )
        if tu_possibly_fixable_errors > 0:
            print(f"Detected {tu_possibly_fixable_errors} possibly fixable errors;")
            for (
                filepath_str,
                ranges,
            ) in phase1results.higher_order_potentially_modified_fn_ptr_type_locs.items():
                content = rewriter.get_content(filepath_str)

                for old_start_offset, old_end_offset in ranges:
                    rewrites = phase1results.applied_rewrites.get(filepath_str, [])
                    start_offset = translate_offset_thru_rewrites(old_start_offset, rewrites)
                    end_offset = translate_offset_thru_rewrites(old_end_offset, rewrites)

                    print(
                        "~~~~~~~~~~~~~~~~ translating offsets:",
                        old_start_offset,
                        old_end_offset,
                        "to",
                        start_offset,
                        end_offset,
                    )

                    original_text = content[start_offset:end_offset].decode("utf-8")
                    between_parens = original_text[1:].strip()
                    add_trailing_comma = False
                    if between_parens == "void" or not between_parens:
                        target_offset = end_offset  # replace void (and whitespace)
                    else:
                        add_trailing_comma = True
                        target_offset = start_offset + 1

                    after_opening_paren = start_offset + 1
                    rewriter.add_rewrite(
                        filepath_str,
                        after_opening_paren,
                        target_offset - after_opening_paren,
                        f"struct XjGlobals *{', ' if add_trailing_comma else ''}",
                    )

            snapshot = rewriter.capture_snapshot()
            rewriter.apply_rewrites()
            rewriter.replace_rewrites({})  # clear rewrites

            errors_after, total_errors_after = count_possibly_fixable_errors(index)
            if total_errors_after > 0:
                print(
                    "After adding additional function pointer parameters, "
                    + f"{errors_after} possibly fixable errors remain (of {total_errors_after} total); "
                    + "rolling back these changes."
                )
                rewriter.restore_snapshot(snapshot)
            else:
                print(
                    "After adding additional function pointer parameters, " + "all errors resolved."
                )


def localize_mutable_globals(
    json_path: Path,
    compdb: compilation_database.CompileCommands,
    prev: Path,
    current_codebase: Path,
):
    # Here is an example of the data output by `cc2json`:
    # {
    # "mutated_globals": [
    #     "a_1.foo_xjtr_1",
    #     "a_2.foo_xjtr_2",
    #     "b_1.foo_xjtr_0"
    # ],
    # "escaped_globals": [
    #     "b_1.foo_xjtr_0"
    # ],
    # "call_graph_components": [
    #     {
    #         "call_sites": [
    #             {
    #                 "line": 10,
    #                 "col": 25,
    #                 "p": "main",
    #                 "uf": "main.c"
    #             },
    #             {
    #                 "line": 11,
    #                 "col": 25,
    #                 "p": "main",
    #                 "uf": "main.c"
    #             },
    #             {
    #                 "line": 12,
    #                 "col": 25,
    #                 "p": "main",
    #                 "uf": "main.c"
    #             }
    #         ],
    #         "call_targets": [
    #             "<llvm-link>:a_1"
    #         ],
    #         "all_mutable": true
    #     },
    #     {
    #         "call_sites": [ ... ] }
    # ],
    # "unique_filenames": {
    # "a.c": {"directory": "/home/brk/tenjin/ju_xjres2/c_03_run_cclzyerpp_analysis", "filename": "a.c"},
    # "b.c": {"directory": "/home/brk/tenjin/ju_xjres2/c_03_run_cclzyerpp_analysis", "filename": "b.c"},
    # "main.c": {"directory": "/home/brk/tenjin/ju_xjres2/c_03_run_cclzyerpp_analysis", "filename": "main.c"}
    # },
    # "mutable_global_tissue": {
    #     "directly_accesses": [
    #     "a_1",
    #     "a_2",
    #     "b_1"
    #     ],
    #     "tissue": [
    #     "main",
    #     "a_1",
    #     "a_2",
    #     "b_1"
    #     ]
    # },
    # "global_initializer_references": {
    # "basesort": ["alnumsort"]
    # }
    # }

    # Assume this value holds an instance of the above JSON type
    j: dict = json.load(json_path.open("r"))

    # To localize mutable globals, we perform the following steps:
    # z. Inspect j["global_initializer_references"] to identify those globals which
    #     reference other mutable globals in their initializers. To avoid creating a
    #     self-referential Rust structure, we consider the *referenced* globals to be
    #     ineligible for lifting.
    # 1. Inspect j["call_graph_components"], discarding those which are not all_mutable.
    # 2a. Find the definitions of of each mutated global, excluding those
    #     which are ineligible for lifting.
    # 2b. Construct the transitive closure of all directly used struct/union/enum/typedef
    #     definitions needed to define those globals. Note that fields which use
    #     a struct behind a pointer do not require inclusion of that struct definition,
    #     as the type can be forward-declared. (Such type names should be collected in a separate set.)
    # 3. Construct a `struct XjGlobals` declaration containing all mutable globals
    #    adn the struct/union definitions from step 2b.
    #    Place the declaration in a new header file `xj_globals.h`.
    # 4. Based on the definitions from step 2, initialize a singleton XjGlobals instance
    #    in `main()`, called `xjgv`, and a pointer to it called `xjg`.
    # 5. For each function in the "tissue" part of "mutable_global_tissue", except for `main`,
    #    pass a pointer to the XjGlobals instance, called `xjg`, as an additional first parameter.
    # 6. For each call to a tissue function, pass along the `xjg` parameter.
    # 8. Each syntatic use of a liftable mutable global named WHATEVER is replaced
    #    with `xjg->WHATEVER`.
    # 9. In each file that uses mutable globals, add `#include "xj_globals.h"`
    #    before the first function definition which uses a mutable global.
    #
    # Use the `BatchingRewriter` to perform all of these rewrites in a single pass.

    compdb.to_json_file(current_codebase / "compile_commands.json")

    nonmain_tissue_functions: set[str] = set(j.get("mutable_global_tissue", {}).get("tissue", []))
    nonmain_tissue_functions.discard("main")  # Don't modify main

    print("calling localize_mutable_globals_phase1()...")
    time_start = time.time()
    phase1results = localize_mutable_globals_phase1(
        compdb, j, current_codebase, prev, nonmain_tissue_functions
    )
    time_elapsed = time.time() - time_start
    print(f"... localize_mutable_globals_phase1() done, elapsed: {time_elapsed:.1f}")

    speculatively_fix_higher_order_fn_ptr_types(compdb, phase1results)

    index = create_xj_clang_index()
    tus = parse_project(index, compdb)

    globals_and_statics = compute_globals_and_statics_for_translation_units(
        list(tus.values()), elide_functions=True
    )
    liftable_mutated_globals_and_statics = [
        c
        for c in globals_and_statics
        if c.spelling in phase1results.mutd_global_names
        and c.spelling not in phase1results.ineligible_for_lifting
    ]

    if not liftable_mutated_globals_and_statics:
        print("No liftable mutated globals found; skipping further localization steps.")
        return

    # Step 2b: Construct transitive closure of struct/union definitions
    print("\n" + "=" * 80)
    print("STEP 2b: Finding struct/union dependencies")
    print("=" * 80)

    needed_struct_defs = {}
    needed_typedefs: dict[str, tuple[Cursor, str]] = {}
    forward_declarable_types = set()

    def collect_type_dependencies(type_obj_noncanonical, depth=0):
        """Recursively collect struct/union types needed to define this type."""
        # indent = "  " * depth

        # Get the canonical type
        type_obj_canonical = type_obj_noncanonical.get_canonical()

        # print(
        #     f"{indent}Analyzing type: {type_obj_noncanonical.spelling} (kind: {type_obj_noncanonical.kind}) "
        #     + f" [canon spelling: {type_obj_canonical.spelling}]"
        #     + f" @canon {type_obj_canonical.get_declaration().location}"
        #     + f" @noncanon {type_obj_noncanonical.get_declaration().location}"
        # )

        # Handle occurrences of typedef'ed names
        if type_obj_noncanonical.kind == TypeKind.ELABORATED:
            decl = type_obj_noncanonical.get_declaration()
            if decl.kind == CursorKind.TYPEDEF_DECL:
                type_name = decl.spelling
                assert type_name, "Typedef without a name?"
                if type_name not in needed_typedefs:
                    # print(f"{indent}  -> Need typedef: {type_name}")
                    needed_typedefs[type_name] = (
                        decl,
                        decl.underlying_typedef_type.get_canonical().spelling,
                    )
                elif needed_typedefs[type_name][0] == decl:
                    pass
                elif (
                    needed_typedefs[type_name][1]
                    == decl.underlying_typedef_type.get_canonical().spelling
                ):
                    # Typedefs in preprocessed code can be duplicated between translation units,
                    # as long as the associated types are identical, it's all good.
                    pass
                elif needed_typedefs[type_name][0] != decl:
                    raise ValueError(
                        f"Typedef {type_name} already recorded, but different declaration!"
                    )

            # Continue with the underlying type
            decl_def = decl.get_definition()
            if decl_def:
                typedef_decl = decl_def.referenced
                # print(f"{indent}  Saw typedef elaborated...")
                # print(f"{indent}    typedef cursor: {typedef_decl.kind}")
                # print(f"{indent}    typedef cursor: {typedef_decl.extent}")
                # print(f"{indent}    typedef type: {typedef_decl.type}")
                # print(f"{indent}    typedef type: {typedef_decl.type.kind}")
                collect_type_dependencies(typedef_decl.type, depth + 1)
            # else:
            #     print(f"{indent}  WARNING: Elaborated typedef has no definition! {decl.location}")
            return

        if type_obj_noncanonical.kind == TypeKind.TYPEDEF:
            typedef_decl = type_obj_noncanonical.get_declaration()
            # print(f"{indent}  Saw typedef...")
            # print(f"{indent}    typedef cursor: {typedef_decl.kind}")
            # print(f"{indent}    typedef cursor: {typedef_decl.extent}")
            # print(f"{indent}    typedef type: {typedef_decl.type}")
            # print(f"{indent}    typedef type: {typedef_decl.type.kind}")
            # print(f"{indent}    underlying type: {typedef_decl.underlying_typedef_type.kind}")
            # print(f"{indent}    referenced type: {typedef_decl.get_definition().referenced.kind}")
            collect_type_dependencies(typedef_decl.underlying_typedef_type, depth + 1)
            pass

        if type_obj_canonical.kind == TypeKind.POINTER:
            assert type_obj_noncanonical.kind == TypeKind.POINTER

        # If it's a pointer, the pointee can be forward-declared
        if type_obj_noncanonical.kind == TypeKind.POINTER:
            pointee = type_obj_noncanonical.get_pointee()
            pointee_canonical = pointee.get_canonical()
            # print(f"{indent}  Pointer to: {pointee.spelling}")

            # Check if pointee is a struct/union
            decl = pointee_canonical.get_declaration()
            if decl.kind in [CursorKind.STRUCT_DECL, CursorKind.UNION_DECL]:
                forward_declarable_types.add(decl.spelling)
                # print(f"{indent}  -> Can forward-declare: {decl.spelling}")

            collect_type_dependencies(pointee, depth + 1)
            return

        if type_obj_canonical.kind in (
            TypeKind.CONSTANTARRAY,
            TypeKind.INCOMPLETEARRAY,
            TypeKind.VARIABLEARRAY,
        ):
            assert type_obj_noncanonical.kind == type_obj_canonical.kind

            elem_type = type_obj_noncanonical.get_array_element_type()
            # print(f"{indent}  Array of: {elem_type.spelling}")
            collect_type_dependencies(elem_type, depth + 1)
            return

        if type_obj_noncanonical.kind == TypeKind.FUNCTIONPROTO:
            assert type_obj_noncanonical.kind == type_obj_canonical.kind
            for child in type_obj_noncanonical.argument_types():
                collect_type_dependencies(child, depth + 1)
            collect_type_dependencies(type_obj_noncanonical.get_result(), depth + 1)
            return

        # If it's a struct or union, we need its full definition
        decl = type_obj_noncanonical.get_declaration()

        if decl.kind in [CursorKind.STRUCT_DECL, CursorKind.UNION_DECL]:
            type_name = decl.spelling
            if type_name and type_name not in needed_struct_defs:
                # print(f"{indent}  -> Need full definition: {type_name}")
                needed_struct_defs[type_name] = decl

                # Recursively process fields
                for field in decl.get_children():
                    if field.kind == CursorKind.FIELD_DECL:
                        # print(f"{indent}    Field: {field.spelling} : {field.type.spelling}")
                        collect_type_dependencies(field.type, depth + 2)

    for cursor in liftable_mutated_globals_and_statics:
        # print(f"\nAnalyzing dependencies for {cursor.spelling}:")
        collect_type_dependencies(cursor.type, depth=1)

    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"\nFound {len(liftable_mutated_globals_and_statics)} mutated global definitions:")

    for cursor in liftable_mutated_globals_and_statics:
        print(
            f"  - {cursor.spelling}: {cursor.type.spelling} at {cursor.location.file}:{cursor.location.line}"
        )

    # print(f"\nNeed {len(needed_struct_defs)} struct/union definitions:")
    # for name, decl in needed_struct_defs.items():
    #     print(f"  - {name} -> {decl.location}")
    #     print(f"           -> {decl.extent}")
    #     print(f"           -> {decl.type}")
    #     print(f"           -> {decl.type.spelling}")
    #     print(f"           -> {decl.get_usr()}")
    #     print(f"           -> {decl.get_definition()}")
    #     print(f"           -> {decl.get_definition().extent}")
    #     print()

    # print(f"\nNeed {len(needed_typedefs)} typedef definitions:")
    # for name, (decl, canonical_spelling) in needed_typedefs.items():
    #     print(f"  - {name} -> {decl.location}")
    #     print(f"           -> {decl.extent}")
    #     print(f"           -> {decl.type}")
    #     print(f"           -> {decl.type.spelling}")
    #     print(f"  canon_ty -> {canonical_spelling}")
    #     print(f"           -> {decl.get_usr()}")
    #     print(f"           -> {decl.get_definition()}")
    #     print(f"           -> {decl.get_definition().extent}")
    #     print()

    # print(f"\nCan forward-declare {len(forward_declarable_types)} types:")
    # for name in forward_declarable_types:
    #     print(f"  - {name}")
    print("=" * 80)

    # Steps 5 and 6: Modify function signatures and call sites
    print("\n" + "=" * 80)
    print("STEPS 5 & 6: Modifying function signatures and call sites")
    print("=" * 80)

    print(f"\nTissue functions to modify: {nonmain_tissue_functions}")

    with batching_rewriter.BatchingRewriter() as rewriter:
        # TU -> offset of first fn using mutable globals
        lowest_mutable_accessing_fn_starts: dict[str, int] = {}

        # Step 8: Replace uses of mutable globals with xjg->WHATEVER
        print("\n  --- Step 8: Replacing global variable accesses ---")
        for tu_path, tu in tus.items():
            current_fn_start = None
            for child in tu.cursor.walk_preorder():
                if child.kind == CursorKind.FUNCTION_DECL:
                    if child.is_definition():
                        current_fn_start = child.extent.start.offset
                if (
                    child.kind == CursorKind.DECL_REF_EXPR
                    and child.spelling in phase1results.mutd_global_names
                    and child.spelling not in phase1results.all_function_names
                ):
                    # Get the extent of the variable reference
                    start_offset = child.extent.start.offset
                    end_offset = child.extent.end.offset
                    length = end_offset - start_offset
                    var_name = child.spelling

                    replacement = f"xjg->{var_name}"
                    # print(
                    #     f"    Found DECL_REF_EXPR for {var_name} at {tu_path}:{child.location.line}:{child.location.column}"
                    # )

                    # Check the parent - if it's a VAR_DECL, skip it
                    parent = child.semantic_parent
                    if (
                        parent
                        and parent.kind == CursorKind.VAR_DECL
                        and parent.spelling == var_name
                    ):
                        print("      Skipping: this is part of the declaration")
                        print(child.extent)
                        continue

                    referenced_decl = child.referenced
                    if (
                        referenced_decl
                        and referenced_decl.storage_class == StorageClass.NONE
                        and referenced_decl.linkage == LinkageKind.NO_LINKAGE
                    ):
                        # This is a local variable with the same name as a global; skip it
                        continue

                    # print(f"    Replacing {var_name} with {replacement}")

                    rewriter.add_rewrite(tu_path, start_offset, length, replacement)
                    assert current_fn_start is not None
                    if tu_path not in lowest_mutable_accessing_fn_starts:
                        lowest_mutable_accessing_fn_starts[tu_path] = current_fn_start
                    else:
                        lowest_mutable_accessing_fn_starts[tu_path] = min(
                            lowest_mutable_accessing_fn_starts[tu_path], current_fn_start
                        )

        # Step 3: Create xj_globals.h header file
        print("\n  --- Step 3: Creating xj_globals.h ---")

        # Get the directory where we should place the headers
        # Use the directory from the compilation database
        if compdb.get_source_files():
            header_dir = compdb.get_source_files()[0].parent
        else:
            header_dir = Path(".")

        # Create xj_globals.h with full definitions
        header_path = header_dir / "xj_globals.h"
        print(f"  Creating full definition header at {header_path}")

        # Build the header content
        header_lines = []
        header_lines.append("#ifndef XJ_GLOBALS_H")
        header_lines.append("#define XJ_GLOBALS_H")
        header_lines.append("")

        # Add forward declarations if needed
        if forward_declarable_types:
            for type_name in sorted(forward_declarable_types):
                # Determine if it's a struct or union
                # We'll default to struct (can improve this later)
                header_lines.append(f"struct {type_name};")
            header_lines.append("")

        # Add typedefs
        # typedefs_sorted_by_line = sorted(
        #     list(needed_typedefs.items()), key=lambda item: item[1][0].location.line
        # )
        # if typedefs_sorted_by_line:
        #     header_lines.append("// typedefs_sorted_by_line")
        #     for name, (decl_cursor, _u_t_canonical_spelling) in typedefs_sorted_by_line:
        #         # Get the full definition text
        #         start_offset = decl_cursor.extent.start.offset
        #         end_offset = decl_cursor.extent.end.offset
        #         # Find the file containing this definition
        #         for tu_path, tu in tus.items():
        #             if str(tu_path) == decl_cursor.location.file.name:
        #                 content = rewriter.get_content(tu_path)
        #                 typedef_text = content[start_offset:end_offset].decode("utf-8")
        #                 header_lines.append(typedef_text + ";")
        #                 break

        #     header_lines.append("")

        # # Add full struct/union definitions from step 2b
        # if needed_struct_defs:
        #     header_lines.append("// needed_struct_defs")
        #     for type_name, decl_cursor in needed_struct_defs.items():
        #         # Get the full definition text
        #         # We need to extract the source text for this struct/union
        #         start_offset = decl_cursor.extent.start.offset
        #         end_offset = decl_cursor.extent.end.offset

        #         # Find the file containing this definition
        #         for tu_path, tu in tus.items():
        #             if str(tu_path) == decl_cursor.location.file.name:
        #                 content = rewriter.get_content(tu_path)
        #                 struct_text = content[start_offset:end_offset].decode("utf-8")
        #                 header_lines.append(struct_text + ";")
        #                 header_lines.append("")
        #                 break

        # Add the XjGlobals struct definition
        mutated_globals_cursors_by_name = {
            c.spelling: c for c in liftable_mutated_globals_and_statics
        }
        assert len(mutated_globals_cursors_by_name) == len(liftable_mutated_globals_and_statics), (
            "Expected all (liftable) mutated global names to be unique, "
            + f"but got duplicates within: {mutated_globals_cursors_by_name.keys()}"
        )

        header_lines.append("struct XjGlobals {")
        for global_name in sorted(mutated_globals_cursors_by_name.keys()):
            var_cursor = mutated_globals_cursors_by_name[global_name]
            # Add the field (we'll handle initialization separately)
            header_lines.append(
                render_declaration_sans_qualifiers(var_cursor.type, var_cursor.spelling) + ";"
            )

        header_lines.append("};")
        header_lines.append("")
        header_lines.append("#endif /* XJ_GLOBALS_H */")
        header_lines.append("")

        # Write the header file
        with open(header_path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(header_lines))

        # Step 4: Initialize xjgv in main()
        print("\n  --- Step 4: Initializing xjgv in main() ---")

        # First, we need to detect which globals reference other globals
        # and build a dependency graph
        print("\n  Analyzing global dependencies for initializers...")

        global_dependencies: dict[str, set[str]] = {}  # global_name -> set of referenced globals

        for var_cursor in liftable_mutated_globals_and_statics:
            dependencies = set()

            # Walk through the initializer expression to find DECL_REF_EXPR nodes
            for child in var_cursor.walk_preorder():
                if (
                    child.kind == CursorKind.DECL_REF_EXPR
                    and child.spelling not in phase1results.mutd_global_names
                ):
                    dependencies.add(child.spelling)
                    print(f"    {var_cursor.spelling} references {child.spelling}")

            global_dependencies[var_cursor.spelling] = dependencies

        # Compute transitive closure of dependencies for all globals
        # We need to copy (not move) any global that is referenced by another
        globals_to_copy_to_main = set()

        def collect_transitive_deps(global_name: str, visited: set[str]) -> None:
            if global_name in visited:
                return
            visited.add(global_name)
            for dep in global_dependencies.get(global_name, set()):
                globals_to_copy_to_main.add(dep)
                collect_transitive_deps(dep, visited)

        for global_name in phase1results.mutd_global_names:
            collect_transitive_deps(global_name, set())

        print(f"\n  Globals to copy into main before xjgv: {globals_to_copy_to_main}")

        # Find main() function and insert initialization at the beginning
        for tu_path, tu in tus.items():
            for cursor in tu.cursor.walk_preorder():
                if (
                    cursor.kind == CursorKind.FUNCTION_DECL
                    and cursor.spelling == "main"
                    and cursor.is_definition()
                ):
                    print(f"  Found main() at {tu_path}:{cursor.location.line}")

                    globals_and_statics_by_name = {c.spelling: c for c in globals_and_statics}

                    # Find the opening brace of main's body
                    # The compound statement is a child of the function
                    for child in cursor.get_children():
                        if child.kind == CursorKind.COMPOUND_STMT:
                            # Insert after the opening brace
                            insert_offset = child.extent.start.offset + 1

                            # Build initialization code
                            init_lines = []

                            # First, copy definitions of referenced globals
                            if globals_to_copy_to_main:
                                init_lines.append("")
                                init_lines.append(
                                    "// Local copies of globals referenced by other globals"
                                )

                                # Sort by dependency order (topological sort)
                                sorted_globals = []
                                visited = set()

                                def visit_for_topo(g: str) -> None:
                                    if g in visited or g not in globals_to_copy_to_main:
                                        return
                                    visited.add(g)
                                    for dep in global_dependencies.get(g, set()):
                                        if dep in globals_to_copy_to_main:
                                            visit_for_topo(dep)
                                    sorted_globals.append(g)

                                for g in globals_to_copy_to_main:
                                    visit_for_topo(g)

                                # Generate local variable definitions
                                for global_name in sorted_globals:
                                    var_cursor = globals_and_statics_by_name.get(global_name)
                                    if var_cursor is None:
                                        # We want to copy immutable globals, but function that
                                        # escape are not in the globals_and_statics list, and
                                        # we don't want to (& cannot) duplicate their definitions
                                        # within main().
                                        continue

                                    # Get the initializer value
                                    initializer = "0"  # Default
                                    for child_node in var_cursor.get_children():
                                        if child_node.kind != CursorKind.TYPE_REF:
                                            init_start = child_node.extent.start.offset
                                            init_end = child_node.extent.end.offset
                                            content = rewriter.get_content(
                                                var_cursor.location.file.name
                                            )
                                            initializer = (
                                                content[init_start:init_end].decode("utf-8").strip()
                                            )
                                            break

                                    init_lines.append(
                                        f"  static {render_declaration_sans_qualifiers(var_cursor.type, var_cursor.spelling)} = {initializer};"
                                    )

                                init_lines.append("")

                            # Initialize each field based on original initializers
                            field_inits = []
                            for global_name in sorted(mutated_globals_cursors_by_name.keys()):
                                var_cursor = mutated_globals_cursors_by_name[global_name]
                                content = rewriter.get_content(var_cursor.location.file.name)
                                if global_name.startswith("pathsize_"):
                                    print(f"   Special handling for {global_name}")
                                    for child_node in var_cursor.get_children():
                                        print("    child node:", child_node.kind, child_node.extent)
                                        print(
                                            "   child node text:",
                                            content[
                                                child_node.extent.start.offset : child_node.extent.end.offset
                                            ],
                                        )
                                        print()

                                if global_name in phase1results.globals_without_initializers:
                                    initializer = "{0}"
                                elif var_cursor.is_definition():
                                    child_node = list(var_cursor.get_children())[-1]
                                    if child_node.kind == CursorKind.TYPE_REF:
                                        # No initializer
                                        initializer = "0"
                                    else:
                                        init_start = child_node.extent.start.offset
                                        init_end = child_node.extent.end.offset
                                        initializer = (
                                            content[init_start:init_end].decode("utf-8").strip()
                                        )
                                else:
                                    initializer = "0"

                                field_inits.append(f"    .{global_name} = {initializer}")

                            init_lines.append("\n  struct XjGlobals xjgv = {")
                            init_lines.append(",\n".join(field_inits))
                            init_lines.append("\n  };")
                            init_lines.append("struct XjGlobals *xjg = &xjgv;\n")

                            init_text = "\n".join(init_lines)
                            rewriter.add_rewrite(tu_path, insert_offset, 0, init_text)
                            print("  Added xjgv initialization in main()")
                            break
                    break

        # Step 9: Add includes and type definitions to files that use mutable globals
        print("\n  --- Step 9: Adding includes and type definitions ---")

        # Phase 1 only inserted forward declarations, we'll also add the header as needed.
        # (not much point in replacing the forward declarations).
        for tu_path, offset in lowest_mutable_accessing_fn_starts.items():
            tu = tus[tu_path]

            # print(f"\n  Analyzing types in scope in TU: {tu_path}")

            types_in_scope = set()  # Set of type names (struct/union/typedef)

            # Find all struct/union/typedef declarations
            for cursor in tu.cursor.walk_preorder():
                if cursor.kind == CursorKind.STRUCT_DECL and cursor.spelling:
                    types_in_scope.add(cursor.spelling)
                    # print(f"    Found struct in scope: {cursor.spelling}")
                elif cursor.kind == CursorKind.UNION_DECL and cursor.spelling:
                    types_in_scope.add(cursor.spelling)
                    # print(f"    Found union in scope: {cursor.spelling}")
                elif cursor.kind == CursorKind.TYPEDEF_DECL and cursor.spelling:
                    types_in_scope.add(cursor.spelling)
                    # print(f"    Found typedef in scope: {cursor.spelling}")

            # print(f"\n  Found {len(types_in_scope)} types already in scope in TU: {tu_path}")

            # Determine which types need to be emitted
            types_to_emit_structs: dict[str, Cursor] = {}  # name -> decl_cursor
            types_to_emit_typedefs: dict[str, Cursor] = {}  # name -> decl_cursor

            for type_name, decl_cursor in needed_struct_defs.items():
                if type_name not in types_in_scope:
                    types_to_emit_structs[type_name] = decl_cursor
                #     print(f"    Will emit struct definition: {type_name}")
                # else:
                #     print(f"    Skipping struct (already in scope): {type_name}")

            for type_name, decl_cursor in needed_typedefs.items():
                if type_name not in types_in_scope:
                    types_to_emit_typedefs[type_name] = decl_cursor
                #     print(f"    Will emit typedef: {type_name}")
                # else:
                #     print(f"    Skipping typedef (already in scope): {type_name}")

            # Build type definitions to insert before main()
            type_defs_lines: list[str] = []
            type_defs_lines.append("\n// Type definitions needed for XjGlobals")

            # Add forward declarations if needed
            forward_decls_to_emit = forward_declarable_types - types_in_scope
            if forward_decls_to_emit:
                for type_name in sorted(forward_decls_to_emit):
                    type_defs_lines.append(f"struct {type_name};")

            # Add typedefs
            if types_to_emit_typedefs:
                typedefs_sorted = sorted(
                    list(types_to_emit_typedefs.items()), key=lambda item: item[1].location.line
                )
                for name, decl_cursor in typedefs_sorted:
                    start_offset = decl_cursor.extent.start.offset
                    end_offset = decl_cursor.extent.end.offset
                    for tu_path, tu in tus.items():
                        if str(tu_path) == decl_cursor.location.file.name:
                            content = rewriter.get_content(tu_path)
                            typedef_text = content[start_offset:end_offset].decode("utf-8")
                            type_defs_lines.append(typedef_text + ";")
                            break

            # Add struct/union definitions
            if types_to_emit_structs:
                for type_name, decl_cursor in types_to_emit_structs.items():
                    start_offset = decl_cursor.extent.start.offset
                    end_offset = decl_cursor.extent.end.offset
                    for tu_path, tu in tus.items():
                        if str(tu_path) == decl_cursor.location.file.name:
                            content = rewriter.get_content(tu_path)
                            struct_text = content[start_offset:end_offset].decode("utf-8")
                            type_defs_lines.append(struct_text + ";")
                            break

            type_defs_lines.append('#include "xj_globals.h"')

            type_defs_text = "\n".join(type_defs_lines) + "\n"
            rewriter.add_rewrite(tu_path, offset, 0, type_defs_text)

    # After all batched rewrites have been applied, replace the XJG_PLACEHOLDER
    # in all files. (This is done afterwards to avoid changing offsets
    # during the main rewrite phase.)
    for tu_path in tus.keys():
        with open(tu_path, "r", encoding="utf-8") as fh:
            content = fh.read()
        if XJG_PLACEHOLDER in content:
            content = content.replace(XJG_PLACEHOLDER, "xjg")
            with open(tu_path, "w", encoding="utf-8") as fh:
                fh.write(content)

    update_vars_of_type_guidance_for_xjg(current_codebase, phase1results, tus)

    print(f"{phase1results.all_function_names=}")

    print("=" * 80)


# Update the guidance to have the xjg parameter passed as &mut when possible.
# Functions which are used in higher-order ways must remain as raw pointers.
def update_vars_of_type_guidance_for_xjg(
    current_codebase: Path,
    phase1results: LocalizeMutableGlobalsPhase1Results,
    tus: dict[str, TranslationUnit],
):
    higher_order_tissue_functions = set()
    for _tu_path, tu in tus.items():
        for v, ancestors in yield_matching_cursors(tu.cursor, [CursorKind.DECL_REF_EXPR]):
            if v.spelling in phase1results.nonmain_tissue_functions:
                # Found a use of a tissue function; was it in a call position?
                parent = v
                while ancestors:
                    parent, rest = ancestors
                    if rest is None:
                        break
                    if not parent.kind.is_unexposed():
                        break
                    ancestors = rest
                if parent.kind != CursorKind.CALL_EXPR:
                    higher_order_tissue_functions.add(v.spelling)
                else:
                    # might be a callee, or a call arg
                    callee = next(parent.get_children(), None)
                    if callee and callee.spelling == v.spelling:
                        # direct call, all good
                        pass
                    else:
                        # passed as an argument
                        higher_order_tissue_functions.add(v.spelling)
    guidance: dict = json.load(open(current_codebase / XJ_GUIDANCE_FILENAME, "r", encoding="utf-8"))
    can_take_mut_xjg = phase1results.nonmain_tissue_functions - higher_order_tissue_functions
    mut_specs = guidance.get("vars_of_type", {}).get("&mut XjGlobals", [])
    for tissue_fn_name in can_take_mut_xjg:
        mut_specs.append(f"{tissue_fn_name}:xjg")
    guidance.setdefault("vars_of_type", {})["&mut XjGlobals"] = mut_specs
    with open(current_codebase / XJ_GUIDANCE_FILENAME, "w", encoding="utf-8") as fh:
        json.dump(guidance, fh, indent=2)


def extract_function_info(
    tus, nonmain_tissue_functions
) -> tuple[set[str], dict[str, list[TissueFunctionCursorInfo]]]:
    all_function_names = set()

    # Collect all declarations (both definitions and forward declarations)
    nonmain_tissue_function_cursors: dict[str, list[TissueFunctionCursorInfo]] = {}

    for abs_path, tu in tus.items():
        for cursor in tu.cursor.walk_preorder():
            if cursor.kind == CursorKind.FUNCTION_DECL:
                func_name = cursor.spelling
                all_function_names.add(func_name)
                if func_name in nonmain_tissue_functions:
                    if func_name not in nonmain_tissue_function_cursors:
                        nonmain_tissue_function_cursors[func_name] = []
                    nonmain_tissue_function_cursors[func_name].append(
                        TissueFunctionCursorInfo(
                            cursor=cursor,
                            file=abs_path,
                            is_definition=cursor.is_definition(),
                        )
                    )

    return all_function_names, nonmain_tissue_function_cursors


def get_call_sites_from_json(
    prev: Path,
    current_codebase: Path,
    j: dict,
    nonmain_tissue_functions: set[str],
    escd_global_names: set[str],
) -> list[TissueCallSiteInfo]:
    def un_uf(uf: str) -> str:
        v = j["unique_filenames"][uf]
        return v["directory"] + "/" + v["filename"]

    fns_with_possibly_unknown_call_sites = nonmain_tissue_functions.intersection(escd_global_names)
    if fns_with_possibly_unknown_call_sites:
        print(
            f"""
===============================================================================
Error: Some functions which access mutable globals (or may eventually call one)
    appear to have unknown call sites outside of our control. They are:

{fns_with_possibly_unknown_call_sites}

To help produce safe Rust output, we seek to move all mutable globals into
   a context struct which is passed explicitly to functions that need them.
   So the target functions involved must have their signatures modified to
   take an additional parameter.
But we cannot safely modify the signatures
   of functions that have call sites outside of our control. An example
   of such a call site would be `qsort()` calling a comparison function.
Our determination of what functions might have unknown call sites is
   by necessity conservative; it's uncomputable in general. Any function
   pointer in a struct passed to an unknown third-party library function
   might end up being called by that library. And when analyzing library
   code (without a `main()` function), any non-static global is likewise
   potentially accessible by whatever code links against it.
Often an application will use function pointers in ways that are hard
   for a static analyzer to track, but which a human can verify do not
   have any external call sites.
If, upon review, you believe the functions listed above do not escape,
   you can add their names to the "assume_no_unknown_call_sites" list in
   the input guidance provided to Tenjin.

If a function which accesses globals does in fact get called externally,
   there are two ways one might address the situation:

   1. Mark the globals it accesses as being ineligible
      for lifting into the context struct.
   2. Modify the C code by hand to access globals
      via an explicit context struct.

For many standard library functions which call user-provided function pointers,
a bare function pointer is often paired with a `void*` context parameter.
For `qsort()` specifically, context parameter support is only provided by
non-standard variants like `qsort_r()`.
    See https://stackoverflow.com/a/39561369/169305 for details.

Tenjin is not yet sophisticated enough to do option 2 automatically.

Here are the call sites which may have been affected by this issue:
"""
        )
        print()
        affected_call_sites_by_location: dict[tuple[str, int, int], list[str]] = {}

        for component in j.get("call_graph_components", []):
            if component.get("all_mutable", True):
                continue  # all-mutable call sites not affected!
            call_targets = component.get("call_targets", [])
            affected_targets = []
            for target in call_targets:
                # Target format: "<llvm-link>:function_name"
                if ":" in target:
                    callee_func = target.split(":")[-1]
                    if callee_func in nonmain_tissue_functions:
                        affected_targets.append(callee_func)

            if affected_targets:
                # Record all call sites for these targets
                for site in component.get("call_sites", []):
                    caller_func = site.get("p")
                    line = site.get("line")
                    col = site.get("col")

                    # Get the actual file path - need to adjust for current directory
                    # The JSON has paths from c_03 but we're working in c_04
                    file_path_old = un_uf(site.get("uf"))
                    assert file_path_old.startswith(prev.as_posix())
                    i_file_path = file_path_old.replace(
                        prev.as_posix(), current_codebase.as_posix()
                    )

                    location_key = (i_file_path, line, col)
                    if location_key not in affected_call_sites_by_location:
                        affected_call_sites_by_location[location_key] = []
                    affected_call_sites_by_location[location_key].extend(affected_targets)

        lines_by_file_path = {}
        # Sort by file path, then line, then column
        for location_key in sorted(affected_call_sites_by_location.keys()):
            i_file_path, line, col = location_key
            target_funcs = affected_call_sites_by_location[location_key]
            print(f"Call site at {i_file_path}:{line}:{col}")
            startline = max(0, line - 2)
            endline = line + 1  # include one line after
            if i_file_path not in lines_by_file_path:
                with open(i_file_path, "r", encoding="utf-8") as fh:
                    lines_by_file_path[i_file_path] = fh.readlines()
            file_lines = lines_by_file_path[i_file_path][startline:endline]
            for idx, file_line in enumerate(file_lines, start=startline + 1):
                pointer = ">> " if idx == line else "   "
                print(f"{pointer}{idx:4d}: {file_line.rstrip()}")
            print()
            print("            May call: " + " or ".join(target_funcs))
            print()

        print("""
To err on the side of safety, we currently lift all mutable globals,
but indirect calls to functions that might escape must be left unchanged,
to match the signatures expected by the external callers.

Rather than "resolving" this mismatch automatically by not lifting any
globals transitively accessed during the execution of conflicted functions
              (which could easily mean silently not lifting
               any globals at all; a very confusing outcome),
we're instead going to print this wall of text for you to enjoy. Oh,
and we shan't proceed further with the refactoring, either, since the
code we'd be generating would almost certainly have type mismatches.
===============================================================================
              """)
        raise ValueError("please look ABOVE traceback for error info")

    call_sites: list[TissueCallSiteInfo] = []

    # Get call sites from JSON (more reliable than libclang semantic_parent)
    for component in j.get("call_graph_components", []):
        if not component.get("all_mutable", False):
            continue
        component_call_sites: list[TissueCallSiteInfo] = []
        call_targets = component.get("call_targets", [])
        call_target_fn_names = [target.split(":")[-1] for target in call_targets]
        if any(fn in nonmain_tissue_functions for fn in call_target_fn_names):
            for site in component.get("call_sites", []):
                caller_func = site.get("p")
                line = site.get("line")
                col = site.get("col")

                # Get the actual file path - need to adjust for current directory
                # The JSON has paths from c_03 but we're working in c_04
                file_path_old = un_uf(site.get("uf"))
                assert file_path_old.startswith(prev.as_posix())
                i_file_path = file_path_old.replace(prev.as_posix(), current_codebase.as_posix())

                component_call_sites.append(
                    TissueCallSiteInfo(
                        caller_func=caller_func,
                        callee_funcs=call_target_fn_names,
                        i_file_path=i_file_path,
                        line=line,
                        col=col,
                    )
                )
            call_sites.extend(component_call_sites)

        # Mixed-usage call sites, which call both tissue and non-tissue functions,
        # would lead to undefined behavior if unaddressed. In FindFnPtrDecls.cpp
        # we detect when a non-tissue function pointer is assigned to a location
        # with a modified signature, and generate a wrapper for it. This should
        # ensure correct operation, but does not get reflected in the call target
        # info at hand, which reflects the pre-wrapper-insertion code. Just in case
        # our analysis is incomplete, here's some code to help identify mixed-usage
        # call sites for debugging.
        if False:
            ctfn = set(call_target_fn_names)
            ctfn_tissue = ctfn.intersection(nonmain_tissue_functions)
            ctfn_nontissue = ctfn - ctfn_tissue
            if ctfn_tissue and ctfn_nontissue:
                print("WARNING: found mixed-usage call sites, which appear to call a mix")
                print("         of global-accessing functions (which need modified signatures)")
                print("         and non-global-accessing functions (which do not).")
                print(
                    "         This risks undefined behavior due to conflicting function signatures."
                )
                for idx, site in enumerate(component_call_sites):
                    andq = "and" if idx > 0 else "   "
                    print(f"{andq} Call site at {site.i_file_path}:{site.line}:{site.col}")

                print("May call global-accessing functions: " + ", ".join(ctfn_tissue))
                print("  or non-global-accessing functions: " + ", ".join(ctfn_nontissue))

    return call_sites


"""
These don't appear to be exposed to IDE completion...

Cursor kinds as of LLVM 18.1.8:
    [
        CursorKind.UNEXPOSED_DECL,
        CursorKind.STRUCT_DECL,
        CursorKind.UNION_DECL,
        CursorKind.CLASS_DECL,
        CursorKind.ENUM_DECL,
        CursorKind.FIELD_DECL,
        CursorKind.ENUM_CONSTANT_DECL,
        CursorKind.FUNCTION_DECL,
        CursorKind.VAR_DECL,
        CursorKind.PARM_DECL,
        CursorKind.OBJC_INTERFACE_DECL,
        CursorKind.OBJC_CATEGORY_DECL,
        CursorKind.OBJC_PROTOCOL_DECL,
        CursorKind.OBJC_PROPERTY_DECL,
        CursorKind.OBJC_IVAR_DECL,
        CursorKind.OBJC_INSTANCE_METHOD_DECL,
        CursorKind.OBJC_CLASS_METHOD_DECL,
        CursorKind.OBJC_IMPLEMENTATION_DECL,
        CursorKind.OBJC_CATEGORY_IMPL_DECL,
        CursorKind.TYPEDEF_DECL,
        CursorKind.CXX_METHOD,
        CursorKind.NAMESPACE,
        CursorKind.LINKAGE_SPEC,
        CursorKind.CONSTRUCTOR,
        CursorKind.DESTRUCTOR,
        CursorKind.CONVERSION_FUNCTION,
        CursorKind.TEMPLATE_TYPE_PARAMETER,
        CursorKind.TEMPLATE_NON_TYPE_PARAMETER,
        CursorKind.TEMPLATE_TEMPLATE_PARAMETER,
        CursorKind.FUNCTION_TEMPLATE,
        CursorKind.CLASS_TEMPLATE,
        CursorKind.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION,
        CursorKind.NAMESPACE_ALIAS,
        CursorKind.USING_DIRECTIVE,
        CursorKind.USING_DECLARATION,
        CursorKind.TYPE_ALIAS_DECL,
        CursorKind.OBJC_SYNTHESIZE_DECL,
        CursorKind.OBJC_DYNAMIC_DECL,
        CursorKind.CXX_ACCESS_SPEC_DECL,
        CursorKind.OBJC_SUPER_CLASS_REF,
        CursorKind.OBJC_PROTOCOL_REF,
        CursorKind.OBJC_CLASS_REF,
        CursorKind.TYPE_REF,
        CursorKind.CXX_BASE_SPECIFIER,
        CursorKind.TEMPLATE_REF,
        CursorKind.NAMESPACE_REF,
        CursorKind.MEMBER_REF,
        CursorKind.LABEL_REF,
        CursorKind.OVERLOADED_DECL_REF,
        CursorKind.VARIABLE_REF,
        CursorKind.INVALID_FILE,
        CursorKind.NO_DECL_FOUND,
        CursorKind.NOT_IMPLEMENTED,
        CursorKind.INVALID_CODE,
        CursorKind.UNEXPOSED_EXPR,
        CursorKind.DECL_REF_EXPR,
        CursorKind.MEMBER_REF_EXPR,
        CursorKind.CALL_EXPR,
        CursorKind.OBJC_MESSAGE_EXPR,
        CursorKind.BLOCK_EXPR,
        CursorKind.INTEGER_LITERAL,
        CursorKind.FLOATING_LITERAL,
        CursorKind.IMAGINARY_LITERAL,
        CursorKind.STRING_LITERAL,
        CursorKind.CHARACTER_LITERAL,
        CursorKind.PAREN_EXPR,
        CursorKind.UNARY_OPERATOR,
        CursorKind.ARRAY_SUBSCRIPT_EXPR,
        CursorKind.BINARY_OPERATOR,
        CursorKind.COMPOUND_ASSIGNMENT_OPERATOR,
        CursorKind.CONDITIONAL_OPERATOR,
        CursorKind.CSTYLE_CAST_EXPR,
        CursorKind.COMPOUND_LITERAL_EXPR,
        CursorKind.INIT_LIST_EXPR,
        CursorKind.ADDR_LABEL_EXPR,
        CursorKind.StmtExpr,
        CursorKind.GENERIC_SELECTION_EXPR,
        CursorKind.GNU_NULL_EXPR,
        CursorKind.CXX_STATIC_CAST_EXPR,
        CursorKind.CXX_DYNAMIC_CAST_EXPR,
        CursorKind.CXX_REINTERPRET_CAST_EXPR,
        CursorKind.CXX_CONST_CAST_EXPR,
        CursorKind.CXX_FUNCTIONAL_CAST_EXPR,
        CursorKind.CXX_TYPEID_EXPR,
        CursorKind.CXX_BOOL_LITERAL_EXPR,
        CursorKind.CXX_NULL_PTR_LITERAL_EXPR,
        CursorKind.CXX_THIS_EXPR,
        CursorKind.CXX_THROW_EXPR,
        CursorKind.CXX_NEW_EXPR,
        CursorKind.CXX_DELETE_EXPR,
        CursorKind.CXX_UNARY_EXPR,
        CursorKind.OBJC_STRING_LITERAL,
        CursorKind.OBJC_ENCODE_EXPR,
        CursorKind.OBJC_SELECTOR_EXPR,
        CursorKind.OBJC_PROTOCOL_EXPR,
        CursorKind.OBJC_BRIDGE_CAST_EXPR,
        CursorKind.PACK_EXPANSION_EXPR,
        CursorKind.SIZE_OF_PACK_EXPR,
        CursorKind.LAMBDA_EXPR,
        CursorKind.OBJ_BOOL_LITERAL_EXPR,
        CursorKind.OBJ_SELF_EXPR,
        CursorKind.OMP_ARRAY_SECTION_EXPR,
        CursorKind.OBJC_AVAILABILITY_CHECK_EXPR,
        CursorKind.UNEXPOSED_STMT,
        CursorKind.LABEL_STMT,
        CursorKind.COMPOUND_STMT,
        CursorKind.CASE_STMT,
        CursorKind.DEFAULT_STMT,
        CursorKind.IF_STMT,
        CursorKind.SWITCH_STMT,
        CursorKind.WHILE_STMT,
        CursorKind.DO_STMT,
        CursorKind.FOR_STMT,
        CursorKind.GOTO_STMT,
        CursorKind.INDIRECT_GOTO_STMT,
        CursorKind.CONTINUE_STMT,
        CursorKind.BREAK_STMT,
        CursorKind.RETURN_STMT,
        CursorKind.ASM_STMT,
        CursorKind.OBJC_AT_TRY_STMT,
        CursorKind.OBJC_AT_CATCH_STMT,
        CursorKind.OBJC_AT_FINALLY_STMT,
        CursorKind.OBJC_AT_THROW_STMT,
        CursorKind.OBJC_AT_SYNCHRONIZED_STMT,
        CursorKind.OBJC_AUTORELEASE_POOL_STMT,
        CursorKind.OBJC_FOR_COLLECTION_STMT,
        CursorKind.CXX_CATCH_STMT,
        CursorKind.CXX_TRY_STMT,
        CursorKind.CXX_FOR_RANGE_STMT,
        CursorKind.SEH_TRY_STMT,
        CursorKind.SEH_EXCEPT_STMT,
        CursorKind.SEH_FINALLY_STMT,
        CursorKind.MS_ASM_STMT,
        CursorKind.NULL_STMT,
        CursorKind.DECL_STMT,
        CursorKind.OMP_PARALLEL_DIRECTIVE,
        CursorKind.OMP_SIMD_DIRECTIVE,
        CursorKind.OMP_FOR_DIRECTIVE,
        CursorKind.OMP_SECTIONS_DIRECTIVE,
        CursorKind.OMP_SECTION_DIRECTIVE,
        CursorKind.OMP_SINGLE_DIRECTIVE,
        CursorKind.OMP_PARALLEL_FOR_DIRECTIVE,
        CursorKind.OMP_PARALLEL_SECTIONS_DIRECTIVE,
        CursorKind.OMP_TASK_DIRECTIVE,
        CursorKind.OMP_MASTER_DIRECTIVE,
        CursorKind.OMP_CRITICAL_DIRECTIVE,
        CursorKind.OMP_TASKYIELD_DIRECTIVE,
        CursorKind.OMP_BARRIER_DIRECTIVE,
        CursorKind.OMP_TASKWAIT_DIRECTIVE,
        CursorKind.OMP_FLUSH_DIRECTIVE,
        CursorKind.SEH_LEAVE_STMT,
        CursorKind.OMP_ORDERED_DIRECTIVE,
        CursorKind.OMP_ATOMIC_DIRECTIVE,
        CursorKind.OMP_FOR_SIMD_DIRECTIVE,
        CursorKind.OMP_PARALLELFORSIMD_DIRECTIVE,
        CursorKind.OMP_TARGET_DIRECTIVE,
        CursorKind.OMP_TEAMS_DIRECTIVE,
        CursorKind.OMP_TASKGROUP_DIRECTIVE,
        CursorKind.OMP_CANCELLATION_POINT_DIRECTIVE,
        CursorKind.OMP_CANCEL_DIRECTIVE,
        CursorKind.OMP_TARGET_DATA_DIRECTIVE,
        CursorKind.OMP_TASK_LOOP_DIRECTIVE,
        CursorKind.OMP_TASK_LOOP_SIMD_DIRECTIVE,
        CursorKind.OMP_DISTRIBUTE_DIRECTIVE,
        CursorKind.OMP_TARGET_ENTER_DATA_DIRECTIVE,
        CursorKind.OMP_TARGET_EXIT_DATA_DIRECTIVE,
        CursorKind.OMP_TARGET_PARALLEL_DIRECTIVE,
        CursorKind.OMP_TARGET_PARALLELFOR_DIRECTIVE,
        CursorKind.OMP_TARGET_UPDATE_DIRECTIVE,
        CursorKind.OMP_DISTRIBUTE_PARALLELFOR_DIRECTIVE,
        CursorKind.OMP_DISTRIBUTE_PARALLEL_FOR_SIMD_DIRECTIVE,
        CursorKind.OMP_DISTRIBUTE_SIMD_DIRECTIVE,
        CursorKind.OMP_TARGET_PARALLEL_FOR_SIMD_DIRECTIVE,
        CursorKind.OMP_TARGET_SIMD_DIRECTIVE,
        CursorKind.OMP_TEAMS_DISTRIBUTE_DIRECTIVE,
        CursorKind.TRANSLATION_UNIT,
        CursorKind.UNEXPOSED_ATTR,
        CursorKind.IB_ACTION_ATTR,
        CursorKind.IB_OUTLET_ATTR,
        CursorKind.IB_OUTLET_COLLECTION_ATTR,
        CursorKind.CXX_FINAL_ATTR,
        CursorKind.CXX_OVERRIDE_ATTR,
        CursorKind.ANNOTATE_ATTR,
        CursorKind.ASM_LABEL_ATTR,
        CursorKind.PACKED_ATTR,
        CursorKind.PURE_ATTR,
        CursorKind.CONST_ATTR,
        CursorKind.NODUPLICATE_ATTR,
        CursorKind.CUDACONSTANT_ATTR,
        CursorKind.CUDADEVICE_ATTR,
        CursorKind.CUDAGLOBAL_ATTR,
        CursorKind.CUDAHOST_ATTR,
        CursorKind.CUDASHARED_ATTR,
        CursorKind.VISIBILITY_ATTR,
        CursorKind.DLLEXPORT_ATTR,
        CursorKind.DLLIMPORT_ATTR,
        CursorKind.CONVERGENT_ATTR,
        CursorKind.WARN_UNUSED_ATTR,
        CursorKind.WARN_UNUSED_RESULT_ATTR,
        CursorKind.ALIGNED_ATTR,
        CursorKind.PREPROCESSING_DIRECTIVE,
        CursorKind.MACRO_DEFINITION,
        CursorKind.MACRO_INSTANTIATION,
        CursorKind.INCLUSION_DIRECTIVE,
        CursorKind.MODULE_IMPORT_DECL,
        CursorKind.TYPE_ALIAS_TEMPLATE_DECL,
        CursorKind.STATIC_ASSERT,
        CursorKind.FRIEND_DECL,
        CursorKind.CONCEPT_DECL,
        CursorKind.OVERLOAD_CANDIDATE,
    ]
    """
