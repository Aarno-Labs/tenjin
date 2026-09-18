import json
from pathlib import Path
import re

import pytest
from clang.cindex import CursorKind  # type: ignore

import c_refact
import c_refact_decl_splitter
import c_refact_tag_hoister
import c_refact_type_mod_replicator
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


def disposition_manifest_for_localization(
    functions: list[str], fields: list[dict] | None = None, callsites: list[dict] | None = None
) -> dict:
    return {
        "schema_version": 8,
        "context_rewrite": {
            "selected": {
                "fields": fields or [],
                "accessors": [],
                "functions": functions,
                "rewrite_callsites": callsites or [],
            }
        },
    }


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
        json.dumps(disposition_manifest_for_localization([])), encoding="utf-8"
    )

    class UnusedCompdb:
        def to_json_file(self, _path):
            raise AssertionError("an empty selection must not enter the rewrite pipeline")

    c_refact.localize_mutable_globals(manifest_path, UnusedCompdb(), tmp_codebase, tmp_codebase)


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


def test_findfnptrdecls_marks_initialized_fnptr_vars_for_cross_tu_replication(root, tmp_codebase):
    tmp_codebase.mkdir()
    a_c = tmp_codebase / "a.c"
    b_c = tmp_codebase / "b.c"

    a_c.write_text(
        "extern int (*dispatch)(int);\nint use_dispatch(void) { return dispatch(7); }\n",
        encoding="utf-8",
    )
    b_c.write_text(
        "int foo(int x) { return x + 1; }\nint (*dispatch)(int) = foo;\n",
        encoding="utf-8",
    )
    write_compile_commands_for_sources(tmp_codebase, [a_c, b_c])

    output = c_refact.run_xj_prepare_findfnptrdecls(
        tmp_codebase,
        nonmain_context_functions={"foo"},
        all_function_names={"foo", "use_dispatch"},
    )

    modified_in_b = output["modified_fn_ptr_type_locs"].get(b_c.as_posix(), [])
    assert modified_in_b, output
    assert output["var_decl_fn_ptr_arg_lparen_locs"][a_c.as_posix()]["dispatch"] >= 0
    assert output["var_decl_fn_ptr_arg_lparen_locs"][b_c.as_posix()]["dispatch"] >= 0

    compdb = compilation_database.CompileCommands.from_json_file(
        tmp_codebase / "compile_commands.json"
    )
    tus = c_refact.parse_project(create_xj_clang_index(), compdb)
    equivalence_classes = c_refact_type_mod_replicator.collect_type_definitions(
        list(tus.values()), output["var_decl_fn_ptr_arg_lparen_locs"]
    )

    b_lparen, _b_rparen = modified_in_b[0]
    replicated = c_refact_type_mod_replicator.replicate_type_modifications(
        {b_c.as_posix(): [(b_lparen + 1, 0, "struct XjGlobals *, ")]},
        equivalence_classes,
    )

    a_rewrites = replicated.get(a_c.as_posix(), [])
    assert any(
        offset == output["var_decl_fn_ptr_arg_lparen_locs"][a_c.as_posix()]["dispatch"] + 1
        for offset, _, _ in a_rewrites
    ), replicated


def test_findfnptrdecls_does_not_cross_tu_replicate_local_fnptr_types(root, tmp_codebase):
    tmp_codebase.mkdir()
    a_c = tmp_codebase / "a.c"
    b_c = tmp_codebase / "b.c"

    a_c.write_text(
        "int needs_globals(int x) { return x + 1; }\n"
        "int use_needs_globals(int x) {\n"
        "    int (*fp)(int) = needs_globals;\n"
        "    return fp(x);\n"
        "}\n",
        encoding="utf-8",
    )
    b_c.write_text(
        "int stays_plain(int x) { return x - 1; }\n"
        "int use_stays_plain(int x) {\n"
        "    int (*fp)(int) = stays_plain;\n"
        "    return fp(x);\n"
        "}\n",
        encoding="utf-8",
    )
    write_compile_commands_for_sources(tmp_codebase, [a_c, b_c])

    output = c_refact.run_xj_prepare_findfnptrdecls(
        tmp_codebase,
        nonmain_context_functions={"needs_globals"},
        all_function_names={"needs_globals", "use_needs_globals", "stays_plain", "use_stays_plain"},
    )

    # The local `fp` in a.c must be rewritten for `needs_globals`, but
    # its unrelated, same-named counterpart in b.c has no cross-TU ABI.
    assert output["modified_fn_ptr_type_locs"].get(a_c.as_posix()), output
    assert "fp" not in output["var_decl_fn_ptr_arg_lparen_locs"].get(a_c.as_posix(), {})
    assert "fp" not in output["var_decl_fn_ptr_arg_lparen_locs"].get(b_c.as_posix(), {})


