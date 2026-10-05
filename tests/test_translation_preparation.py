import os
import json
import re
from pathlib import Path

import c_refact
import cindex_helpers
import compilation_database
import hermetic
import ingest_tracking
import targets
import translation
import translation_preparation
from translation_types import TranslationFlags
import pangs_source
import pytest


def _named_decl(name: str, usr: str, offset: int = 0) -> c_refact.NamedDeclInfo:
    return c_refact.NamedDeclInfo(
        spelling=name,
        file_path="/code/main.i",
        decl_start_byte_offset=offset,
        decl_end_byte_offset=offset + 1,
        decl_location_byte_offset=offset,
        start_line=1,
        start_col=1,
        end_line=1,
        end_col=2,
        usr=usr,
    )


def test_redirect_nexttoward_calls_with_matching_signatures(tmp_path):
    declared_source = (
        "typedef long double extended;\n"
        "extern double nexttoward(double, extended);\n"
        "extern float nexttowardf(float, extended);\n"
        "double declared_double(double x, extended y) {\n"
        "    double (*alias)(double, long double) = nexttoward;\n"
        "    return nexttoward(x, y) + alias(x, y);\n"
        "}\n"
        "float declared_float(float x, extended y) { return nexttowardf(x, y); }\n"
    )
    defined_source = (
        "double nexttoward(double x, long double y) { return x; }\n"
        "float nexttowardf(float x, long double y) { return x; }\n"
        "double defined_double(double x, long double y) { return nexttoward(x, y); }\n"
        "float defined_float(float x, long double y) { return nexttowardf(x, y); }\n"
    )
    mismatched_source = (
        "double nexttoward(double x, double y) { return x; }\n"
        "float nexttowardf(double x, long double y) { return x; }\n"
        "double wrong_double(double x, double y) { return nexttoward(x, y); }\n"
        "float wrong_float(float x, long double y) { return nexttowardf(x, y); }\n"
    )
    declared_path = tmp_path / "declared.nolines.i"
    defined_path = tmp_path / "defined.nolines.i"
    mismatched_path = tmp_path / "mismatched.nolines.i"
    declared_path.write_text(declared_source, encoding="utf-8")
    defined_path.write_text(defined_source, encoding="utf-8")
    mismatched_path.write_text(mismatched_source, encoding="utf-8")
    commands = [
        compilation_database.CompileCommand(
            directory=tmp_path.as_posix(),
            file=path.as_posix(),
            arguments=["clang", "-x", "c", path.as_posix()],
        )
        for path in (declared_path, defined_path, mismatched_path)
    ]

    assert translation_preparation.rewrite_nexttoward_calls(commands) == 4
    assert declared_path.read_text(encoding="utf-8") == declared_source.replace(
        "return nexttoward(x, y)", "return nextafter(x, y)"
    ).replace("return nexttowardf(x, y)", "return nextafterf(x, y)")
    assert defined_path.read_text(encoding="utf-8") == defined_source.replace(
        "return nexttoward(x, y)", "return nextafter(x, y)"
    ).replace("return nexttowardf(x, y)", "return nextafterf(x, y)")
    assert mismatched_path.read_text(encoding="utf-8") == mismatched_source


def test_static_uniquification_only_suffixes_collisions():
    singleton = _named_decl("singleton", "static-singleton")
    singleton_redecl = _named_decl("singleton", "static-singleton", 10)
    duplicate_1 = _named_decl("duplicate", "static-duplicate-1", 20)
    duplicate_2 = _named_decl("duplicate", "static-duplicate-2", 30)
    external_collision = _named_decl("external_collision", "static-external", 40)
    external = _named_decl("external_collision", "external", 50)

    statics = [
        singleton,
        singleton_redecl,
        duplicate_1,
        duplicate_2,
        external_collision,
    ]
    plan = translation_preparation._plan_static_uniquification(statics, [*statics, external])

    assert plan == {
        "static-singleton": "singleton",
        "static-duplicate-1": "duplicate_xjtr_0",
        "static-duplicate-2": "duplicate_xjtr_1",
        "static-external": "external_collision_xjtr_0",
    }


