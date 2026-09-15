import os
from pathlib import Path

import c_refact
import translation_preparation


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
