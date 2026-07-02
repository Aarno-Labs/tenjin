from pathlib import Path
import re

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
        nonmain_tissue_functions={"foo"},
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
        j={
            "mutated_globals": [],
            "escaped_globals": [],
            "call_graph_components": [],
            "unique_filenames": {},
            "mutable_global_tissue": {"tissue": ["foo"]},
            "global_initializer_references": {},
        },
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_tissue_functions={"foo"},
    )

    rewritten_a = a_current.read_text(encoding="utf-8")
    rewritten_b = b_current.read_text(encoding="utf-8")

    for rewritten in (rewritten_a, rewritten_b):
        assert "typedef int (*callback_t)(int);" in rewritten
        assert "typedef int (*callback_t_xjtp)(struct XjGlobals *, int);" in rewritten
        assert "typedef int (*callback_t)(struct XjGlobals *, int);" not in rewritten
        assert "struct Holder { callback_t_xjtp cb; };" in rewritten


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
        nonmain_tissue_functions={"foo"},
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
        nonmain_tissue_functions={"foo"},
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
        nonmain_tissue_functions={"foo"},
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
        nonmain_tissue_functions={"foo"},
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

    j = {
        "mutated_globals": [],
        "escaped_globals": [],
        "call_graph_components": [
            {
                "call_sites": [
                    {
                        "line": direct_sites["tissue"][0],
                        "col": direct_sites["tissue"][1],
                        "p": "caller",
                        "uf": "sample",
                    },
                    {
                        "line": direct_sites["target"][0],
                        "col": direct_sites["target"][1],
                        "p": "caller",
                        "uf": "sample",
                    },
                ],
                "call_targets": ["<llvm-link>:tissue", "<llvm-link>:target"],
                "all_mutable": True,
            }
        ],
        "unique_filenames": {
            "sample": {
                "directory": prev_codebase.as_posix(),
                "filename": "sample.nolines.i",
            }
        },
        "mutable_global_tissue": {"tissue": ["tissue"]},
        "global_initializer_references": {},
    }

    c_refact.localize_mutable_globals_phase1(
        compdb=compdb,
        j=j,
        current_codebase=current_codebase,
        prev=prev_codebase,
        nonmain_tissue_functions={"tissue"},
    )

    rewritten = current_file.read_text(encoding="utf-8")
    assert "int tissue(struct XjGlobals *xjg, int (*f)(int), int x)" in rewritten
    assert "return tissue(((struct XjGlobals*)0), &target, ((target)(1)));" in rewritten
    assert "target(((struct XjGlobals*)0), x)" not in rewritten
    assert "target(((struct XjGlobals*)0), 1)" not in rewritten
    assert "((target)(((struct XjGlobals*)0), 1))" not in rewritten