def test_static_uniquification_avoids_occupied_names_and_preserves_source_suffixes():
    duplicate_1 = _named_decl("duplicate", "static-duplicate-1")
    duplicate_2 = _named_decl("duplicate", "static-duplicate-2", 10)
    occupied_candidate = _named_decl("duplicate_xjtr_0", "external-occupied", 20)
    source_name_using_reserved_suffix = _named_decl("natural_xjtr_0", "static-natural", 30)

    statics = [duplicate_1, duplicate_2, source_name_using_reserved_suffix]
    plan = translation_preparation._plan_static_uniquification(
        statics, [*statics, occupied_candidate]
    )

    assert plan == {
        "static-duplicate-1": "duplicate_xjtr_1",
        "static-duplicate-2": "duplicate_xjtr_2",
        "static-natural": "natural_xjtr_0",
    }


@pytest.mark.parametrize(
    "other_function",
    [
        "int second(void) { int shared = 2; return shared; }\n",
        "int second(int shared) { return shared; }\n",
    ],
    ids=["local", "parameter"],
)
def test_static_uniquification_reserves_local_names(other_function):
    source = (
        "int first(void) { static int shared = 1; return ++shared; }\n"
        + other_function
        + (
            "int third(void) { int shared_xjtr_0 = 3; return shared_xjtr_0; }\n"
            "int fourth(void) { static int singleton = 4; return singleton; }\n"
        )
    )
    tu = cindex_helpers.create_xj_clang_index().parse(
        "locals.c", args=["-xc"], unsaved_files=[("locals.c", source)]
    )
    assert not list(tu.diagnostics)
    statics = [
        c_refact.mk_NamedDeclInfo(c)
        for c in c_refact.compute_globals_and_statics_for_translation_units([tu], statics_only=True)
    ]
    project_symbols = [
        c_refact.mk_NamedDeclInfo(c)
        for c in c_refact.compute_global_symbol_inventory_for_translation_units([tu])
    ]
    plan = translation_preparation._plan_static_uniquification(statics, project_symbols)

    assert {static.spelling: plan[static.usr] for static in statics} == {
        "shared": "shared_xjtr_1",
        "singleton": "singleton",
    }
    static_usrs = {static.usr for static in statics}
    locals = [
        symbol
        for symbol in project_symbols
        if symbol.spelling in {"shared", "shared_xjtr_0"} and symbol.usr not in static_usrs
    ]
    assert {symbol.spelling for symbol in locals} == {"shared", "shared_xjtr_0"}
    assert all(symbol.usr not in plan for symbol in locals)


def test_collect_decls_distinguishes_extern_from_tentative_definition(tmp_path):
    header = tmp_path / "file.h"
    header.write_text("extern const char *file_names[];\n", encoding="utf-8")
    source = tmp_path / "apprentice.c"
    source.write_text('#include "file.h"\nconst char *file_names[62];\n', encoding="utf-8")
    compdb = compilation_database.synthetic_compile_commands_for_c_file(source, tmp_path)

    declarations = translation_preparation.collect_decls_by_rel_tu(tmp_path, compdb)
    names = declarations["apprentice.c"]["file_names"]
    assert [(path, text, is_defn) for path, _, _, text, is_defn in names] == [
        ("file.h", "extern const char *file_names[]", False),
        ("apprentice.c", "const char *file_names[62]", True),
    ]


@pytest.mark.parametrize(
    "sources, expected",
    [
        ({"a.i": "extern long value", "b.i": "extern long value"}, "extern long value"),
        ({"a.i": "extern long value", "b.i": "extern int value"}, None),
        ({"a.i": "extern long value"}, None),
        ({"a.i": "extern long value", "b.i": "extern short value"}, None),
    ],
    ids=["all-modified", "one-unchanged", "one-missing", "different-modifications"],
)
def test_header_consolidation_requires_every_tu_to_agree(sources, expected):
    assert translation_preparation._common_header_source(sources, {"a.i", "b.i"}) == expected


