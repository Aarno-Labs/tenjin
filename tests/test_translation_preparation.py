import os

import translation_preparation


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
