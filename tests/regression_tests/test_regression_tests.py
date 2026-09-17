import pytest
import platform

from tenjin_pytest_helpers import (
    TenjinFixtures,
    annotate_pytest_request_with_translation_notes,
    assert_final_had_no_unsafe_fns,
    clean_up_resultsdir,
    run_cargo_on_final,
)
import translation
import translation_preparation
from translation_types import TranslationFlags


def single_file_check_translation(
    dir, filename, test_dir, tenjin_fixtures: TenjinFixtures, guidance: str = "{}"
):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    translation_preparation.copy_codebase(test_dir / dir / filename, tmp_codebase)
    translation.do_translate(
        TranslationFlags.simple(
            tenjin_fixtures.root,
            tmp_codebase,
            tmp_resultsdir,
            cratename=dir,
        ),
        guidance_path_or_literal=guidance,
    )

    assert (tmp_resultsdir / "final" / "Cargo.toml").exists()
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def test_errno_global(test_dir, tenjin_fixtures):
    single_file_check_translation("errno_global", "main.c", test_dir, tenjin_fixtures)


def test_float_classification_macros(test_dir, tenjin_fixtures):
    if platform.system() == "Darwin":
        return pytest.skip("mac currently exposes a refolder bug")
    single_file_check_translation("isnormal", "main.c", test_dir, tenjin_fixtures)
    rs_prog = run_cargo_on_final(
        tenjin_fixtures.tmp_resultsdir / "final", ["run"], capture_output=True
    )
    assert rs_prog.returncode == 0


def test_time_coercion_unguided(test_dir, tenjin_fixtures):
    single_file_check_translation("time_coercion", "main.c", test_dir, tenjin_fixtures)
    rs_prog_output = run_cargo_on_final(
        tenjin_fixtures.tmp_resultsdir / "final", ["run"], capture_output=True
    )
    # We'll generate
    # pub unsafe extern "C" fn wrap_time(mut tloc: *mut time_t) -> time_t {
    #     return xj_ctime::compat::time(tloc.as_mut());
    # }

    assert rs_prog_output.stdout == b"1\n"


def test_time_coercion_guided(test_dir, tenjin_fixtures):
    guidance = """
        { "vars_of_type": { "&mut time_t": ["wrap_time:tloc"] } }
    """
    single_file_check_translation("time_coercion", "main.c", test_dir, tenjin_fixtures, guidance)
    rs_prog_output = run_cargo_on_final(
        tenjin_fixtures.tmp_resultsdir / "final", ["run"], capture_output=True
    )
    assert rs_prog_output.stdout == b"1\n"
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir)
    # We'll generate
    # pub extern "C" fn wrap_time(mut tloc: &mut time_t) -> time_t {
    #     return xj_ctime::compat::time(Some(tloc));
    # }


def test_weak_symbol_override(test_dir, tenjin_fixtures):
    """A `__attribute__((weak))` definition overridden by a strong definition in
    another translation unit of the same target must not produce two competing
    `#[no_mangle]` definitions in the generated crate."""
    tmp_resultsdir = tenjin_fixtures.tmp_resultsdir
    translation.do_translate(
        TranslationFlags.simple(
            tenjin_fixtures.root,
            test_dir / "weak_symbol",
            tmp_resultsdir,
            cratename="weak_symbol",
            buildcmd="make",
        ),
        guidance_path_or_literal="{}",
    )

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(tmp_resultsdir / "final", ["run"], capture_output=True)
    assert rs_prog_output.stdout == b"dispatch() = 149\n"

    # The executable and the shared library each get their own copy of the
    # sources; keep this test covering that arrangement, since it is where the
    # bug was reported.
    xjdup_files = sorted(p.name for p in (tmp_resultsdir / "final").rglob("weakdef_xjdup_*.rs"))
    assert xjdup_files == ["weakdef_xjdup_0.rs", "weakdef_xjdup_1.rs"], xjdup_files

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)
