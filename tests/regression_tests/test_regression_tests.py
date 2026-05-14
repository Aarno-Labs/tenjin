from tenjin_pytest_helpers import (
    TenjinFixtures,
    annotate_pytest_request_with_translation_notes,
    assert_final_had_no_unsafe_fns,
    clean_up_resultsdir,
    run_cargo_on_final,
)
import translation
import translation_preparation


def single_file_check_translation(
    dir, filename, test_dir, tenjin_fixtures: TenjinFixtures, guidance: str = "{}"
):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    translation_preparation.copy_codebase(test_dir / dir / filename, tmp_codebase)
    translation.do_translate(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        cratename=dir,
        guidance_path_or_literal=guidance,
    )

    assert (tmp_resultsdir / "final" / "Cargo.toml").exists()
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def test_errno_global(test_dir, tenjin_fixtures):
    single_file_check_translation("errno_global", "main.c", test_dir, tenjin_fixtures)


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