@pytest.mark.parametrize("indexed_word", [False, True], ids=["both-modified", "one-modified"])
def test_refolding_preserves_header_used_for_distinct_function_variants(
    root, tmp_codebase, tmp_resultsdir, indexed_word
):
    tmp_codebase.mkdir()
    header = (
        "static inline void FN(copy)(DataType *dst, const DataType *src, int n) {\n"
        "#ifdef USE_INDICES\n"
        "    for (int i = 0; i < n; i++) dst[i] = src[i];\n"
        "#else\n"
        "    while (n--) *dst++ = *src++;\n"
        "#endif\n"
        "}\n"
    )
    (tmp_codebase / "template.h").write_text(header, encoding="utf-8")
    (tmp_codebase / "main.c").write_text(
        "#define FN(name) name##Byte\n#define DataType unsigned char\n"
        '#include "template.h"\n'
        "#undef FN\n#undef DataType\n"
        "#define FN(name) name##Word\n#define DataType unsigned short\n"
        + ("#define USE_INDICES\n" if indexed_word else "")
        + '#include "template.h"\n'
        "int main(void) {\n"
        "    unsigned char a[3] = {0}, b[3] = {1, 2, 3};\n"
        "    unsigned short c[3] = {0}, d[3] = {256, 512, 768};\n"
        "    copyByte(a, b, 3); copyWord(c, d, 3);\n"
        "    return a[2] != 3 || c[2] != 768;\n"
        "}\n",
        encoding="utf-8",
    )
    flags = TranslationFlags.simple(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="header_variants",
        buildcmd="cc main.c -o main",
    )
    tracker = ingest_tracking.TimingRepo(translation.stub_ingestion_record(tmp_codebase, {}, []))
    translation_preparation.run_preparation_passes(flags, {}, tracker)

    prepared = next(tmp_resultsdir.glob("c_*_refold_preprocessor"))
    assert (prepared / "template.h").read_text(encoding="utf-8") == header
    transformed = next(tmp_resultsdir.glob("c_*_pointertransform"))
    assert "dst_index_xj" in (transformed / "main.nolines.i").read_text(encoding="utf-8")
    hermetic.run(["cc", "main.c", "-o", "main"], cwd=prepared, check=True)
    assert hermetic.run([prepared / "main"], check=False).returncode == 0


def test_refolding_preserves_extern_array_declaration(root, tmp_codebase, tmp_resultsdir):
    tmp_codebase.mkdir()
    (tmp_codebase / "file.h").write_text("extern const int file_names[];\n", encoding="utf-8")
    (tmp_codebase / "apprentice.c").write_text(
        '#include "file.h"\nconst int file_names[2];\n', encoding="utf-8"
    )
    (tmp_codebase / "consumer.c").write_text(
        '#include "file.h"\nint main(void) { return file_names[0] != 0; }\n',
        encoding="utf-8",
    )
    (tmp_codebase / "Makefile").write_text(
        "program: apprentice.c consumer.c file.h\n"
        "\t$(CC) -c apprentice.c -o apprentice.o\n"
        "\t$(CC) -c consumer.c -o consumer.o\n"
        "\t$(CC) apprentice.o consumer.o -o program\n",
        encoding="utf-8",
    )
    translation.do_translate(
        TranslationFlags.simple(
            root, tmp_codebase, tmp_resultsdir, cratename="tentative_array", buildcmd="make"
        ),
        guidance_path_or_literal="{}",
    )

    prepared = next(tmp_resultsdir.glob("c_*_refold_preprocessor"))
    assert (prepared / "file.h").read_text(encoding="utf-8") == ("extern const int file_names[];\n")
    initial_rust = tmp_resultsdir / "00_out" / "program" / "src"
    apprentice = (initial_rust / "apprentice.rs").read_text(encoding="utf-8")
    consumer = (initial_rust / "consumer.rs").read_text(encoding="utf-8")
    assert "file_names:" in apprentice
    assert "file_names:" in consumer
    definition = r"\bstatic(?: mut)? file_names:\s*\[[^\]]+\]\s*="
    assert re.search(definition, apprentice)
    assert not re.search(definition, consumer)


