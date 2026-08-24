import os
from pathlib import Path

import translation_preparation


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