def test_localize_mutable_globals_phase1_clones_typedef_backed_field_types(root, tmp_codebase):
    current_codebase = tmp_codebase
    prev_codebase = tmp_codebase.parent / "prev_codebase"
    current_codebase.mkdir()
    prev_codebase.mkdir()

    shared_defs = "typedef int (*callback_t)(int);\nstruct Holder { callback_t cb; };\n"
    a_source = shared_defs + "int foo(int x) { return x + 1; }\nstruct Holder holder = { foo };\n"
    b_source = (
        shared_defs + "extern struct Holder holder;\n" + "int use(void) { return holder.cb(7); }\n"
    )
    a_current = current_codebase / "a.c"
    b_current = current_codebase / "b.c"
    a_prev = prev_codebase / "a.c"
    b_prev = prev_codebase / "b.c"
    for path, source in (
        (a_current, a_source),
        (b_current, b_source),
        (a_prev, a_source),
        (b_prev, b_source),
    ):
        path.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(current_codebase, [a_current, b_current])

    c_refact.localize_mutable_globals_phase1(
        compdb=compilation_database.CompileCommands.from_json_file(
            current_codebase / "compile_commands.json"
        ),
        manifest=disposition_manifest_for_localization(["foo"]),
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_context_functions={"foo"},
    )

    rewritten_a = a_current.read_text(encoding="utf-8")
    rewritten_b = b_current.read_text(encoding="utf-8")

    for rewritten in (rewritten_a, rewritten_b):
        assert "typedef int (*callback_t)(int);" in rewritten
        assert "typedef int (*callback_t_xjtp)(struct XjGlobals *, int);" in rewritten
        assert "typedef int (*callback_t)(struct XjGlobals *, int);" not in rewritten
        assert "struct Holder { callback_t_xjtp cb; };" in rewritten


def test_localize_mutable_globals_phase1_clones_typedef_in_function_redeclarations(
    root, tmp_codebase
):
    current_codebase = tmp_codebase
    prev_codebase = tmp_codebase.parent / "prev_codebase"
    current_codebase.mkdir()
    prev_codebase.mkdir()

    source = (
        "typedef int (*callback_t)(int);\n"
        "int apply(callback_t cb, int x);\n"
        "int foo(int x) { return x + 1; }\n"
        "int apply(callback_t cb, int x) { return cb(x); }\n"
        "int use(void) { return apply(foo, 1); }\n"
    )
    current_file = current_codebase / "sample.c"
    prev_file = prev_codebase / "sample.c"
    current_file.write_text(source, encoding="utf-8")
    prev_file.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(current_codebase, [current_file])

    c_refact.localize_mutable_globals_phase1(
        compdb=compilation_database.CompileCommands.from_json_file(
            current_codebase / "compile_commands.json"
        ),
        manifest=disposition_manifest_for_localization(["foo"]),
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_context_functions={"foo"},
    )

    rewritten = current_file.read_text(encoding="utf-8")
    assert "int apply(callback_t_xjtp cb, int x);" in rewritten
    assert "int apply(callback_t_xjtp cb, int x) {" in rewritten


def test_localize_mutable_globals_phase1_propagates_typedef_clone_from_param_to_field(
    root, tmp_codebase
):
    current_codebase = tmp_codebase
    prev_codebase = tmp_codebase.parent / "prev_codebase"
    current_codebase.mkdir()
    prev_codebase.mkdir()

    source = (
        "typedef int (*callback_t)(int);\n"
        "struct Holder { callback_t cb; };\n"
        "int apply(callback_t cb);\n"
        "int foo(int x) { return x + 1; }\n"
        "int apply(callback_t cb) {\n"
        "    struct Holder holder;\n"
        "    holder.cb = cb;\n"
        "    return 0;\n"
        "}\n"
        "int use(void) { return apply(foo); }\n"
    )
    current_file = current_codebase / "sample.c"
    prev_file = prev_codebase / "sample.c"
    current_file.write_text(source, encoding="utf-8")
    prev_file.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(current_codebase, [current_file])

    c_refact.localize_mutable_globals_phase1(
        compdb=compilation_database.CompileCommands.from_json_file(
            current_codebase / "compile_commands.json"
        ),
        manifest=disposition_manifest_for_localization(["foo"]),
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_context_functions={"foo"},
    )

    rewritten = current_file.read_text(encoding="utf-8")
    assert "struct Holder { callback_t_xjtp cb; };" in rewritten
    assert "int apply(callback_t_xjtp cb);" in rewritten
    assert "int apply(callback_t_xjtp cb) {" in rewritten


