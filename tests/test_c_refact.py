import json
from pathlib import Path
import re

import pytest
from clang.cindex import CursorKind  # type: ignore

import c_refact
import c_refact_decl_splitter
import c_refact_tag_hoister
import compilation_database
import targets
from cindex_helpers import create_xj_clang_index


def test_decl_splitter_skips_embedded_tag_definition_prefix(tmp_codebase):
    tmp_codebase.mkdir()
    source = (
        "struct histindex {\n"
        "\tstruct record {\n"
        "\t\tunsigned int ptr, cnt;\n"
        "\t\tstruct record *next;\n"
        "\t} **records, /* an occurrence */\n"
        "\t        **line_map;\n"
        "};\n"
    )
    sample_c = tmp_codebase / "sample.c"
    sample_c.write_text(source, encoding="utf-8")

    start = source.index("struct record")
    end = source.index(";\n", start)
    c_refact_decl_splitter.apply_decl_splitting_rewrites(
        tmp_codebase,
        {
            "edits": [
                {
                    "r": {"f": sample_c.as_posix(), "b": start, "e": end},
                    "cat": "field",
                    "prefix": (
                        "struct record {\n"
                        "\t\tunsigned int ptr, cnt;\n"
                        "\t\tstruct record *next;\n"
                        "\t} "
                    ),
                    "declarators": ["**records", " /* an occurrence */\n\t        **line_map"],
                }
            ]
        },
    )

    assert sample_c.read_text(encoding="utf-8") == source


def write_compile_commands_for_sources(codebase: Path, sources: list[Path]) -> None:
    commands: list[compilation_database.CompileCommand] = []
    for source in sources:
        commands.extend(
            compilation_database.synthetic_compile_commands_for_c_file(source, codebase).commands
        )
    compilation_database.CompileCommands(commands).to_json_file(codebase / "compile_commands.json")


def build_info_for_single_source(codebase: Path, source: Path) -> targets.BuildInfo:
    build_info = targets.BuildInfo()
    build_info.for_single_file(
        source,
        codebase,
        targets.BuildTarget(
            key=source.with_suffix(".o").name,
            type=targets.TargetType.OBJECT,
            stem_not_unique=source.stem,
        ),
    )
    return build_info


def test_localize_mutable_globals_empty_selection_is_a_noop(tmp_codebase):
    tmp_codebase.mkdir()
    manifest_path = tmp_codebase / "pangs-manifest.json"
    manifest_path.write_text(
        json.dumps({"context_rewrite": {"selected": {"fields": []}}}), encoding="utf-8"
    )

    class UnusedCompdb:
        def to_json_file(self, _path):
            raise AssertionError("an empty selection must not enter the rewrite pipeline")

    c_refact.localize_mutable_globals(manifest_path, UnusedCompdb(), tmp_codebase)


def test_global_definition_blank_rewrite_preserves_width_and_lines():
    source = b"static int global =\n    42;\nint next;\n"
    extent_end = source.index(b"42") + len(b"42")

    offset, length, replacement = c_refact.global_definition_blank_rewrite(source, 0, extent_end)
    rewritten = source[:offset] + replacement.encode() + source[offset + length :]

    assert len(rewritten) == len(source)
    assert rewritten.count(b"\n") == source.count(b"\n")
    assert rewritten.endswith(b"\nint next;\n")
    assert rewritten[: rewritten.index(b"int next;")].strip() == b""


def test_global_definition_blank_rewrite_rejects_joined_declaration():
    source = b"int first = 1, second = 2;\n"
    extent_end = source.index(b"1") + 1

    with pytest.raises(ValueError, match="not a standalone declaration"):
        c_refact.global_definition_blank_rewrite(source, 0, extent_end)


def test_cursor_extent_contains_typedef_embedded_struct_definition(tmp_codebase):
    tmp_codebase.mkdir()
    sample_c = tmp_codebase / "sample.c"
    sample_c.write_text(
        "typedef struct Payload { int value; } PayloadAlias;\n"
        "struct Standalone { int value; };\n"
        "PayloadAlias payload;\n"
        "struct Standalone standalone;\n",
        encoding="utf-8",
    )
    write_compile_commands_for_sources(tmp_codebase, [sample_c])

    compdb = compilation_database.CompileCommands.from_json_file(
        tmp_codebase / "compile_commands.json"
    )
    tus = c_refact.parse_project(create_xj_clang_index(), compdb)
    cursors = list(next(iter(tus.values())).cursor.walk_preorder())

    typedef_cursor = next(
        cursor
        for cursor in cursors
        if cursor.kind == CursorKind.TYPEDEF_DECL and cursor.spelling == "PayloadAlias"
    )
    payload_cursor = next(
        cursor
        for cursor in cursors
        if cursor.kind == CursorKind.STRUCT_DECL
        and cursor.spelling == "Payload"
        and cursor.is_definition()
    )
    standalone_cursor = next(
        cursor
        for cursor in cursors
        if cursor.kind == CursorKind.STRUCT_DECL
        and cursor.spelling == "Standalone"
        and cursor.is_definition()
    )

    assert c_refact.cursor_extent_contains(typedef_cursor, payload_cursor)
    assert not c_refact.cursor_extent_contains(typedef_cursor, standalone_cursor)


