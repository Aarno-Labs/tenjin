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