def test_localize_mutable_globals_phase1_propagates_typedef_clone_from_param_to_argument(
    root, tmp_codebase
):
    current_codebase = tmp_codebase
    prev_codebase = tmp_codebase.parent / "prev_codebase"
    current_codebase.mkdir()
    prev_codebase.mkdir()

    source = (
        "typedef int (*callback_t)(int);\n"
        "struct Holder { callback_t cb; };\n"
        "int foo(int x) { return x + 1; }\n"
        "int apply(callback_t cb) { return 0; }\n"
        "int use(struct Holder *holder) { return apply(foo) + apply(holder->cb); }\n"
    )
    current_file = current_codebase / "sample.c"
    prev_file = prev_codebase / "sample.c"
    current_file.write_text(source, encoding="utf-8")
    prev_file.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(current_codebase, [current_file])

    c_refact.localize_mutable_globals_phase1(
        compdb=compilation_database.CompileCommands.from_json_file(
            current_codebase / "compile_commands.json"
        ),
        manifest=disposition_manifest_for_localization(["foo"]),
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_context_functions={"foo"},
    )

    rewritten = current_file.read_text(encoding="utf-8")
    assert "struct Holder { callback_t_xjtp cb; };" in rewritten
    assert "int apply(callback_t_xjtp cb)" in rewritten


def test_localize_mutable_globals_phase1_clones_explicit_cast_typedef_use(root, tmp_codebase):
    current_codebase = tmp_codebase
    prev_codebase = tmp_codebase.parent / "prev_codebase"
    current_codebase.mkdir()
    prev_codebase.mkdir()

    source = (
        "typedef int (*callback_t)(int);\n"
        "int foo(int x) { return x + 1; }\n"
        "int bar(int x) { return x + 2; }\n"
        "int apply(callback_t cb) { return cb(1); }\n"
        "int use(void) { return apply(foo) + apply((callback_t)bar); }\n"
    )
    current_file = current_codebase / "sample.c"
    prev_file = prev_codebase / "sample.c"
    current_file.write_text(source, encoding="utf-8")
    prev_file.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(current_codebase, [current_file])

    c_refact.localize_mutable_globals_phase1(
        compdb=compilation_database.CompileCommands.from_json_file(
            current_codebase / "compile_commands.json"
        ),
        manifest=disposition_manifest_for_localization(["foo"]),
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_context_functions={"foo"},
    )

    rewritten = current_file.read_text(encoding="utf-8")
    assert "int apply(callback_t_xjtp cb)" in rewritten
    assert "apply((callback_t_xjtp)bar_xjw)" in rewritten
    assert "bar_xjw(struct XjGlobals*, int x)" in rewritten


def test_findfnptrdecls_tracks_call_args_to_fnptr_params(root, tmp_codebase):
    tmp_codebase.mkdir()
    callbacks_c = tmp_codebase / "callbacks.c"

    source = (
        "int foo(int x) { return x + 1; }\n"
        "int bar(int x) { return x + 2; }\n"
        "int apply(int (*cb)(int), int x) { return cb(x); }\n"
        "int use(void) { return apply(foo, 1) + apply(bar, 2); }\n"
    )
    callbacks_c.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(tmp_codebase, [callbacks_c])

    output = c_refact.run_xj_prepare_findfnptrdecls(
        tmp_codebase,
        nonmain_context_functions={"foo"},
        all_function_names={"apply", "bar", "foo", "use"},
    )

    modified_in_file = output["modified_fn_ptr_type_locs"].get(callbacks_c.as_posix(), [])
    assert len(modified_in_file) == 1, output
    start_offset, end_offset = modified_in_file[0]
    assert source[start_offset:end_offset] == "(int"

    wrappers = output["unmod_fn_occ_wrappers"].get(callbacks_c.as_posix(), [])
    assert len(wrappers) == 1, output
    wrapper = wrappers[0]
    assert wrapper["name"] == "bar"
    assert wrapper["suffix"] == "_xjw"
    assert wrapper["occ_offsets"] == [source.index("apply(bar, 2)") + len("apply(")]
    assert "bar_xjw(struct XjGlobals*, int x)" in wrapper["wrapper_defn"]


