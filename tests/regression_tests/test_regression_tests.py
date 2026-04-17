from tenjin_pytest_helpers import annotate_pytest_request_with_translation_notes
import translation
import translation_preparation


def test_errno_global(root, test_dir, tmp_codebase, tmp_resultsdir, extras, request):
    codebase = test_dir / "errno_global" / "main.c"

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Run translation
    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="errno_global",
        guidance_path_or_literal="{}",
    )

    assert (tmp_resultsdir / "final" / "Cargo.toml").exists()

    annotate_pytest_request_with_translation_notes(request, tmp_resultsdir, extras)