def test_type_names_declared_before_offset_excludes_later_atomic_typedef(tmp_codebase):
    tmp_codebase.mkdir()
    sample_c = tmp_codebase / "sample.c"
    sample_c.write_text(
        "typedef int EarlyAlias;\n"
        "struct EarlyTag { int value; };\n"
        "int first_user(void) { return 0; }\n"
        "typedef _Atomic(int) __tenjin_atomic_i32_t;\n"
        "struct LateTag { int value; };\n",
        encoding="utf-8",
    )
    write_compile_commands_for_sources(tmp_codebase, [sample_c])

    compdb = compilation_database.CompileCommands.from_json_file(
        tmp_codebase / "compile_commands.json"
    )
    tus = c_refact.parse_project(create_xj_clang_index(), compdb)
    tu = next(iter(tus.values()))
    first_user = next(
        cursor
        for cursor in tu.cursor.get_children()
        if cursor.kind == CursorKind.FUNCTION_DECL and cursor.spelling == "first_user"
    )

    names = c_refact.type_names_declared_before_offset(tu, first_user.extent.start.offset)

    assert "EarlyAlias" in names
    assert "EarlyTag" in names
    assert "__tenjin_atomic_i32_t" not in names
    assert "LateTag" not in names


def test_hoist_embedded_tag_definitions_unblocks_histindex_split(root, tmp_codebase):
    tmp_codebase.mkdir()
    sample_c = tmp_codebase / "sample.c"
    sample_c.write_text(
        "struct histindex {\n"
        "    struct record {\n"
        "        unsigned int ptr, cnt;\n"
        "        struct record *next;\n"
        "    } **records, **line_map;\n"
        "};\n",
        encoding="utf-8",
    )

    build_info = build_info_for_single_source(tmp_codebase, sample_c)
    hoist = c_refact.run_xj_hoist_embedded_tag_defs(tmp_codebase, build_info)
    c_refact_tag_hoister.apply_tag_hoisting_rewrites(tmp_codebase, hoist)
    split = c_refact.run_xj_locate_joined_decls(tmp_codebase, build_info)
    c_refact_decl_splitter.apply_decl_splitting_rewrites(tmp_codebase, split)

    rewritten = sample_c.read_text(encoding="utf-8")
    assert "struct histindex_record {" in rewritten
    assert "struct histindex_record *next;" in rewritten
    assert "struct histindex {\n    struct histindex_record **records;" in rewritten
    assert "struct histindex_record  **line_map;" in rewritten
    assert "struct record {" not in rewritten


def test_hoist_embedded_tag_definitions_supported_and_skipped_cases(root, tmp_codebase):
    tmp_codebase.mkdir()
    sample_c = tmp_codebase / "sample.c"
    sample_c.write_text(
        "struct Holder_Node { int collision; };\n"
        "struct Holder {\n"
        "    struct Node { struct Node *next, *previous; } *head, *tail;\n"
        "    struct Node *again;\n"
        "    union Value { int i; float f; } v1, v2;\n"
        "    enum Kind { K_A, K_B } k1, k2;\n"
        "};\n"
        "typedef struct { int x; } AliasA, AliasB;\n"
        "#define EMBEDDED(name) struct Macro { int x; } name##_1, name##_2\n"
        "struct MacroHolder { EMBEDDED(m); };\n"
        "int fn(void) {\n"
        "    struct Local { int x; } l1, l2;\n"
        "    return l1.x + l2.x;\n"
        "}\n",
        encoding="utf-8",
    )

    build_info = build_info_for_single_source(tmp_codebase, sample_c)
    hoist = c_refact.run_xj_hoist_embedded_tag_defs(tmp_codebase, build_info)
    c_refact_tag_hoister.apply_tag_hoisting_rewrites(tmp_codebase, hoist)

    rewritten = sample_c.read_text(encoding="utf-8")
    assert "struct Holder_Node_xj1 { struct Holder_Node_xj1 *next, *previous; };" in rewritten
    assert "struct Holder_Node_xj1 *head, *tail;" in rewritten
    assert "struct Holder_Node_xj1 *again;" in rewritten
    assert "union Holder_Value { int i; float f; };" in rewritten
    assert "union Holder_Value v1, v2;" in rewritten
    assert "enum Holder_Kind { K_A, K_B };" in rewritten
    assert "enum Holder_Kind k1, k2;" in rewritten
    assert re.search(r"struct xj_anon_struct_[0-9a-f]+ \{ int x; \};", rewritten)
    assert re.search(r"typedef struct xj_anon_struct_[0-9a-f]+ AliasA, AliasB;", rewritten)
    assert "struct Macro { int x; } name##_1, name##_2" in rewritten
    assert re.search(r"struct xj_Local_[0-9a-f]+ \{ int x; \};", rewritten)
    assert re.search(r"struct xj_Local_[0-9a-f]+ l1, l2;", rewritten)