def test_findfnptrdecls_inserts_wrapper_after_forward_declaration(root, tmp_codebase):
    tmp_codebase.mkdir()
    callbacks_c = tmp_codebase / "callbacks.c"

    source = (
        "int bar(int x);\n"
        "int foo(int x) { return x + 1; }\n"
        "int apply(int (*cb)(int), int x) { return cb(x); }\n"
        "int use(void) { return apply(foo, 1) + apply(bar, 2); }\n"
        "int bar(int x) { return x + 2; }\n"
    )
    callbacks_c.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(tmp_codebase, [callbacks_c])

    output = c_refact.run_xj_prepare_findfnptrdecls(
        tmp_codebase,
        nonmain_context_functions={"foo"},
        all_function_names={"apply", "bar", "foo", "use"},
    )

    wrappers = output["unmod_fn_occ_wrappers"].get(callbacks_c.as_posix(), [])
    assert len(wrappers) == 1, output
    assert wrappers[0]["decl_post_offset"] == source.index(";") + 1


def test_findfnptrdecls_deduplicates_wrapper_across_function_redeclarations(root, tmp_codebase):
    tmp_codebase.mkdir()
    callbacks_c = tmp_codebase / "callbacks.c"

    source = (
        "int bar(int x);\n"
        "int foo(int x) { return x + 1; }\n"
        "int apply(int (*cb)(int), int x) { return cb(x); }\n"
        "int use_before(void) { return apply(foo, 1) + apply(bar, 2); }\n"
        "int bar(int x) { return x + 2; }\n"
        "int use_after(void) { return apply(bar, 3); }\n"
    )
    callbacks_c.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(tmp_codebase, [callbacks_c])

    output = c_refact.run_xj_prepare_findfnptrdecls(
        tmp_codebase,
        nonmain_context_functions={"foo"},
        all_function_names={"apply", "bar", "foo", "use_after", "use_before"},
    )

    wrappers = output["unmod_fn_occ_wrappers"].get(callbacks_c.as_posix(), [])
    assert len(wrappers) == 1, output
    wrapper = wrappers[0]
    assert wrapper["name"] == "bar"
    assert wrapper["decl_post_offset"] == source.index(";") + 1
    assert wrapper["occ_offsets"] == [
        source.index("apply(bar, 2)") + len("apply("),
        source.index("apply(bar, 3)") + len("apply("),
    ]


def test_findfnptrdecls_tracks_ampersand_call_args_to_fnptr_params(root, tmp_codebase):
    tmp_codebase.mkdir()
    callbacks_c = tmp_codebase / "callbacks.c"

    source = (
        "int foo(int x) { return x + 1; }\n"
        "int bar(int x) { return x + 2; }\n"
        "int apply(int (*cb)(int), int x) { return cb(x); }\n"
        "int use(void) { return apply(&foo, 1) + apply(&bar, 2); }\n"
    )
    callbacks_c.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(tmp_codebase, [callbacks_c])

    output = c_refact.run_xj_prepare_findfnptrdecls(
        tmp_codebase,
        nonmain_context_functions={"foo"},
        all_function_names={"apply", "bar", "foo", "use"},
    )

    modified_in_file = output["modified_fn_ptr_type_locs"].get(callbacks_c.as_posix(), [])
    assert len(modified_in_file) == 1, output
    start_offset, end_offset = modified_in_file[0]
    assert source[start_offset:end_offset] == "(int"

    wrappers = output["unmod_fn_occ_wrappers"].get(callbacks_c.as_posix(), [])
    assert len(wrappers) == 1, output
    wrapper = wrappers[0]
    assert wrapper["name"] == "bar"
    assert wrapper["suffix"] == "_xjw"
    assert wrapper["occ_offsets"] == [source.index("&bar") + 1]
    assert "bar_xjw(struct XjGlobals*, int x)" in wrapper["wrapper_defn"]