@pytest.mark.xfail(reason="triggers a pre-refold-consolidation bug")
def test_refolding_keeps_macro_header_used_for_functions_and_array(
    root, tmp_codebase, tmp_resultsdir
):
    tmp_codebase.mkdir()
    header_text = "LIBM_DDD(nextafter)\nLIBM_DDD(nexttoward)\n"
    (tmp_codebase / "libm.h").write_text(header_text, encoding="utf-8")
    (tmp_codebase / "main.c").write_text(
        "#include <math.h>\n"
        "#define LIBM_DDD(name) static double f_##name(double x, long double y) "
        "{ return name(x, y); }\n"
        '#include "libm.h"\n'
        "#undef LIBM_DDD\n"
        "struct entry { const char *name; double (*fn)(double, long double); };\n"
        "#define LIBM_DDD(name) { #name, f_##name },\n"
        "static const struct entry functions[] = {\n"
        '#include "libm.h"\n'
        "};\n"
        "int main(void) { return functions[1].fn(1.0, 2.0L) != 1.0; }\n",
        encoding="utf-8",
    )
    flags = TranslationFlags.simple(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="reused_macro_header",
        buildcmd="cc main.c -o main.exe -lm",
    )
    tracker = ingest_tracking.TimingRepo(translation.stub_ingestion_record(tmp_codebase, {}, []))
    translation_preparation.run_preparation_passes(flags, {}, tracker)

    prepared = next(tmp_resultsdir.glob("c_*_refold_preprocessor"))
    assert (prepared / "libm.h").read_text(encoding="utf-8") == header_text
    refolded = (prepared / "main.c").read_text(encoding="utf-8")
    assert "nextafter(x, y)" in refolded
    assert '#include "libm.h"' in refolded
    hermetic.run(["cc", "main.c", "-o", "main.exe", "-lm"], cwd=prepared, check=True)


def _immutable_global(name, declaration=None):
    return {
        "meta": {"llvm_name": name},
        "disposition": {"chosen": "immutable"},
        "facts": {
            "source_obligations": {
                "emitter": "tenjin-c2rust-default-v1",
                "declaration": declaration or name,
            }
        },
    }


def test_add_immutable_dispositions_to_guidance_preserves_existing_entries(tmp_path):
    manifest_path = tmp_path / "pangs-manifest.json"
    manifest_path.write_text(
        json.dumps({
            "schema_version": 8,
            "globals": [
                _immutable_global("global_immutable"),
                _immutable_global(
                    "function.static_immutable_xjtr_0", "function:static_immutable_xjtr_0"
                ),
                _immutable_global("explicitly_immutable"),
                {
                    "meta": {"llvm_name": "still_mutable"},
                    "disposition": {"chosen": "unhandled"},
                },
            ],
        }),
        encoding="utf-8",
    )
    guidance_path = tmp_path / "xj-guidance.json"
    guidance_path.write_text(
        json.dumps({
            "vars_mut": {
                "global_immutable": True,
                "explicitly_immutable": False,
                "user_choice": False,
            }
        }),
        encoding="utf-8",
    )

    added = translation_preparation.add_immutable_dispositions_to_guidance(
        manifest_path,
        guidance_path,
    )

    assert added == {"function:static_immutable_xjtr_0"}
    assert json.loads(guidance_path.read_text(encoding="utf-8")) == {
        "vars_mut": {
            "global_immutable": True,
            "explicitly_immutable": False,
            "user_choice": False,
        },
        "semantically_immutable_globals": ["function:static_immutable_xjtr_0"],
    }


