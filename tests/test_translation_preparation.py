import os
import json
from pathlib import Path

import c_refact
import targets
import translation_preparation
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