def test_findfnptrdecls_tracks_ampersand_assignments_and_var_initializers(root, tmp_codebase):
    tmp_codebase.mkdir()
    callbacks_c = tmp_codebase / "callbacks.c"

    source = (
        "int foo(int x) { return x + 1; }\n"
        "int bar(int x) { return x + 2; }\n"
        "int (*fp)(int) = &foo;\n"
        "void use(void) { fp = &bar; }\n"
    )
    callbacks_c.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(tmp_codebase, [callbacks_c])

    output = c_refact.run_xj_prepare_findfnptrdecls(
        tmp_codebase,
        nonmain_context_functions={"foo"},
        all_function_names={"bar", "foo", "use"},
    )

    modified_in_file = output["modified_fn_ptr_type_locs"].get(callbacks_c.as_posix(), [])
    assert len(modified_in_file) == 1, output
    wrappers = output["unmod_fn_occ_wrappers"].get(callbacks_c.as_posix(), [])
    assert len(wrappers) == 1, output
    wrapper = wrappers[0]
    assert wrapper["name"] == "bar"
    assert wrapper["occ_offsets"] == [source.index("&bar") + 1]


def test_findfnptrdecls_tracks_ampersand_init_list_entries(root, tmp_codebase):
    tmp_codebase.mkdir()
    callbacks_c = tmp_codebase / "callbacks.c"

    source = (
        "int foo(int x) { return x + 1; }\n"
        "int bar(int x) { return x + 2; }\n"
        "struct Holder { int (*cb)(int); };\n"
        "struct Holder mod = { &foo };\n"
        "struct Holder unmod = { &bar };\n"
    )
    callbacks_c.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(tmp_codebase, [callbacks_c])

    output = c_refact.run_xj_prepare_findfnptrdecls(
        tmp_codebase,
        nonmain_context_functions={"foo"},
        all_function_names={"bar", "foo"},
    )

    modified_in_file = output["modified_fn_ptr_type_locs"].get(callbacks_c.as_posix(), [])
    assert len(modified_in_file) == 1, output
    wrappers = output["unmod_fn_occ_wrappers"].get(callbacks_c.as_posix(), [])
    assert len(wrappers) == 1, output
    wrapper = wrappers[0]
    assert wrapper["name"] == "bar"
    assert wrapper["occ_offsets"] == [source.index("&bar") + 1]


def test_localize_mutable_globals_phase1_skips_direct_calls_to_nontissue_functions(
    root, tmp_codebase
):
    current_codebase = tmp_codebase
    prev_codebase = tmp_codebase.parent / "prev_codebase"
    current_codebase.mkdir()
    prev_codebase.mkdir()

    source = (
        "int target(int x) { return x + 1; }\n"
        "int tissue(int (*f)(int), int x) { return x ? f(x) : target(x); }\n"
        "int caller(void) { return tissue(&target, ((target)(1))); }\n"
    )
    current_file = current_codebase / "sample.nolines.i"
    prev_file = prev_codebase / "sample.nolines.i"
    current_file.write_text(source, encoding="utf-8")
    prev_file.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(current_codebase, [current_file])

    compdb = compilation_database.CompileCommands.from_json_file(
        current_codebase / "compile_commands.json"
    )
    tus = c_refact.parse_project(create_xj_clang_index(), compdb)
    call_exprs_by_loc = c_refact.collect_cursors_by_loc(tus, [CursorKind.CALL_EXPR])

    direct_sites_by_name: dict[str, list[tuple[int, int]]] = {}
    for (line, col, filepath), cursors in call_exprs_by_loc.items():
        if filepath != current_file.as_posix():
            continue
        for cursor in cursors:
            name = c_refact.direct_call_callee_name(cursor)
            if name in {"target", "tissue"}:
                direct_sites_by_name.setdefault(name, []).append((line, col))

    direct_sites = {name: max(sites) for name, sites in direct_sites_by_name.items() if sites}

    manifest = disposition_manifest_for_localization(
        ["tissue"],
        callsites=[
            {
                "key": "caller@sample#0",
                "caller": "caller",
                "callees": ["tissue"],
                "unresolved": False,
                "site": {
                    "file": prev_file.as_posix(),
                    "line": direct_sites["tissue"][0],
                    "col": direct_sites["tissue"][1],
                },
            }
        ],
    )

    c_refact.localize_mutable_globals_phase1(
        compdb=compdb,
        manifest=manifest,
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_context_functions={"tissue"},
    )

    rewritten = current_file.read_text(encoding="utf-8")
    assert "int tissue(struct XjGlobals *xjg, int (*f)(int), int x)" in rewritten
    assert "return tissue(((struct XjGlobals*)0), &target, ((target)(1)));" in rewritten
    assert "target(((struct XjGlobals*)0), x)" not in rewritten
    assert "target(((struct XjGlobals*)0), 1)" not in rewritten
    assert "((target)(((struct XjGlobals*)0), 1))" not in rewritten


