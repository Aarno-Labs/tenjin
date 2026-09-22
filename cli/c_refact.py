import json
import time
from dataclasses import dataclass
from pathlib import Path
import subprocess
import shutil
from typing import TypedDict
import pprint
from os import environ
import os
import tempfile

from clang.cindex import (  # type: ignore
    Index,
    CursorKind,
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
import cindex_helpers
from cindex_helpers import (
    create_xj_clang_index,
    render_declaration_sans_qualifiers,
    yield_matching_cursors,
)
from constants import XJ_GUIDANCE_FILENAME
import targets
import tenj_types
import pangs_source


def xj_comp_db_from_directory(dir: str) -> compilation_database.CompileCommands:
    return compilation_database.CompileCommands.from_json_file(f"{dir}/compile_commands.json")


def clang_comp_db_from_directory(dir: str) -> CompilationDatabase:
    return CompilationDatabase.fromDirectory(dir)


def parse_translation_unit_with_args(
    index: Index, path: str, args: list[str], in_dir: str | None = None
) -> TranslationUnit:
    """Parse a translation unit with given arguments."""
    args_matching_path = []

    if not args:
        args = [path]

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

    compdb = b.compdb_for_target_within(t.key, target_dir)
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


@dataclass
class ConsolidationRevert:
    """A region in a TU's preprocessed `.i` file that pre_refold_consolidation
    reverted from a "modified" version (introduced by earlier prep passes) back
    to the original expanded header content, after stashing the modification in
    the actual header file.

    The intent is to let refolding fold the region back into an `#include`, so
    the included (modified) header carries the change into the `.c`. When
    refolding fails to produce the include, the un-modified expanded content
    survives in the `.c` and the modification is silently dropped. These
    records let `refold_build` detect that and re-insert the modified text in
    the residual refolded `.c`.

    `expanded_header_version` is the post-preprocessor `.i` form (macros fully
    expanded). `original_header_version` is the pre-preprocessor `.h` form
    (with macros). The refolder partially restores macros even when it can't
    refold to `#include`, so the `.h` form is the better pattern to find in
    the residual `.c`; the `.i` form is a fallback.

    `pre_rewrite_i_start` / `pre_rewrite_i_length` describe the region in the
    `.i` as it stood at consolidation time (before the BatchingRewriter
    applied any rewrites). The post-rewrite location is computed from the
    sibling `ConsolidationRevertContext.all_i_rewrites` list.
    """

    modified_version: str
    expanded_header_version: str
    original_header_version: str
    header_rel_path: str
    quss: str
    is_defn: bool
    pre_rewrite_i_start: int
    pre_rewrite_i_length: int


@dataclass(frozen=True)
class RelocatedIncludeBlock:
    """An XjGlobals include block moved from a preprocessed TU into a header.

    If refolding cannot recover that header include, the block must be restored
    alongside the expanded definition identified by `target_quss`.
    """

    target_quss: str
    contents: str


@dataclass
class ConsolidationRevertContext:
    """Refold fallbacks for one TU, plus its consolidation rewrite locations.

    `all_i_rewrites` is `(pre_start, pre_length, post_length)` per rewrite
    that affects this `.i` file (reverts plus any other consolidation-pass
    edits like the `struct XjGlobals;` forward-decl removal). The triples are
    enough to translate every revert's pre-rewrite location to its
    post-rewrite byte range in the final `.i`, which is the anchor used to
    look up the corresponding `.c` range via the edit-map.

    `relocated_include_blocks` records XjGlobals support text moved from the
    TU into a header. When a reverted definition remains expanded after
    refolding, its support block must remain expanded with it.
    """

    reverts: list[ConsolidationRevert]
    relocated_include_blocks: list[RelocatedIncludeBlock]
    all_i_rewrites: list[tuple[int, int, int]]


def refold_build(
    b: targets.BuildInfo,
    t: targets.BuildTarget,
    target_dir_path: Path,
    consolidation_data_by_rel_tu: dict[str, ConsolidationRevertContext] | None = None,
    refold_map_root: Path | None = None,
) -> None:
    """
    For each TU in compdb, run clang-refold to produce .c files from modified .i files
    """
    target_dir_path.mkdir(parents=True, exist_ok=True)
    compdb = b.compdb_for_target_within(t.key, target_dir_path)
    for cmd in compdb.commands:
        abs_src_path = cmd.absolute_file_path

        assert abs_src_path.suffixes[-2:] == [".nolines", ".i"]
        abs_src_path_base = abs_src_path.with_suffix("")
        c_path = abs_src_path_base.with_suffix(".c")
        if refold_map_root is None:
            refold_map_path = abs_src_path_base.with_suffix(".nolines.refoldmap.json")
        else:
            rel_src_path = abs_src_path.relative_to(target_dir_path)
            refold_map_path = (refold_map_root / rel_src_path).with_suffix(".refoldmap.json")
        edit_map_path = abs_src_path_base.with_suffix(".nolines.editmap.json")

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
                f"--emit-edit-map={edit_map_path.as_posix()}",
            ],
            check=True,
            cwd=cmd.directory,
        )

        if consolidation_data_by_rel_tu:
            rel_tu_path = abs_src_path.relative_to(target_dir_path).as_posix()
            ctx = consolidation_data_by_rel_tu.get(rel_tu_path)
            if ctx is not None and ctx.reverts:
                restore_dropped_consolidation_reverts(c_path, edit_map_path, ctx)

        if environ.get("XJ_REFOLD_CHECK"):
            crc_cp = hermetic.run(
                [
                    "clang-refold",
                    "--check",
                    c_path,
                    "--refold-map",
                    refold_map_path,
                    "--pp-mod",
                    abs_src_path,
                ],
                check=False,
                capture_output=True,
            )
            if crc_cp.returncode != 0:
                print("clang-refold --check failed:")
                print("stdout:")
                print(crc_cp.stdout.decode("utf-8"))
                print("stderr:")
                print(crc_cp.stderr.decode("utf-8"))
                raise RuntimeError("clang-refold --check failed")

    b._use_preprocessed_files = False


@dataclass
class _EditMapEntry:
    i_begin: int
    i_end: int
    c_begin: int
    c_end: int


