from tenjin_pytest_helpers import (
    TenjinFixtures,
    annotate_pytest_request_with_translation_notes,
    run_cargo_on_final,
)
import translation
import translation_preparation


def single_file_check_translation(dir, filename, test_dir, tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    translation_preparation.copy_codebase(test_dir / dir / filename, tmp_codebase)
    translation.do_translate(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        cratename=dir,
        guidance_path_or_literal="{}",
    )

    assert (tmp_resultsdir / "final" / "Cargo.toml").exists()
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def test_errno_global(test_dir, tenjin_fixtures):
    single_file_check_translation("errno_global", "main.c", test_dir, tenjin_fixtures)


def test_time_coercion(test_dir, tenjin_fixtures):
    single_file_check_translation("time_coercion", "main.c", test_dir, tenjin_fixtures)
    rs_prog_output = run_cargo_on_final(
        tenjin_fixtures.tmp_resultsdir / "final", ["run"], capture_output=True
    )
    assert rs_prog_output.stdout == b"1\n"