def test_localize_mutable_globals_phase1_closes_over_source_only_calls(root, tmp_codebase):
    current_codebase = tmp_codebase
    prev_codebase = tmp_codebase.parent / "prev_codebase"
    current_codebase.mkdir()
    prev_codebase.mkdir()

    source = (
        "static int context_leaf(int x) { return x + 1; }\n"
        "static int dead_middle(int x) { return context_leaf(x); }\n"
        "static int dead_outer(int x) { return dead_middle(x); }\n"
        "int main(void) {\n"
        '    if (sizeof("S_MP_WORD_TOO_SMALL_C") == 1u) return dead_outer(1);\n'
        "    return 0;\n"
        "}\n"
    )
    current_file = current_codebase / "sample.nolines.i"
    prev_file = prev_codebase / "sample.nolines.i"
    current_file.write_text(source, encoding="utf-8")
    prev_file.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(current_codebase, [current_file])

    results = c_refact.localize_mutable_globals_phase1(
        compdb=compilation_database.CompileCommands.from_json_file(
            current_codebase / "compile_commands.json"
        ),
        manifest=disposition_manifest_for_localization(["context_leaf"]),
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_context_functions={"context_leaf"},
    )

    rewritten = current_file.read_text(encoding="utf-8")
    assert results.nonmain_context_functions == {"context_leaf", "dead_middle", "dead_outer"}
    assert "context_leaf(struct XjGlobals *xjg, int x)" in rewritten
    assert "dead_middle(struct XjGlobals *xjg, int x)" in rewritten
    assert "dead_outer(struct XjGlobals *xjg, int x)" in rewritten
    assert "int main(void)" in rewritten
    assert "context_leaf(((struct XjGlobals*)0), x)" in rewritten
    assert "dead_middle(((struct XjGlobals*)0), x)" in rewritten
    assert "dead_outer(((struct XjGlobals*)0), 1)" in rewritten


def test_localize_mutable_globals_phase1_seeds_source_only_global_users(root, tmp_codebase):
    current_codebase = tmp_codebase
    prev_codebase = tmp_codebase.parent / "prev_codebase"
    current_codebase.mkdir()
    prev_codebase.mkdir()
    (current_codebase / "demo").mkdir()
    (prev_codebase / "demo").mkdir()

    source = (
        "static int localized_global;\n"
        "static int dead_leaf(void) { return localized_global; }\n"
        "static int dead_caller(void) { return dead_leaf(); }\n"
        "int main(void) {\n"
        '    if (sizeof("DEAD_C") == 1u) return dead_caller();\n'
        "    return 0;\n"
        "}\n"
    )
    current_file = current_codebase / "demo" / "sample.nolines.i"
    prev_file = prev_codebase / "demo" / "sample.nolines.i"
    current_file.write_text(source, encoding="utf-8")
    prev_file.write_text(source, encoding="utf-8")
    write_compile_commands_for_sources(current_codebase, [current_file])
    (current_codebase / "xj-guidance.json").write_text("{}", encoding="utf-8")

    manifest_path = current_codebase / "pangs-manifest.json"
    manifest_path.write_text(
        json.dumps(
            disposition_manifest_for_localization([], fields=[{"llvm_name": "localized_global"}])
        ),
        encoding="utf-8",
    )
    # Migration comparison for the legacy materializer, not the production
    # entry point (which now requires a source-complete PANGS contract).
    c_refact._localize_mutable_globals_in_place(
        manifest_path=manifest_path,
        compdb=compilation_database.CompileCommands.from_json_file(
            current_codebase / "compile_commands.json"
        ),
        prev=prev_codebase,
        current_codebase=current_codebase,
    )

    rewritten = current_file.read_text(encoding="utf-8")
    assert "dead_leaf(struct XjGlobals *xjg)" in rewritten
    assert "dead_caller(struct XjGlobals *xjg)" in rewritten
    assert "int main(void)" in rewritten
    assert "dead_leaf(xjg)" in rewritten
    assert "dead_caller(xjg)" in rewritten
    assert (current_codebase / "xj_globals.h").is_file()
    assert '#include "../xj_globals.h"' in rewritten