def test_add_immutable_dispositions_to_guidance_creates_semantic_entry(tmp_path):
    manifest_path = tmp_path / "pangs-manifest.json"
    manifest_path.write_text(
        json.dumps({
            "schema_version": 8,
            "globals": [_immutable_global("global_immutable")],
        }),
        encoding="utf-8",
    )
    guidance_path = tmp_path / "xj-guidance.json"
    guidance_path.write_text(json.dumps({"public_api": []}), encoding="utf-8")

    translation_preparation.add_immutable_dispositions_to_guidance(manifest_path, guidance_path)

    assert json.loads(guidance_path.read_text(encoding="utf-8")) == {
        "public_api": [],
        "vars_mut": {},
        "semantically_immutable_globals": ["global_immutable"],
    }


def test_missing_disposition_does_not_imply_immutability(tmp_path):
    manifest_path = tmp_path / "pangs-manifest.json"
    manifest_path.write_text(
        json.dumps({
            "schema_version": 8,
            "globals": [
                {
                    "meta": {"llvm_name": "explicitly_unhandled"},
                    "disposition": {"chosen": "unhandled"},
                },
                {
                    "meta": {"llvm_name": "record_without_disposition"},
                    "disposition": None,
                },
            ],
        }),
        encoding="utf-8",
    )
    guidance_path = tmp_path / "xj-guidance.json"
    guidance_path.write_text("{}", encoding="utf-8")

    added = translation_preparation.add_immutable_dispositions_to_guidance(
        manifest_path,
        guidance_path,
    )

    assert added == set()
    assert json.loads(guidance_path.read_text(encoding="utf-8")) == {}


def test_add_immutable_dispositions_preserves_existing_semantic_entries(tmp_path):
    manifest_path = tmp_path / "pangs-manifest.json"
    manifest_path.write_text(
        json.dumps({
            "schema_version": 8,
            "globals": [
                _immutable_global("already_recorded"),
                _immutable_global("newly_recorded"),
            ],
        }),
        encoding="utf-8",
    )
    guidance_path = tmp_path / "xj-guidance.json"
    guidance_path.write_text(
        json.dumps({"semantically_immutable_globals": ["already_recorded"]}),
        encoding="utf-8",
    )

    added = translation_preparation.add_immutable_dispositions_to_guidance(
        manifest_path,
        guidance_path,
    )

    assert added == {"newly_recorded"}
    assert json.loads(guidance_path.read_text(encoding="utf-8")) == {
        "vars_mut": {},
        "semantically_immutable_globals": ["already_recorded", "newly_recorded"],
    }


def test_immutable_selection_requires_an_emitter_binding(tmp_path):
    manifest = tmp_path / "manifest.json"
    manifest.write_text(
        json.dumps({
            "schema_version": 8,
            "globals": [
                {
                    "meta": {"llvm_name": "g"},
                    "disposition": {"chosen": "immutable"},
                }
            ],
        })
    )
    guidance = tmp_path / "guidance.json"
    guidance.write_text("{}")
    with pytest.raises(pangs_source.ContractViolation, match="declaration binding"):
        translation_preparation.add_immutable_dispositions_to_guidance(manifest, guidance)
    assert guidance.read_text() == "{}"


def test_remap_path_prefix_in_argument_only_rewrites_absolute_path_components():
    remap = translation_preparation._remap_path_prefix_in_argument
    source = Path("/md4c")
    dest = Path("/results/c_01_intercept_build")

    assert remap("/md4c/src/md4c.c", source, dest) == ("/results/c_01_intercept_build/src/md4c.c")
    assert remap("-I/md4c/src", source, dest) == "-I/results/c_01_intercept_build/src"
    assert remap("--sysroot=/md4c/sysroot", source, dest) == (
        "--sysroot=/results/c_01_intercept_build/sysroot"
    )
    assert remap("/md4c/lib:/md4c/lib64", source, dest) == (
        "/results/c_01_intercept_build/lib:/results/c_01_intercept_build/lib64"
    )
    assert remap("CMakeFiles/md4c-html.dir/md4c-html.c.o", source, dest) == (
        "CMakeFiles/md4c-html.dir/md4c-html.c.o"
    )
    assert remap("/md4c-other/src", source, dest) == "/md4c-other/src"

    source_prefixed_dest = Path("/md4c-results/c_01_intercept_build")
    assert remap("/md4c/src/md4c.c", source, source_prefixed_dest) == (
        "/md4c-results/c_01_intercept_build/src/md4c.c"
    )
    assert remap("/md4c-results/_build_1/src", source, source_prefixed_dest) == (
        "/md4c-results/_build_1/src"
    )


