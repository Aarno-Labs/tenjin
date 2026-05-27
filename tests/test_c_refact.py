from pathlib import Path

from clang.cindex import CursorKind  # type: ignore

import c_refact
import c_refact_type_mod_replicator
import compilation_database
from cindex_helpers import create_xj_clang_index


def write_compile_commands_for_sources(codebase: Path, sources: list[Path]) -> None:
    commands: list[compilation_database.CompileCommand] = []
    for source in sources:
        commands.extend(
            compilation_database.synthetic_compile_commands_for_c_file(source, codebase).commands
        )
    compilation_database.CompileCommands(commands).to_json_file(codebase / "compile_commands.json")


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