def _load_edit_map(edit_map_path: Path) -> list[_EditMapEntry]:
    """Parse the JSON sidecar emitted by `clang-refold --emit-edit-map`.

    Each entry pairs a half-open byte range in the modified preprocessed `.i`
    with the half-open byte range in the refolded `.c` that materialized it.
    Entries are sorted by `.i` start offset; ranges may be coalesced.
    """
    if not edit_map_path.exists():
        return []
    try:
        data = json.loads(edit_map_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return []
    entries: list[_EditMapEntry] = []
    for edit in data.get("edits", []):
        try:
            entries.append(
                _EditMapEntry(
                    i_begin=int(edit["modified_pp_byte_range"]["begin"]),
                    i_end=int(edit["modified_pp_byte_range"]["end"]),
                    c_begin=int(edit["refolded_output_byte_range"]["begin"]),
                    c_end=int(edit["refolded_output_byte_range"]["end"]),
                )
            )
        except (KeyError, TypeError, ValueError):
            continue
    entries.sort(key=lambda e: e.i_begin)
    return entries


def _post_rewrite_i_offset(
    pre_rewrite_i_start: int,
    pre_rewrite_i_length: int,
    all_rewrites: list[tuple[int, int, int]],
) -> tuple[int, int] | None:
    """Translate a pre-rewrite [start, start+length) region in a `.i` file to
    its post-rewrite [start, end) byte range.

    `all_rewrites` lists every rewrite the BatchingRewriter applied to this
    file as (pre_rewrite_start, pre_rewrite_length, replacement_length).
    Rewrites must be non-overlapping; the rewrite *for this region* must be
    one of them (length-match), and it determines the post-rewrite length.
    Returns `None` if the region wasn't found among the rewrites — meaning
    the consolidation rewrite never made it into the BatchingRewriter and the
    region's post-rewrite identity is undefined.
    """
    cumulative_delta = 0
    own_replacement_length: int | None = None
    for rw_start, rw_pre_len, rw_post_len in sorted(all_rewrites, key=lambda r: r[0]):
        if rw_start < pre_rewrite_i_start:
            cumulative_delta += rw_post_len - rw_pre_len
            continue
        if rw_start == pre_rewrite_i_start and rw_pre_len == pre_rewrite_i_length:
            own_replacement_length = rw_post_len
            break
        # `rw_start > pre_rewrite_i_start` means we passed our region without
        # finding it; the consolidation revert wasn't applied.
        break
    if own_replacement_length is None:
        return None
    post_start = pre_rewrite_i_start + cumulative_delta
    return (post_start, post_start + own_replacement_length)


def _find_unique_in_range(
    content: str, needle: str, search_begin: int, search_end: int
) -> int | None:
    """Find the unique occurrence of `needle` in `content[search_begin:search_end]`,
    returning its absolute start offset, or None if there are zero or >1 hits.
    """
    if not needle:
        return None
    pos = max(0, search_begin)
    end = min(len(content), search_end) if search_end >= 0 else len(content)
    first: int | None = None
    while pos < end:
        idx = content.find(needle, pos, end)
        if idx == -1:
            break
        if first is not None:
            return None
        first = idx
        pos = idx + 1
    return first


def restore_dropped_consolidation_reverts(
    c_path: Path,
    edit_map_path: Path,
    ctx: ConsolidationRevertContext,
) -> None:
    """For each consolidation revert in this TU, decide whether refolding
    folded the reverted region into an `#include`, and if not, splice the
    modified text into the residual refolded `.c`.

    The decision uses the edit-map sidecar from `clang-refold --emit-edit-map`:

      1. Translate the revert's pre-rewrite `.i` region to its post-rewrite
         byte range using `ctx.all_i_rewrites`.
      2. Find the edit-map entry whose modified-pp byte range contains the
         post-rewrite `.i` region. The entry's refolded-output byte range is
         the corresponding scope in the `.c`. (Edit-map coalescing typically
         swallows the reverted, now-unedited bytes into a surrounding edit's
         range. If no entry covers the region the refolder produced an
         identity-ish mapping outside any edit and the whole `.c` is the
         scope.)
      3. Within that `.c` scope, look for the original `.h` form first (the
         refolder restores macros in expansion-fallback regions), falling
         back to the expanded `.i` form. A unique match means refolding fell
         back to expansion — splice in the modified version. No match means
         refolding produced an `#include` and the modified header carries the
         change.

    Restricting the search to the edit-map's `.c` scope is the principled
    part; a textual match within that scope is still necessary because
    edit-map coalescing collapses sub-edit structure.
    """
    if not c_path.exists():
        return

    c_content = c_path.read_text(encoding="utf-8")
    edit_map = _load_edit_map(edit_map_path)
    include_blocks_by_target: dict[str, list[str]] = {}
    for block in ctx.relocated_include_blocks:
        include_blocks_by_target.setdefault(block.target_quss, []).append(block.contents)

    restored = 0
    restored_include_blocks = 0
    skipped_ambiguous = 0
    skipped_already_folded = 0
    pending_rewrites: list[tuple[int, int, str, str]] = []
    for revert in ctx.reverts:
        if revert.modified_version == revert.expanded_header_version:
            continue
        post = _post_rewrite_i_offset(
            revert.pre_rewrite_i_start,
            revert.pre_rewrite_i_length,
            ctx.all_i_rewrites,
        )
        if post is None:
            skipped_already_folded += 1
            continue
        i_post_start, i_post_end = post

        chosen_entry: _EditMapEntry | None = None
        for entry in edit_map:
            if entry.i_begin <= i_post_start and i_post_end <= entry.i_end:
                chosen_entry = entry
                break
        if chosen_entry is not None:
            c_scope_begin = chosen_entry.c_begin
            c_scope_end = chosen_entry.c_end
        else:
            c_scope_begin = 0
            c_scope_end = len(c_content)

        match_idx: int | None = None
        chosen_candidate: str | None = None
        for candidate in (revert.original_header_version, revert.expanded_header_version):
            if not candidate:
                continue
            idx = _find_unique_in_range(c_content, candidate, c_scope_begin, c_scope_end)
            if idx is not None:
                match_idx = idx
                chosen_candidate = candidate
                break

        if match_idx is None or chosen_candidate is None:
            any_global_hit = any(
                candidate and c_content.count(candidate) > 0
                for candidate in (revert.original_header_version, revert.expanded_header_version)
            )
            if not any_global_hit:
                skipped_already_folded += 1
            else:
                print(
                    f"refold restore: WARNING: ambiguous or out-of-scope match for {revert.quss}"
                    f" in {c_path.name} (edit-map scope=[{c_scope_begin},{c_scope_end}))"
                )
                skipped_ambiguous += 1
            continue

        replacement = revert.modified_version
        if revert.is_defn and revert.quss in include_blocks_by_target:
            blocks = include_blocks_by_target.pop(revert.quss)
            replacement = "".join(blocks) + replacement
            restored_include_blocks += len(blocks)

        pending_rewrites.append((
            match_idx,
            match_idx + len(chosen_candidate),
            replacement,
            revert.quss,
        ))

    pending_rewrites.sort(reverse=True)
    for c_start, c_end, replacement, quss in pending_rewrites:
        c_content = c_content[:c_start] + replacement + c_content[c_end:]
        restored += 1
        print(f"refold restore: restored {quss} in {c_path.name}")

    if restored or skipped_ambiguous:
        print(
            f"refold restore for {c_path.name}: restored={restored} "
            f"restored_include_blocks={restored_include_blocks} "
            f"skipped_ambiguous={skipped_ambiguous} "
            f"already_folded_or_absent={skipped_already_folded} "
            f"(total reverts={len(ctx.reverts)})"
        )
    if restored:
        c_path.write_text(c_content, encoding="utf-8")


@dataclass
class NamedDeclInfo:
    spelling: tenj_types.CIdentifier
    file_path: tenj_types.FilePathStr | None
    decl_start_byte_offset: int
    decl_end_byte_offset: int
    decl_location_byte_offset: int
    start_line: int
    start_col: int
    end_line: int
    end_col: int
    usr: tenj_types.ClangUSR


@dataclass(frozen=True)
class WeakCapableFnDefn:
    """An external-linkage function definition, and whether it is `weak`."""

    name: tenj_types.CIdentifier
    file_path: tenj_types.FilePathStr
    is_weak: bool
    # Byte range of the function body, i.e. the outermost `{...}` compound
    # statement. Replacing it with `;` demotes the definition to a declaration.
    body_start_byte_offset: int
    body_end_byte_offset: int


# Spellings of the GNU `weak` attribute, as reported by libclang. The attribute
# is unexposed, so we recognize it by the single token making up its extent.
_WEAK_ATTR_TOKENS = frozenset(["weak", "__weak__"])


def _cursor_is_weak(cursor: Cursor) -> bool:
    if not cursor.has_attrs():
        return False
    for child in cursor.get_children():
        if not child.kind.is_attribute():
            continue
        tokens = [t.spelling for t in child.get_tokens()]
        if len(tokens) == 1 and tokens[0] in _WEAK_ATTR_TOKENS:
            return True
    return False


def collect_extern_fn_definitions(
    translation_unit: TranslationUnit,
) -> list[WeakCapableFnDefn]:
    """Collect the external-linkage function definitions of a translation unit.

    Static (internal-linkage) functions are excluded because they never produce
    a symbol that can collide with a definition from another translation unit.
    """
    defns: list[WeakCapableFnDefn] = []
    for cursor in translation_unit.cursor.get_children():  # type: ignore[attr-defined]
        if cursor.kind != CursorKind.FUNCTION_DECL:
            continue
        if not cursor.is_definition():
            continue
        if cursor.storage_class == StorageClass.STATIC:
            continue
        if cursor.linkage != LinkageKind.EXTERNAL:
            continue
        body = next(
            (c for c in cursor.get_children() if c.kind == CursorKind.COMPOUND_STMT),
            None,
        )
        if body is None:
            continue
        file = body.extent.start.file
        if file is None:
            continue
        defns.append(
            WeakCapableFnDefn(
                name=cursor.spelling,
                file_path=file.name,
                is_weak=_cursor_is_weak(cursor),
                body_start_byte_offset=body.extent.start.offset,
                body_end_byte_offset=body.extent.end.offset,
            )
        )
    return defns


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


def compute_global_symbol_inventory_for_translation_units(
    translation_units: list[TranslationUnit],
) -> list[Cursor]:
    """Collect declarations which can collide with a project static's name.

    Unlike ``compute_globals_and_statics_for_translation_units``, this includes
    declarations as well as definitions and includes externally linked functions.
    The result is intended as a name-occupancy inventory, not as a list of
    entities eligible for a source rewrite.
    """

    combined: list[Cursor] = []

    def visit(node: Cursor):
        if node.kind in (CursorKind.VAR_DECL, CursorKind.FUNCTION_DECL) and (
            node.storage_class == StorageClass.STATIC or node.linkage == LinkageKind.EXTERNAL
        ):
            combined.append(node)
        for child in node.get_children():
            visit(child)

    for translation_unit in translation_units:
        visit(translation_unit.cursor)  # type: ignore[attr-defined]
    return combined


def mk_NamedDeclInfo(node: Cursor) -> NamedDeclInfo:
    extent = node.extent
    start = extent.start
    end = extent.end
    location = node.location

    file_path = start.file.name if start.file else None
    return NamedDeclInfo(
        spelling=node.spelling,
        file_path=file_path,
        decl_start_byte_offset=start.offset,
        decl_end_byte_offset=end.offset,
        decl_location_byte_offset=location.offset,
        start_line=start.line,
        start_col=start.column,
        end_line=end.line,
        end_col=end.column,
        usr=node.get_usr(),
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
                    node.semantic_parent is not None
                    and node.semantic_parent.kind == CursorKind.TRANSLATION_UNIT
                )
                # Include top-level declarations or entities with 'static' storage
                if (top_level_var_decl and not statics_only) or sc == StorageClass.STATIC:
                    grab(node)
        for child in node.get_children():
            visit(child)

    visit(translation_unit.cursor)  # type:ignore[attr-defined]
    return results


def global_definition_blank_rewrite(
    content: bytes, start_offset: int, extent_end_offset: int
) -> tuple[int, int, str]:
    """Return a width- and line-preserving rewrite for one standalone definition."""
    if content[extent_end_offset - 1 : extent_end_offset] == b";":
        end_offset = extent_end_offset
    else:
        semicolon_offset = content.find(b";", extent_end_offset)
        if semicolon_offset == -1:
            raise ValueError("Could not find the semicolon terminating a localized global")
        between = content[extent_end_offset:semicolon_offset]
        if between.strip():
            raise ValueError(
                "Localized global is not a standalone declaration; "
                "split joined declarations before localization"
            )
        end_offset = semicolon_offset + 1

    original = content[start_offset:end_offset]
    replacement = "".join(
        "\n" if byte == ord("\n") else "\r" if byte == ord("\r") else " " for byte in original
    )
    return start_offset, end_offset - start_offset, replacement


XJG_PLACEHOLDER = "((struct XjGlobals*)0)"


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


class XjHoistEmbeddedTagDefsLoc(TypedDict):
    f: str
    b: int
    e: int


class XjHoistEmbeddedTagDefsRefEdit(TypedDict):
    r: XjHoistEmbeddedTagDefsLoc | None
    text: str


class XjHoistEmbeddedTagDefsEdit(TypedDict):
    insert: XjHoistEmbeddedTagDefsLoc | None
    insert_text: str
    replace: XjHoistEmbeddedTagDefsLoc | None
    replace_text: str
    refs: list[XjHoistEmbeddedTagDefsRefEdit]


class XjHoistEmbeddedTagDefsOutput(TypedDict):
    edits: list[XjHoistEmbeddedTagDefsEdit]


def run_xj_hoist_embedded_tag_defs(
    current_codebase: Path,
    build_info: targets.BuildInfo,
) -> XjHoistEmbeddedTagDefsOutput:
    builddir = hermetic.xj_prepare_locatejoineddecls_build_dir(repo_root.localdir())
    assert builddir.exists(), (
        f"Build directory {builddir} does not exist, should have been built already"
    )

    xj_clang_resource_dir = (
        hermetic.run("clang -print-resource-dir", shell=True, capture_output=True)
        .stdout.decode("utf-8")
        .strip()
    )

    build_info.compdb_for_all_targets_within(current_codebase).to_json_file(
        current_codebase / "compile_commands.json"
    )

    binary_path = builddir / "xj-hoist-embedded-tag-defs"
    xj_find_start = time.time()
    cp = hermetic.run(
        [
            binary_path.as_posix(),
            "--extra-arg=-Wno-zero-length-array",
            "--extra-arg=-Wno-implicit-int-conversion",
            "--extra-arg=-Wno-unused-function",
            "--executor=all-TUs",
            "--execute-concurrency=1",
            "--json-output-path=xj-hoist-embedded-tag-defs.json",
            f"--extra-arg=-resource-dir={xj_clang_resource_dir}",
            (current_codebase / "compile_commands.json").as_posix(),
        ],
        cwd=current_codebase,
        check=True,
        capture_output=True,
    )
    xj_find_elapsed = time.time() - xj_find_start
    print(f"xj-hoist-embedded-tag-defs completed in {xj_find_elapsed:.1f} seconds")

    print("xj-hoist-embedded-tag-defs stderr:")
    print("==========================")
    print(cp.stderr.decode("utf-8"))
    print("==========================")

    print("xj-hoist-embedded-tag-defs stdout:")
    print("==========================")
    print(cp.stdout.decode("utf-8"))
    print("==========================")
    try:
        raw: XjHoistEmbeddedTagDefsOutput = json.load(
            (current_codebase / "xj-hoist-embedded-tag-defs.json").open()
        )
    except:
        print("Failed to parse xj-hoist-embedded-tag-defs output as JSON:")
        print(cp.stdout.decode("utf-8"))
        raise

    return raw


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


def cursor_extent_contains(outer: Cursor, inner: Cursor) -> bool:
    outer_file = outer.location.file
    inner_file = inner.location.file
    if outer_file is None or inner_file is None:
        return False
    return (
        outer_file.name == inner_file.name
        and outer.extent.start.offset <= inner.extent.start.offset
        and inner.extent.end.offset <= outer.extent.end.offset
    )


def type_names_declared_before_offset(tu: TranslationUnit, offset: int) -> set[str]:
    """Return named type declarations that are visible before ``offset``.

    The translation units processed by mutable-global localization are flattened
    ``.i`` files, so source offsets describe declaration order within the file.
    A declaration later in the translation unit is not yet in scope at an
    earlier insertion point.
    """
    type_declaration_kinds = {
        CursorKind.STRUCT_DECL,
        CursorKind.UNION_DECL,
        CursorKind.ENUM_DECL,
        CursorKind.TYPEDEF_DECL,
    }
    return {
        cursor.spelling
        for cursor in tu.cursor.walk_preorder()  # type: ignore[attr-defined]
        if cursor.kind in type_declaration_kinds
        and cursor.spelling
        and cursor.semantic_parent.kind == CursorKind.TRANSLATION_UNIT
        and cursor.extent.end.offset <= offset
    }


def order_context_type_declarations(declarations: list[Cursor]) -> list[Cursor]:
    """Order copied type declarations so their dependencies are declared first."""
    tag_kinds = (CursorKind.STRUCT_DECL, CursorKind.UNION_DECL, CursorKind.ENUM_DECL)
    owners = {}
    for declaration in declarations:
        for child in declaration.walk_preorder():
            if child.kind in (*tag_kinds, CursorKind.TYPEDEF_DECL):
                owners[child.kind, child.spelling] = declaration
    result: list[Cursor] = []
    visited = set()

    def visit(declaration):
        key = (declaration.kind, declaration.spelling)
        if key in visited:
            return
        visited.add(key)

        def dependencies(cursor):
            for child in cursor.get_children():
                if child.kind == CursorKind.TYPE_REF and child.referenced:
                    referenced = child.referenced
                    if (
                        referenced.kind in tag_kinds
                        and cursor.type.get_canonical().kind == TypeKind.POINTER
                    ):
                        # Tag forward declarations suffice for pointer fields.
                        continue
                    owner = owners.get((referenced.kind, referenced.spelling))
                    if owner is not None:
                        visit(owner)
                dependencies(child)

        dependencies(declaration)
        result.append(declaration)

    for declaration in sorted(
        declarations, key=lambda c: (c.location.file.name, c.extent.start.offset)
    ):
        visit(declaration)
    return result


def localize_mutable_globals(
    manifest_path: Path,
    compdb: compilation_database.CompileCommands,
    current_codebase: Path,
):
    """Materialize on a private copy, publishing only after both C validators pass."""
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    selected = manifest.get("context_rewrite", {}).get("selected")
    if selected is not None and not selected["fields"]:
        return
    pangs_source.validate_plan(manifest, current_codebase)
    with tempfile.TemporaryDirectory(
        prefix="pangs-materialize-", dir=current_codebase.parent
    ) as temp:
        attempt_manifest = Path(temp) / "manifest.json"
        attempt_manifest.write_text(json.dumps(manifest), encoding="utf-8")
        staged = Path(temp) / "stage"
        shutil.copytree(current_codebase, staged, symlinks=True)
        staged_compdb = pangs_source.relocate_compdb(compdb, current_codebase, staged)
        try:
            _localize_mutable_globals_in_place(attempt_manifest, staged_compdb, staged)
            pangs_source.validate_c(staged_compdb)
            pangs_source.validate_cross_tu(
                manifest, Path(manifest["run"]["analysis"]["repo_root"]), staged
            )
        except Exception as exc:
            raise pangs_source.ContractViolation(
                f"PANGS source-plan contract violated during materialization: {exc}"
            ) from exc
        for path in staged_compdb.get_source_files():
            if XJG_PLACEHOLDER.encode() in path.read_bytes():
                raise pangs_source.ContractViolation(
                    f"PANGS source-plan contract violated: unmaterialized placeholder in {path}"
                )
        compdb.to_json_file(staged / "compile_commands.json")
        manifest["materialization"] = {
            "tool": {"name": "tenjin", "version": "source-plan-2"},
            "marker_inventory": [],
            "demotions": [],
            "source_plan_version": 2,
            "c_validation": ["clang-14", "clang-21"],
            "applied_source_edits": len(selected["source_edits"]),
        }
        materialized_manifest = staged / "pangs-disposition" / "pangs-manifest.json"
        materialized_manifest.parent.mkdir(parents=True, exist_ok=True)
        materialized_manifest.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        original = Path(temp) / "original"
        current_codebase.rename(original)
        try:
            staged.rename(current_codebase)
        except BaseException:
            original.rename(current_codebase)
            raise


def _localize_mutable_globals_in_place(
    manifest_path: Path,
    compdb: compilation_database.CompileCommands,
    current_codebase: Path,
):
    manifest: dict = json.load(manifest_path.open("r"))
    if manifest.get("schema_version") != 8:
        raise ValueError("Tenjin requires PANGS disposition manifest schema version 8")
    context_rewrite = manifest.get("context_rewrite")
    if not isinstance(context_rewrite, dict) or "selected" not in context_rewrite:
        raise ValueError("PANGS manifest has no finalized context-rewrite selection")
    selected = context_rewrite["selected"]

    # To localize mutable globals, we perform the following steps:
    # 1. Use only the fields selected for localization by PANGS disposition policy.
    # 2a. Find the definitions of each selected global.
    # 2b. Construct the transitive closure of all directly used struct/union/enum/typedef
    #     definitions needed to define those globals. Note that fields which use
    #     a struct behind a pointer do not require inclusion of that struct definition,
    #     as the type can be forward-declared. (Such type names should be collected in a separate set.)
    # 3. Construct a `struct XjGlobals` declaration containing all mutable globals
    #    adn the struct/union definitions from step 2b.
    #    Place the declaration in a new header file `xj_globals.h`.
    # 4. Based on the definitions from step 2, initialize a singleton XjGlobals instance
    #    in `main()`, called `xjgv`, and a pointer to it called `xjg`.
    # 5. For each function in the selected context rewrite, except for `main`,
    #    pass a pointer to the XjGlobals instance, called `xjg`, as an additional first parameter.
    # 6. For each call to a selected context function, pass along the `xjg` parameter.
    # 8. Each syntatic use of a liftable mutable global named WHATEVER is replaced
    #    with `xjg->WHATEVER`.
    # 9. In each file that uses mutable globals, add `#include "xj_globals.h"`
    #    before the first function definition which uses a mutable global.
    #
    # Use the `BatchingRewriter` to perform all of these rewrites in a single pass.

    if not selected["fields"]:
        print("No globals selected for localization; skipping localization.")
        return

    compdb.to_json_file(current_codebase / "compile_commands.json")

    nonmain_context_functions: set[str] = set(selected["functions"])
    nonmain_context_functions.discard("main")  # Don't modify main
    localized_global_names = {demangle_meg(f["llvm_name"]) for f in selected["fields"]}
    globals_without_initializers = set(context_rewrite["source"]["globals_without_initializers"])

    print("Applying PANGS source edits...")
    time_start = time.time()
    pangs_source.apply_source_edits(manifest, current_codebase)
    time_elapsed = time.time() - time_start
    print(f"... PANGS source edits applied, elapsed: {time_elapsed:.1f}")
    pangs_source.validate_c(compdb)

    index = create_xj_clang_index()
    tus = parse_project(index, compdb)

    globals_and_statics = compute_globals_and_statics_for_translation_units(
        list(tus.values()), elide_functions=True
    )
    localized_globals_and_statics = [
        c for c in globals_and_statics if c.spelling in localized_global_names
    ]

    if not localized_globals_and_statics:
        if localized_global_names:
            raise ValueError(
                "PANGS selected globals absent from the source tree: "
                + ", ".join(sorted(localized_global_names))
            )
        print("No globals selected for localization; skipping further localization steps.")
        return

    localized_global_cursors_by_name = {c.spelling: c for c in localized_globals_and_statics}

    assert len(localized_global_cursors_by_name) == len(localized_globals_and_statics), (
        "Expected all localized global names to be unique, "
        + f"but got duplicates within: {localized_global_cursors_by_name.keys()}"
    )
    missing_globals = localized_global_names - localized_global_cursors_by_name.keys()
    if missing_globals:
        raise ValueError(
            "PANGS selected globals absent from the source tree: "
            + ", ".join(sorted(missing_globals))
        )

    groups: dict[tuple[str, int], set[str]] = {}
    for cursor in globals_and_statics:
        groups.setdefault((cursor.location.file.name, cursor.extent.start.offset), set()).add(
            cursor.spelling
        )
    for names in groups.values():
        affected = names & localized_global_names
        if len(names) > 1 and affected:
            raise pangs_source.ContractViolation(
                "PANGS selected an unsupported joined global declaration: "
                + ", ".join(sorted(affected))
            )

    # Step 2b: Construct transitive closure of struct/union definitions
    print("\n" + "=" * 80)
    print("STEP 2b: Finding struct/union dependencies")
    print("=" * 80)

    needed_struct_defs = {}
    needed_typedefs: dict[str, tuple[Cursor, str]] = {}
    forward_declarable_types: dict[str, str] = {}

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
            needed_typedefs.setdefault(
                typedef_decl.spelling,
                (typedef_decl, typedef_decl.underlying_typedef_type.get_canonical().spelling),
            )
            # print(f"{indent}  Saw typedef...")
            # print(f"{indent}    typedef cursor: {typedef_decl.kind}")
            # print(f"{indent}    typedef cursor: {typedef_decl.extent}")
            # print(f"{indent}    typedef type: {typedef_decl.type}")
            # print(f"{indent}    typedef type: {typedef_decl.type.kind}")
            # print(f"{indent}    underlying type: {typedef_decl.underlying_typedef_type.kind}")
            # print(f"{indent}    referenced type: {typedef_decl.get_definition().referenced.kind}")
            collect_type_dependencies(typedef_decl.underlying_typedef_type, depth + 1)
            return

        if type_obj_canonical.kind == TypeKind.POINTER:
            while type_obj_noncanonical.kind == TypeKind.TYPEDEF:
                type_obj_noncanonical = (
                    type_obj_noncanonical.get_declaration().underlying_typedef_type
                )
            assert type_obj_noncanonical.kind == TypeKind.POINTER

        # If it's a pointer, the pointee can be forward-declared
        if type_obj_noncanonical.kind == TypeKind.POINTER:
            pointee = type_obj_noncanonical.get_pointee()
            pointee_canonical = pointee.get_canonical()
            # print(f"{indent}  Pointer to: {pointee.spelling}")

            # Check if pointee is a struct/union
            decl = pointee_canonical.get_declaration()
            if decl.kind in [CursorKind.STRUCT_DECL, CursorKind.UNION_DECL]:
                forward_declarable_types[decl.spelling] = (
                    "union" if decl.kind == CursorKind.UNION_DECL else "struct"
                )
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
        decl = decl.get_definition() or decl

        if decl.kind in [CursorKind.STRUCT_DECL, CursorKind.UNION_DECL, CursorKind.ENUM_DECL]:
            type_name = decl.spelling
            if type_name and type_name not in needed_struct_defs:
                # print(f"{indent}  -> Need full definition: {type_name}")
                needed_struct_defs[type_name] = decl

                # Recursively process fields
                for field in decl.get_children():
                    if field.kind == CursorKind.FIELD_DECL:
                        # print(f"{indent}    Field: {field.spelling} : {field.type.spelling}")
                        collect_type_dependencies(field.type, depth + 2)

    for cursor in localized_globals_and_statics:
        # print(f"\nAnalyzing dependencies for {cursor.spelling}:")
        collect_type_dependencies(cursor.type, depth=1)

    initializer_functions: dict[str, Cursor] = {}
    for var_cursor in localized_globals_and_statics:
        for child in var_cursor.walk_preorder():
            if child.kind == CursorKind.DECL_REF_EXPR:
                referenced = child.referenced
                if referenced and referenced.kind == CursorKind.FUNCTION_DECL:
                    initializer_functions[referenced.get_usr()] = referenced
                    collect_type_dependencies(referenced.type)

    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"\nFound {len(localized_globals_and_statics)} localized global definitions:")

    for cursor in localized_globals_and_statics:
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

    print(f"\nContext functions to modify: {nonmain_context_functions}")

    with batching_rewriter.BatchingRewriter() as rewriter:
        global_definition_rewrites: list[tuple[str, int, int, str]] = []
        global_definition_ranges: dict[str, list[tuple[int, int]]] = {}
        for var_cursor in localized_globals_and_statics:
            definition_path = var_cursor.location.file.name  # type: ignore[union-attr]
            rewrite = global_definition_blank_rewrite(
                rewriter.get_content(definition_path),
                var_cursor.extent.start.offset,
                var_cursor.extent.end.offset,
            )
            start_offset, length, replacement = rewrite
            global_definition_rewrites.append((definition_path, start_offset, length, replacement))
            global_definition_ranges.setdefault(definition_path, []).append((
                start_offset,
                start_offset + length,
            ))

        # TU -> offset of first fn using mutable globals
        lowest_mutable_accessing_fn_starts: dict[str, tuple[int, str]] = {}
        initializer_prototypes: dict[str, list[str]] = {}

        def record_mutable_accessing_fn_start(tu_path: str, fn_start: tuple[int, str] | None):
            if fn_start is None:
                return
            if tu_path not in lowest_mutable_accessing_fn_starts:
                lowest_mutable_accessing_fn_starts[tu_path] = fn_start
            else:
                current_lowest = lowest_mutable_accessing_fn_starts[tu_path]
                if fn_start[0] < current_lowest[0]:
                    lowest_mutable_accessing_fn_starts[tu_path] = fn_start

        # Step 8: Replace uses of mutable globals with xjg->WHATEVER
        print("\n  --- Step 8: Replacing global variable accesses ---")
        for tu_path, tu in tus.items():
            current_fn_start = None
            for child in tu.cursor.walk_preorder():  # type: ignore[attr-defined]
                if child.kind == CursorKind.FUNCTION_DECL:
                    if child.is_definition():
                        q = cindex_helpers.quss(child, None)
                        current_fn_start = (child.extent.start.offset, q)

                if (
                    child.kind == CursorKind.DECL_REF_EXPR
                    and child.spelling in localized_global_cursors_by_name
                ):
                    # Get the extent of the variable reference
                    start_offset = child.extent.start.offset
                    end_offset = child.extent.end.offset
                    length = end_offset - start_offset
                    var_name = child.spelling

                    if any(
                        definition_start <= start_offset < definition_end
                        for definition_start, definition_end in global_definition_ranges.get(
                            tu_path, []
                        )
                    ):
                        # Initializer text is copied into main below. Do not schedule an inner
                        # access rewrite that would overlap the definition-blanking rewrite.
                        continue

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
                    record_mutable_accessing_fn_start(tu_path, current_fn_start)

        # Step 3: Create xj_globals.h header file
        print("\n  --- Step 3: Creating xj_globals.h ---")

        # Create xj_globals.h with full definitions
        # This is a generated project header, so it belongs at the root of the
        # prepared codebase. A compilation database's sources can live in
        # different subdirectories (and `get_source_files()` is unordered).
        header_path = current_codebase / "xj_globals.h"
        print(f"  Creating full definition header at {header_path}")

        # Build the header content
        header_lines = []
        header_lines.append("#ifndef XJ_GLOBALS_H")
        header_lines.append("#define XJ_GLOBALS_H")
        header_lines.append("")

        # Add forward declarations if needed
        if forward_declarable_types:
            for type_name in sorted(forward_declarable_types):
                header_lines.append(f"{forward_declarable_types[type_name]} {type_name};")
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
        header_lines.append("struct XjGlobals {")
        for global_name in sorted(localized_global_cursors_by_name.keys()):
            var_cursor = localized_global_cursors_by_name[global_name]
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

        for var_cursor in localized_globals_and_statics:
            dependencies = set()

            # Walk through the initializer expression to find DECL_REF_EXPR nodes
            for child in var_cursor.walk_preorder():
                if (
                    child.kind == CursorKind.DECL_REF_EXPR
                    and child.spelling not in localized_global_names
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

        for global_name in localized_global_names:
            collect_transitive_deps(global_name, set())

        print(f"\n  Globals to copy into main before xjgv: {globals_to_copy_to_main}")

        # Find main() function and insert initialization at the beginning
        for tu_path, tu in tus.items():
            for cursor in tu.cursor.walk_preorder():  # type: ignore[attr-defined]
                if (
                    cursor.kind == CursorKind.FUNCTION_DECL
                    and cursor.spelling == "main"
                    and cursor.is_definition()
                ):
                    print(f"  Found main() at {tu_path}:{cursor.location.line}")

                    # `main()` may not access mutable globals directly, but it needs to see the full
                    # declaration of the XjGlobals struct because it needs to construct the singleton.
                    record_mutable_accessing_fn_start(tu_path, (cursor.extent.start.offset, "main"))

                    visible_functions = {
                        decl.get_usr()
                        for decl in tu.cursor.get_children()
                        if decl.kind == CursorKind.FUNCTION_DECL
                        and decl.extent.start.offset < cursor.extent.start.offset
                    }
                    for usr, function in sorted(initializer_functions.items()):
                        if usr in visible_functions:
                            continue
                        if (
                            function.linkage == LinkageKind.INTERNAL
                            and function.location.file.name != tu_path
                        ):
                            raise pangs_source.ContractViolation(
                                f"Cannot move initializer referencing private function "
                                f"{function.spelling} from {function.location.file.name} into {tu_path}"
                            )
                        # Keep the rewritten signature's typedefs, qualifiers and attributes.
                        body = next(
                            (
                                c
                                for c in function.get_children()
                                if c.kind == CursorKind.COMPOUND_STMT
                            ),
                            None,
                        )
                        end = body.extent.start.offset if body else function.extent.end.offset
                        declaration = (
                            rewriter.get_content(function.location.file.name)[
                                function.extent.start.offset : end
                            ]
                            .decode("utf-8")
                            .strip()
                        )
                        initializer_prototypes.setdefault(tu_path, []).append(declaration + ";")

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
                                                var_cursor.location.file.name  # type:ignore[attr-defined]
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
                            for global_name in sorted(localized_global_cursors_by_name.keys()):
                                var_cursor = localized_global_cursors_by_name[global_name]
                                content = rewriter.get_content(var_cursor.location.file.name)  # type:ignore[attr-defined]
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

                                if global_name in globals_without_initializers:
                                    initializer = "{0}"
                                    try:
                                        if var_cursor.type.get_canonical().spelling.startswith(
                                            "_Atomic("
                                        ):
                                            # Clang considers it an error to put braces on atomic initializers!
                                            initializer = "0"
                                    except:  # noqa: E722
                                        pass
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

        # Step 7: erase the original definitions without changing source coordinates.
        print("\n  --- Step 7: Blanking original global definitions ---")
        for definition_path, start_offset, length, replacement in global_definition_rewrites:
            rewriter.add_rewrite(definition_path, start_offset, length, replacement)

        # Step 9: Add includes and type definitions to files that use mutable globals
        print("\n  --- Step 9: Adding includes and type definitions ---")

        # The planned edits inserted forward declarations; add the header where needed.
        # (not much point in replacing the forward declarations).
        for tu_path, (offset, q) in lowest_mutable_accessing_fn_starts.items():
            tu = tus[tu_path]

            # print(f"\n  Analyzing types in scope in TU: {tu_path}")

            # Use this TU's own definitions when available, and distinguish a
            # visible complete definition from a forward declaration or a late one.
            local_type_definitions = {}
            for cursor in tu.cursor.walk_preorder():  # type:ignore[attr-defined]
                if (
                    cursor.kind
                    in (
                        CursorKind.STRUCT_DECL,
                        CursorKind.UNION_DECL,
                        CursorKind.ENUM_DECL,
                        CursorKind.TYPEDEF_DECL,
                    )
                    and cursor.spelling
                    and cursor.semantic_parent.kind == CursorKind.TRANSLATION_UNIT
                    and (cursor.kind == CursorKind.TYPEDEF_DECL or cursor.is_definition())
                ):
                    local_type_definitions[cursor.kind, cursor.spelling] = cursor

            types_declared_before_include = type_names_declared_before_offset(tu, offset)

            # Determine which types need to be emitted
            types_to_emit_structs: dict[str, Cursor] = {}  # name -> decl_cursor
            types_to_emit_typedefs: dict[str, Cursor] = {}  # name -> decl_cursor

            for type_name, decl_cursor in needed_struct_defs.items():
                decl_cursor = local_type_definitions.get((decl_cursor.kind, type_name), decl_cursor)
                definition_is_removed = any(
                    start <= decl_cursor.extent.start.offset
                    and decl_cursor.extent.end.offset <= end
                    for start, end in global_definition_ranges.get(tu_path, [])
                )
                if (
                    decl_cursor.location.file.name != tu_path
                    or decl_cursor.extent.end.offset > offset
                    or definition_is_removed
                ):
                    types_to_emit_structs[type_name] = decl_cursor
                #     print(f"    Will emit struct definition: {type_name}")
                # else:
                #     print(f"    Skipping struct (already in scope): {type_name}")

            for type_name, decl_cursor in needed_typedefs.items():
                declaration = local_type_definitions.get(
                    (CursorKind.TYPEDEF_DECL, type_name), decl_cursor[0]
                )
                if (
                    declaration.location.file.name != tu_path
                    or declaration.extent.end.offset > offset
                ):
                    types_to_emit_typedefs[type_name] = declaration
                #     print(f"    Will emit typedef: {type_name}")
                # else:
                #     print(f"    Skipping typedef (already in scope): {type_name}")

            # Avoid emitting duplicate struct/union definitions for things
            # appearing within typedefs.
            typedef_cursors_to_emit = tuple(types_to_emit_typedefs.values())
            types_to_emit_structs = {
                type_name: decl_cursor
                for type_name, decl_cursor in types_to_emit_structs.items()
                if not any(
                    cursor_extent_contains(typedef_cursor, decl_cursor)
                    for typedef_cursor in typedef_cursors_to_emit
                )
            }

            # Build type definitions to insert before main()
            type_defs_lines: list[str] = []
            type_defs_lines.append("\n// Type definitions needed for XjGlobals")

            # Add forward declarations if needed
            forward_decls_to_emit = forward_declarable_types.keys() - types_declared_before_include
            if forward_decls_to_emit:
                for type_name in sorted(forward_decls_to_emit):
                    type_defs_lines.append(f"{forward_declarable_types[type_name]} {type_name};")

            # Preserve declaration order between tags and typedefs. Late local
            # definitions are moved, not duplicated; leave any surrounding
            # variable declarator intact when moving an embedded tag.
            declarations = order_context_type_declarations(
                [*types_to_emit_typedefs.values(), *types_to_emit_structs.values()],
            )
            for decl_cursor in declarations:
                start_offset = decl_cursor.extent.start.offset
                end_offset = decl_cursor.extent.end.offset
                source_path = decl_cursor.location.file.name
                content = rewriter.get_content(source_path)
                type_defs_lines.append(content[start_offset:end_offset].decode("utf-8") + ";")
                if source_path == tu_path and not any(
                    start <= start_offset and end_offset <= end
                    for start, end in global_definition_ranges.get(tu_path, [])
                ):
                    if decl_cursor.kind == CursorKind.TYPEDEF_DECL or content[
                        end_offset:
                    ].lstrip().startswith(b";"):
                        replacement = ""
                    else:
                        tag = {
                            CursorKind.STRUCT_DECL: "struct",
                            CursorKind.UNION_DECL: "union",
                            CursorKind.ENUM_DECL: "enum",
                        }[decl_cursor.kind]
                        replacement = f"{tag} {decl_cursor.spelling}"
                    rewriter.add_rewrite(
                        tu_path, start_offset, end_offset - start_offset, replacement
                    )

            # These are preprocessed `.i` files, for which Clang does not
            # honor `-I` flags. Use a path relative to this translation unit
            # so a source under (for example) `demo/` can still include the
            # generated header at the prepared-codebase root.
            header_include = Path(
                os.path.relpath(header_path, start=Path(tu_path).parent)
            ).as_posix()
            type_defs_lines.append(f'#include "{header_include}"')
            type_defs_lines.extend(initializer_prototypes.get(tu_path, []))
            type_defs_lines.append(f"/* @{q} end include block for XjGlobals */")

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

    update_vars_of_type_guidance_for_xjg(current_codebase, nonmain_context_functions, tus)

    print("=" * 80)


# Update the guidance to have the xjg parameter passed as &mut when possible.
# Functions which are used in higher-order ways must remain as raw pointers.
def update_vars_of_type_guidance_for_xjg(
    current_codebase: Path,
    nonmain_context_functions: set[str],
    tus: dict[str, TranslationUnit],
):
    higher_order_context_functions = set()
    for _tu_path, tu in tus.items():
        assert tu.cursor is not None, f"Translation unit {tu.spelling} has no cursor!"
        for v, ancestors in yield_matching_cursors(tu.cursor, [CursorKind.DECL_REF_EXPR]):
            if v.spelling in nonmain_context_functions:
                # Found a use of a context function; was it in a call position?
                parent = v
                while ancestors:
                    parent, rest = ancestors
                    if rest is None:
                        break
                    if not parent.kind.is_unexposed():
                        break
                    ancestors = rest
                if parent.kind != CursorKind.CALL_EXPR:
                    higher_order_context_functions.add(v.spelling)
                else:
                    # might be a callee, or a call arg
                    callee = next(parent.get_children(), None)
                    if callee and callee.spelling == v.spelling:
                        # direct call, all good
                        pass
                    else:
                        # passed as an argument
                        higher_order_context_functions.add(v.spelling)
    guidance: dict = json.load(open(current_codebase / XJ_GUIDANCE_FILENAME, "r", encoding="utf-8"))
    can_take_mut_xjg = nonmain_context_functions - higher_order_context_functions
    mut_specs = guidance.get("vars_of_type", {}).get("&mut XjGlobals", [])
    for context_fn_name in can_take_mut_xjg:
        mut_specs.append(f"{context_fn_name}:xjg")
    guidance.setdefault("vars_of_type", {})["&mut XjGlobals"] = mut_specs
    with open(current_codebase / XJ_GUIDANCE_FILENAME, "w", encoding="utf-8") as fh:
        json.dump(guidance, fh, indent=2)