def test_xj_generated_sources_preserves_extensionless_prebuild_output(tmp_path, monkeypatch):
    original_codebase = tmp_path / "original"
    current_codebase = tmp_path / "current"
    builddir = tmp_path / "build"
    original_codebase.mkdir()
    current_codebase.mkdir()
    builddir.mkdir()

    pre_build_files = translation_preparation.snapshot_codebase_files(original_codebase)
    blocktags = original_codebase / "blocktags"
    blocktags.write_text("#!/bin/sh\n", encoding="utf-8")
    blocktags.chmod(0o755)
    monkeypatch.setenv("XJ_GENERATED_SOURCES", "blocktags;ignored-helper")

    translation_preparation.relocate_generated_files(
        original_codebase, pre_build_files, current_codebase, builddir
    )

    assert not blocktags.exists()
    assert (builddir / "blocktags").exists()
    assert (current_codebase / "blocktags").exists()
    assert os.access(current_codebase / "blocktags", os.X_OK)


def test_prebuild_uses_interceptors_but_discards_its_commands(tmp_path):
    codebase = tmp_path / "codebase"
    builddir = tmp_path / "build"
    codebase.mkdir()

    (codebase / "prebuild.c").write_text("int configured_probe;\n", encoding="utf-8")
    configure = codebase / "configure.sh"
    configure.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        "cc -c prebuild.c -o prebuild.o\n"
        "ar_path=$(command -v ar)\n"
        'printf \'#!/bin/sh\\n"%s" rcs libfrombuild.a\\n\' "$ar_path" > build.sh\n'
        "chmod +x build.sh\n",
        encoding="utf-8",
    )
    configure.chmod(0o755)

    build_info = targets.BuildInfo()
    translation_preparation.compute_build_info_in(
        builddir=builddir,
        codebase=codebase,
        prebuildcmd="./configure.sh && true",
        buildcmd=["./build.sh"],
        tracker=None,  # type: ignore[arg-type]
        mut_build_info=build_info,
    )

    # The prebuild compiler probe was captured only in a disposable directory.
    # The generated build script's absolute `ar` path still names an interceptor,
    # so its build-time invocation is the sole retained target-producing command.
    assert build_info.get_all_targets() == [
        targets.BuildTarget(
            key="libfrombuild.a",
            type=targets.TargetType.STATIC,
            stem_not_unique="libfrombuild",
        )
    ]
    retained_commands = list((codebase / ".xj-build-commands").glob("*.json"))
    assert len(retained_commands) == 1
    assert json.loads(retained_commands[0].read_text(encoding="utf-8"))["type"] == "ar"


def test_copy_preparation_stage_omits_refold_maps(tmp_path):
    previous = tmp_path / "previous"
    current = tmp_path / "current"
    nested = previous / "src"
    nested.mkdir(parents=True)
    (nested / "sample.nolines.i").write_text("translation unit", encoding="utf-8")
    (nested / "sample.nolines.refoldmap.json").write_text("large map", encoding="utf-8")
    (nested / "keep.json").write_text("other metadata", encoding="utf-8")
    (previous / "compile_commands.json").write_text("stale", encoding="utf-8")

    translation_preparation.copy_preparation_stage(previous, current, remove_stale_compdb=True)

    assert (current / "src" / "sample.nolines.i").read_text(encoding="utf-8") == (
        "translation unit"
    )
    assert not (current / "src" / "sample.nolines.refoldmap.json").exists()
    assert (current / "src" / "keep.json").read_text(encoding="utf-8") == "other metadata"
    assert not (current / "compile_commands.json").exists()
