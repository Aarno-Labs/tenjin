from pathlib import Path

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
