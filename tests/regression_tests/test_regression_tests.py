from tenjin_pytest_helpers import annotate_pytest_request_with_translation_notes, run_cargo_on_final
import translation
import translation_preparation


def single_file_check_translation(
    dir, filename, root, test_dir, tmp_codebase, tmp_resultsdir, extras, request
):
    codebase = test_dir / dir / filename

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Run translation
    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename=dir,
        guidance_path_or_literal="{}",
    )

    assert (tmp_resultsdir / "final" / "Cargo.toml").exists()
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    annotate_pytest_request_with_translation_notes(request, tmp_resultsdir, extras)


def test_errno_global(root, test_dir, tmp_codebase, tmp_resultsdir, extras, request):
    single_file_check_translation(
        "errno_global", "main.c", root, test_dir, tmp_codebase, tmp_resultsdir, extras, request
    )
