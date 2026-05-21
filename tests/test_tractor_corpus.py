import os
import json
import shutil
import resource
import platform
from pathlib import Path
from subprocess import SubprocessError

import pytest

from tenjin_pytest_helpers import (
    annotate_pytest_request_with_translation_notes,
    assert_final_had_no_unsafe_fns,
    cached_git_clone_at_commit,
    clean_up_resultsdir,
    run_cargo_on_final,
    run_tractor_test_vector,
    TenjinFixtures,
)
import translation_preparation
import translation


def tractor_tests_git_clone_for(case_dir: str) -> Path:
    if case_dir.startswith("Public-Tests/B02_"):
        # Currently Battery 2 requires authentication to access,
        # so the https URL won't work.
        return cached_git_clone_at_commit(
            "git@github.com:DARPA-TRACTOR-Program/Test-Corpus.git",
            "f4fa82f9472a1c5c0a6b8a42da0a262ccbb560ff",
        )
    return cached_git_clone_at_commit(
        "https://github.com/DARPA-TRACTOR-Program/PUBLIC-Test-Corpus.git",
        "6ec7ae65c906bffded2a24544825de4087bc2a61",
    )


def test_tractor_ta3_corpus_p0_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P00_perlin_noise/001_perlin_noise"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_005_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/005_sphincs_PQCgenKAT_sign_blake_128f_simple"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_010_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/010_sphincs_PQCgenKAT_sign_blake_192f_robust"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_016_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/016_sphincs_PQCgenKAT_sign_blake_256s_robust"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_019_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/019_sphincs_PQCgenKAT_sign_sha2_128s_simple"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_034_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/034_sphincs_PQCgenKAT_sign_shake_192f_robust"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_042_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/042_sphincs_PQCgenKAT_sign_haraka_128f_robust"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_053_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/053_blake_128f_s_initialize_hash_function_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_059_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/059_blake_128f_r_gen_message_random_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_066_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/066_blake_128s_r_prf_addr_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_088_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/088_blake_256f_s_hash_message_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_ta3_corpus_p01_132_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/P01_sphincs_plus/132_sha2_192s_r_hash_message_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


# P01 requires <openssl/conf.h> to compile and dynamically links against `libcrypto`.
# On Mac, we should query `brew --prefix openssl`.
# It also appears that setrlimit doesn't work properly on Mac, so anyone running this test
# would need to set `ulimit -s 32000` before invoking pytest.


def translate_and_build_ta3_test(
    fixtures: TenjinFixtures,
    cratename: str,
    guidance: str,
    orig_codebase: Path,
    resultsdir: Path,
    case_dir: str,
    profile: str,
):
    if (orig_codebase / case_dir / "CMakePresets.json").is_file():
        fixtures.monkeypatch.setenv(
            "XJ_CMAKE_PRESET", "test"
        )  # without this, we'll compile the wrong code
        assert not (orig_codebase / case_dir / "test_case" / "CMakePresets.json").is_file(), (
            f"Found unexpected CMakePresets.json in the test_case directory {orig_codebase / case_dir / 'test_case'}."
        )
        shutil.copyfile(
            orig_codebase / case_dir / "CMakePresets.json",
            fixtures.tmp_codebase / "test_case" / "CMakePresets.json",
        )

    translation.do_translate(
        fixtures.root,
        fixtures.tmp_codebase / "test_case",
        resultsdir,
        cratename,
        guidance_path_or_literal=guidance,
    )

    args = ["build"]
    if profile == "release":
        args.append("--release")
    run_cargo_on_final(resultsdir / "final", args)


def try_raise_stack_limit_if_needed(case_dir: str) -> bool:
    if "P01_sphincs_plus" in str(case_dir):
        # blake256.c has a very large function (~1600 statements) which causes c2rust to
        # blow the stack with the default 8MB limit.
        try:
            mb_32 = 32 * 1024 * 1024
            resource.setrlimit(resource.RLIMIT_STACK, (mb_32, mb_32))
        except ValueError:
            return False
    return True


def ta3_corpus_linux_only(case_dir: str) -> bool:
    # Several TA3 tests only build/link/run on Linux, not on Mac.
    if case_dir.startswith("Public-Tests/P01_sphincs_plus"):
        return True

    # Most of these have test vector differences.
    return case_dir in [
        "Public-Tests/B01_organic/hsv_to_rgb_lib",
        "Public-Tests/B01_organic/colourblind_lib",
        "Public-Tests/B01_organic/to_barycentric_lib",
        "Public-Tests/B01_organic/normalize_lib",
        "Public-Tests/B01_organic/contrast_ratio_lib",
        "Public-Tests/B01_organic/tfm_lib",
        "Public-Tests/B01_organic/007_errno_pow_lib",
        "Public-Tests/B01_organic/007_errno_pow",
        "Public-Tests/B02_organic/qmath",  # missing define on non-Linux
        "Public-Tests/B02_organic/spec_ray_lib",
        "Public-Tests/B02_organic/gen_ray_lib",
        "Public-Tests/P00_perlin_noise/001_perlin_noise",  # test 020 fails on mac
    ]


def eval_tractor_ta3_corpus_app(
    fixtures: TenjinFixtures,
    case_dir: str,
    guidance: str = "{}",
):
    if platform.system() != "Linux" and ta3_corpus_linux_only(case_dir):
        return pytest.skip(
            "P01 doesn't build on Mac, we don't yet translate it either.",
        )

    try:
        codebase = tractor_tests_git_clone_for(case_dir)
    except SubprocessError:
        return pytest.skip(
            f"Could not clone repo for {case_dir}; likely requires authentication. Skipping test."
        )

    if not try_raise_stack_limit_if_needed(case_dir):
        return pytest.skip(
            "P01 requires a large stack which we can't set up on this platform; skipping test.",
        )

    tmp_codebase = fixtures.tmp_codebase
    tmp_resultsdir = fixtures.tmp_resultsdir

    # Copying the whole Test-Corpus repo results in huge numbers of temporary files,
    # resulting in noticeable delays both for test steps and for post-test cleanups.
    translation_preparation.copy_codebase(codebase / case_dir, tmp_codebase)

    exe_name = "driver"  # Some test vectors require the binary to be named "driver".
    profile = "release"
    translate_and_build_ta3_test(
        fixtures,
        cratename="tractor_ta3_corpus_app",
        guidance=guidance,
        orig_codebase=codebase,
        resultsdir=tmp_resultsdir,
        case_dir=case_dir,
        profile=profile,
    )

    rs_bins = [
        p
        for p in (tmp_resultsdir / "final" / "target" / profile).iterdir()
        if p.is_file() and os.access(p, os.X_OK) and p.name == p.stem
    ]
    assert len(rs_bins) == 1, (
        f"Expected exactly one binary in {tmp_resultsdir / 'final' / 'target' / profile} but found: {[p.name for p in rs_bins]}"
    )

    # Some of TA3's tests require the binary be named `driver`, and in some build setups
    # we end up with a workspace member also called `driver`. To avoid clashes, we simply
    # put the binary in a fresh empty directory.
    freshdir = tmp_resultsdir / "final" / "_fresh_"
    freshdir.mkdir(exist_ok=False)
    rs_bin = freshdir / exe_name
    shutil.copy(rs_bins[0], rs_bin)

    if "P01_sphincs_plus" in str(case_dir):
        fixtures.monkeypatch.setenv(
            "XJ_LD_SYSROOT", "1"
        )  # Note: P01 dynamically links against libcrypto; this makes it available.

    vectors_run = 0
    vectors_skipped = 0
    for test_vector in (tmp_codebase / "test_vectors").glob("*.json"):
        # The various corpus tests are not consistent in where they
        # leave the built executable.
        exe_path = tmp_resultsdir / "_build_1" / "app" / exe_name
        if not exe_path.is_file():
            exe_path = tmp_resultsdir / "_build_1" / exe_name

        outcome_c = run_tractor_test_vector(
            exe_path,
            test_vector.stem,
            test_vector,
            cwd=tmp_resultsdir / "final",
        )
        if outcome_c.skipped:
            vectors_skipped += 1
            continue

        vectors_run += 1

        assert outcome_c.ok, (
            f"Test vector {test_vector.stem} failed on the C version: {outcome_c.message}\n{test_vector}"
        )

        outcome_rs = run_tractor_test_vector(
            rs_bin, test_vector.stem, test_vector, cwd=tmp_resultsdir / "final"
        )
        assert outcome_rs.ok, (
            f"Test vector {test_vector.stem} failed on the Rust version: {outcome_rs.message}\n{test_vector}"
        )

    # Clean up built artifacts for the runner to save disk space;
    clean_up_resultsdir(tmp_resultsdir)

    print(f"Ran {vectors_run} test vectors, skipped {vectors_skipped}.")
    annotate_pytest_request_with_translation_notes(fixtures)


def query_cargo_metadata_for_binary_names(cargo_toml_dir: Path) -> list[str]:
    result = run_cargo_on_final(
        cargo_toml_dir,
        ["metadata", "--format-version=1", "--no-deps"],
        capture_output=True,
    )
    metadata = json.loads(result.stdout.decode("utf-8"))
    return [
        bin["name"]
        for pkg in metadata["packages"]
        for bin in pkg.get("targets", [])
        if "bin" in bin["kind"]
    ]


def eval_tractor_ta3_corpus_lib(
    fixtures: TenjinFixtures,
    case_dir: str,
    guidance: str = "{}",
):
    try:
        codebase = tractor_tests_git_clone_for(case_dir)
    except SubprocessError:
        return pytest.skip(
            f"Could not clone repo for {case_dir}; likely requires authentication. Skipping test."
        )

    if not try_raise_stack_limit_if_needed(case_dir):
        return pytest.skip(
            "P01 requires a large stack which we can't set up on this platform; skipping test.",
        )

    tmp_codebase = fixtures.tmp_codebase
    tmp_resultsdir = fixtures.tmp_resultsdir

    # Copying the whole Test-Corpus repo results in huge numbers of temporary files,
    # resulting in noticeable delays both for test steps and for post-test cleanups.
    translation_preparation.copy_codebase(codebase / case_dir, tmp_codebase)

    # cando2 requires the candidate name (e.g. "collided_lib") end in `_lib`.
    candidate_name = Path(case_dir).name
    assert candidate_name.endswith("_lib"), (
        f"Expected case_dir name to end in '_lib', got {candidate_name}"
    )

    # cando2 requires our runner exist in a candidate-named directory.
    candidate_resultsdir = tmp_resultsdir / candidate_name
    candidate_resultsdir.mkdir(exist_ok=False)

    # Copy runner to results dir & build it
    translation_preparation.copy_codebase(
        codebase / case_dir / "runner", candidate_resultsdir / "runner"
    )

    runner_cargo_toml = candidate_resultsdir / "runner" / "Cargo.toml"
    runner_cargo_toml_contents = runner_cargo_toml.read_text(encoding="utf-8")
    runner_cargo_toml_contents = runner_cargo_toml_contents.replace(
        'cando2 = { path = "../../../../tools/cando2" }',
        f'cando2 = {{ path = "{codebase}/tools/cando2" }}',
    )
    runner_cargo_toml.write_text(runner_cargo_toml_contents, encoding="utf-8")

    run_cargo_on_final(candidate_resultsdir / "runner", ["build"])

    profile = "release"
    translate_and_build_ta3_test(
        fixtures,
        cratename="tractor_ta3_corpus_lib",
        guidance=guidance,
        orig_codebase=codebase,
        resultsdir=candidate_resultsdir,
        case_dir=case_dir,
        profile=profile,
    )

    # For single-library tests, cando2 usually requires the built library exist
    # in a `build-ninja` directory and be named `{candidate_name}.so`.
    # Sometimes they hardcode the name `driver.so`.
    # (B02_organic/encode_base64_lib is an example of the latter.)
    # Tests with multiple libraries keep library names as-is.
    build_ninja_dir = Path(candidate_resultsdir / "build-ninja")
    build_ninja_dir.mkdir(exist_ok=False)
    built_dir = candidate_resultsdir / "final" / "target" / profile
    built_libs = list(built_dir.glob("lib*.so")) + list(built_dir.glob("lib*.dylib"))
    for built_lib in built_libs:
        shutil.copyfile(built_lib, build_ninja_dir / f"{built_lib.name}")
        shutil.copyfile(built_lib, build_ninja_dir / f"lib{candidate_name}{built_lib.suffix}")
        shutil.copyfile(built_lib, build_ninja_dir / f"libdriver{built_lib.suffix}")

    if "P01_sphincs_plus" in str(case_dir):
        fixtures.monkeypatch.setenv(
            "XJ_LD_SYSROOT", "1"
        )  # Note: P01 dynamically links against libcrypto; this makes it available.

    run_cargo_on_final(
        candidate_resultsdir / "runner",
        ["build"] + (["--release"] if profile == "release" else []),
        capture_output=False,
    )

    bin_names = query_cargo_metadata_for_binary_names(candidate_resultsdir / "runner")
    assert len(bin_names) == 1, (
        f"Expected exactly one binary target in the runner's Cargo.toml, but found: {bin_names}"
    )

    for test_vector in (tmp_codebase / "test_vectors").glob("*.json"):
        outcome = run_tractor_test_vector(
            binary=candidate_resultsdir / "runner" / "target" / profile / bin_names[0],
            test_name=test_vector.stem,
            test_vector=test_vector,
            verbose=True,
            cwd=candidate_resultsdir / "runner",
        )
        assert outcome.ok, (
            f"Library test vector {test_vector.stem} failed: {outcome.message}\n{test_vector}"
        )

    # Clean up built artifacts for the runner to save disk space;
    clean_up_resultsdir(candidate_resultsdir)

    annotate_pytest_request_with_translation_notes(fixtures)


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗     ██╗    ██╗     ██╗██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ███║    ██║     ██║██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝     ╚██║    ██║     ██║██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝       ██║    ██║     ██║██╔══██╗╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║        ██║    ███████╗██║██████╔╝███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝        ╚═╝    ╚══════╝╚═╝╚═════╝ ╚══════╝


@pytest.mark.slow
def test_tractor_b1_organic_collided_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/collided_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs void pointer cast handling


@pytest.mark.slow
def test_tractor_b1_organic_bin2hex_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/bin2hex_lib"
    # guidance = """{
    #     "vars_of_type":{
    #         "Vec<uint8_t>":["bin2hex:bin"],
    #         "Vec<core::ffi::c_char>":["bin2hex:hex"]},
    #     "fn_return_type":{"bin2hex":"Vec<core::ffi::c_char>"},
    #     "vars_mut":{"bin2hex:*":true}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_bitwriter_add_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/bitwriter_add_lib"
    # guidance = """{"vars_of_type":{"&mut tflac_bitwriter":["bitwriter_add:bw"]}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_colourblind_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/colourblind_lib"
    # guidance = """{"vars_of_type":{ "&mut f32":
    #     ["colourblind:R", "colourblind:G", "colourblind:B", "*:Red", "*:Green", "*:Blue" ]}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_contrast_ratio_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/contrast_ratio_lib"
    # guidance = """{"no_math_errno":true}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_crc16_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/crc16_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs subscript refactoring


@pytest.mark.slow
def test_tractor_b1_organic_dequantize_granule_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/dequantize_granule_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs subscript refactoring


@pytest.mark.slow
def test_tractor_b1_organic_div_euclid_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/div_euclid_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "div_euclid_lib")


@pytest.mark.slow
def test_tractor_b1_organic_encode_quant_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/encode_quant_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "encode_quant_lib")


@pytest.mark.slow
def test_tractor_b1_organic_flac_validate_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/flac_validate_lib"
    # guidance = """{"vars_of_type":{"&mut tflac":["flac_validate:t"]}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_flip_horizontal_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/flip_horizontal_lib"
    # guidance = """{"vars_of_type":{"&mut cp_image_t":["flip_horizontal:img"], "&mut Vec<cp_pixel_t>":["*:pix"]}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs subscript refactoring


def test_tractor_b1_organic_float2half_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/float2half_lib"
    guidance = """{"vars_mut":{"m__shift":false,"m__base": false}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir, guidance)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "float2half_lib")


@pytest.mark.slow
def test_tractor_b1_organic_gaussian_kernel_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/gaussian_kernel_lib"
    # guidance = """'{"vars_of_type":{"&mut Vec<f32>":["gaussian_kernel:dest"]}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_half2float_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/half2float_lib"
    guidance = """{"vars_mut":{"m__mantissa":false,"m__offset": false,"m__exponent": false}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir, guidance)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "half2float_lib")


@pytest.mark.slow
def test_tractor_b1_organic_hdr_bitrate_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/hdr_bitrate_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_hdr_compare_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/hdr_compare_lib"
    # This is an interesting case; it doesn't compile with owned Vec
    # guidance = """{"vars_of_type":{"&Vec<u8>":["hdr_valid:h","hdr_compare:*"]}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_hex2bin_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/hex2bin_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs output parameter transformation


@pytest.mark.slow
def test_tractor_b1_organic_hsl_to_rgb_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/hsl_to_rgb_lib"
    # guidance = """{"vars_of_type":{
    #     "&mut Vec<f32>":["hsl_to_rgb:dest", "hsl_to_rgb:src"]}, "no_math_errno":true}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # NB. clippy identifies bug in the C code! (h < 120.0f && h < 180.0f)


@pytest.mark.slow
def test_tractor_b1_organic_hsv_to_rgb_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/hsv_to_rgb_lib"
    # guidance = """{"vars_of_type":{"&mut Vec<f32>":["hsv_to_rgb:dest", "hsv_to_rgb:src"]}, "no_math_errno":true}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_ima_parse_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/ima_parse_lib"
    # guidance = """{"vars_of_type":{"&mut ima_info":["ima_parse:info"]} }"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_ldexp_q2_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/ldexp_q2_lib"
    guidance = """{"vars_mut":{"ldexp_q2:g_expfrac":false}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir, guidance)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "ldexp_q2_lib")


@pytest.mark.slow
def test_tractor_b1_organic_max_size_frame_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/max_size_frame_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "max_size_frame_lib")


@pytest.mark.slow
def test_tractor_b1_organic_md5_digest_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/md5_digest_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_merge_sort_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/merge_sort_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # This one puns between pointer-to-element and pointer-to-array -- &p[i] == p + i


@pytest.mark.slow
def test_tractor_b1_organic_next_double_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/next_double_lib"
    # guidance = """{"vars_of_type":{"&mut cn_rnd_t":["*:rnd"]}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_normalize_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/normalize_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_pow43_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/pow43_lib"
    guidance = """{"vars_mut":{"g_pow43":false}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir, guidance)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "pow43_lib")


@pytest.mark.slow
def test_tractor_b1_organic_premultiply_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/premultiply_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_read_side_info_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/read_side_info_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_rev16_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/rev16_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "rev16_lib")


@pytest.mark.slow
def test_tractor_b1_organic_rgb_to_hsv_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/rgb_to_hsv_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_synth_pair_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/synth_pair_lib"
    # guidance = """{"vars_of_type":{"&mut Vec<mp3d_sample_t>":"*:pcm", "&Vec<f32>":"*:z"}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs subscript refactoring


@pytest.mark.slow
def test_tractor_b1_organic_tfm_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/tfm_lib"
    # guidance = """{"vars_of_type":{"&mut Vec<f32>":"*:dest", "&Vec<f32>":"*:src"}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs subscript refactoring


@pytest.mark.slow
def test_tractor_b1_organic_to_barycentric_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/to_barycentric_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    assert_final_had_no_unsafe_fns(tenjin_fixtures.tmp_resultsdir / "to_barycentric_lib")


@pytest.mark.slow
def test_tractor_b1_organic_tritanopia_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/tritanopia_lib"
    # guidance = """{"vars_of_type":{"&mut f32":["Tritanopia:Red","Tritanopia:Green","Tritanopia:Blue"]},"no_math_errno":true}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_update_frame_header_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/update_frame_header_lib"
    # guidance = """{"vars_of_type":{"&mut tflac":["update_frame_header:t"]}}"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_update_md5_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/update_md5_lib"
    # guidance = """{"vars_of_type":{
    #     "&mut tflac":"update_md5:t",
    #     "& Vec<tflac_s32>":"update_md5:samples",
    #     "&mut Vec<u8>":"tflac_pack_u64le",
    #     "&mut tflac_md5":["tflac_md5_addsample:m"]} }"""
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs subscript refactoring


@pytest.mark.slow
def test_tractor_b1_organic_wcscat_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_organic/wcscat_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)
    # needs subscript refactoring


@pytest.mark.slow
def test_tractor_b1_organic_compress_bc5_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/compress_bc5_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_convex_clip_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/convex_clip_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_decorrelate_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/decorrelate_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_ima_decode_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/ima_decode_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


# This one fails with stacks of < 4 MB.
@pytest.mark.slow
def test_tractor_b1_organic_md5_transform_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/md5_transform_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_png_qsort_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/png_qsort_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_predict_sample_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/predict_sample_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_read_scalefactors_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/read_scalefactors_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_refine_block_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/refine_block_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_stereo_samples_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_organic/stereo_samples_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗     ██╗     █████╗ ██████╗ ██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ███║    ██╔══██╗██╔══██╗██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝     ╚██║    ███████║██████╔╝██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝       ██║    ██╔══██║██╔═══╝ ██╔═══╝ ╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║        ██║    ██║  ██║██║     ██║     ███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝        ╚═╝    ╚═╝  ╚═╝╚═╝     ╚═╝     ╚══════╝


@pytest.mark.slow
def test_tractor_b1_synthetic_002_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/002_stdin_echo"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_003_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/003_string_slicing"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_004_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/004_nineality_sieve"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_005_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/005_static_loop"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_006_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/006_static_alias"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_007_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/007_errno_pow"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


# omitting 008_long_run for now


@pytest.mark.slow
def test_tractor_b1_synthetic_009_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/009_stack_buffer_overflow"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_010_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/010_integer_overflow"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_011_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/011_uninit_char_ptr"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_012_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/012_uninit_int_ptr"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_013_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/013_poor_quality_addition"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_014_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/014_dead_code"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_015_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/015_return_stack_buffer"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_016_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/016_divide_by_zero_float"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_017_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/017_signed_length_confusion"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_018_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/018_stack_buffer_overflow_loop1"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_019_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/019_integer_overflow_char_max_multiply"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_021_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/021_complex_goto"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_022_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/022_stdlib_div"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_023_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/023_struct_and_errno"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_024_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/024_struct_and_static"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_025_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/025_struct_and_errno_and_static"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_026_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/026_goto_and_static"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_027_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/027_ctype_ascii"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_028_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/028_strchr"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


def test_tractor_b1_synthetic_029_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/029_strcspn"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_030_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/030_mutable_buffer_overlap_extrahard"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_031_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/031_disjoint_arrays"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_032_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/032_comma_operator"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.skip(reason="need to pull in fixes for bitfields from upstream c2rust")
@pytest.mark.slow
def test_tractor_b1_synthetic_033_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/033_bitfield"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_034_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/034_cast_to_char_ptr_int"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_035_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/035_cast_to_char_ptr_float"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_036_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/036_cast_to_char_ptr_struct"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_037_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/037_cast_to_char_ptr_int_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_038_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/038_cast_to_char_ptr_float_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_039_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/039_cast_to_char_ptr_struct_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_040_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/040_storage_class_auto"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_041_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/041_storage_class_register"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.skip(reason="we print hex floats without an explicit + sign")
@pytest.mark.slow
def test_tractor_b1_synthetic_042_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/042_float_union"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_043_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B01_synthetic/043_iso646_and_digraphs"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_002_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/002_echo"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_004_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/004_loop"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_011_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/011_static_dag"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_014_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/014_errno-pow-subfunction"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_016_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/016_switch-arith"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_027_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/027_stack_buffer_overflow_loop2"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_030_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/030_integer_underflow_char_min_multiply"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_045_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/045_strtok"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_048_app_hidden(tenjin_fixtures: TenjinFixtures):
    case_dir = "Hidden-Tests/B01_synthetic/048_mutable_buffer_overlap2"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗    ██████╗     ██╗     ██╗██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ╚════██╗    ██║     ██║██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝      █████╔╝    ██║     ██║██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝      ██╔═══╝     ██║     ██║██╔══██╗╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║       ███████╗    ███████╗██║██████╔╝███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝       ╚══════╝    ╚══════╝╚═╝╚═════╝ ╚══════╝


@pytest.mark.slow
def test_tractor_b2_synthetic_arity_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/arity_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_arrayfunc_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/arrayfunc_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_betagamma_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/betagamma_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_buffapp_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/buffapp_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_charinbuf_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/charinbuf_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_checkshift_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/checkshift_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_cleanup_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/cleanup_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_complexmode_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/complexmode_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_dataentry_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/dataentry_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_doubleneg_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/doubleneg_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_envy_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/envy_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_fallcalc_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/fallcalc_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_findrep_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/findrep_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_goto_lib(tenjin_fixtures: TenjinFixtures):
    pytest.xfail(reason="known bug with stdout buffer flush behavior across cdylib boundary")
    case_dir = "Public-Tests/B02_synthetic/goto_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_gotomach_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/gotomach_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_hatch_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/hatch_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_inreftree_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/inreftree_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_jumpnode_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/jumpnode_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_mathop_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/mathop_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_matrix_mult_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/matrix_mult_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_matrix_sum_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/matrix_sum_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_maxnmin_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/maxnmin_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_memchra2_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/memchra2_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_modeselect_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/modeselect_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_overunder_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/overunder_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_task_manager_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/task_manager_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_aabb_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/aabb_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_agglom_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/agglom_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_arr_del_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/arr_del_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_arr_ins_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/arr_ins_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_arr_push_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/arr_push_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_basename_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/basename_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_call_predict_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/call_predict_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_capsule_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/capsule_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_circle_collide_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/circle_collide_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_convert_pix_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/convert_pix_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_cJSON_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/cJSON_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_decode_base64_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/decode_base64_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_encode_base64_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/encode_base64_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_file_queue_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/file_queue_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_gen_ray_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/gen_ray_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_get_predict_func_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/get_predict_func_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_gjk_cache_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/gjk_cache_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_gjk_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/gjk_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_hm_geti_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/hm_geti_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_intput_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/intput_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_lines_in_buffer_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/lines_in_buffer_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_load_png_mem_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/load_png_mem_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_omni_collide_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/omni_collide_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_omni_manifold_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/omni_manifold_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_parse_number_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/parse_number_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_parse_uname_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/parse_uname_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_pinflate_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/pinflate_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_poly_ray_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/poly_ray_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_rdg_genstdout_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/rdg_genstdout_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_reverse_collide_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/reverse_collide_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_search_and_replace_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/search_and_replace_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_siphash_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/siphash_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_spec_ray_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/spec_ray_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_underhanded_c_nuke_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/underhanded_c_nuke_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_unfilter_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/unfilter_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_utf8_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/utf8_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗    ██████╗     █████╗ ██████╗ ██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ╚════██╗   ██╔══██╗██╔══██╗██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝      █████╔╝   ███████║██████╔╝██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝      ██╔═══╝    ██╔══██║██╔═══╝ ██╔═══╝ ╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║       ███████╗   ██║  ██║██║     ██║     ███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝       ╚══════╝   ╚═╝  ╚═╝╚═╝     ╚═╝     ╚══════╝


@pytest.mark.slow
def test_tractor_b2_synthetic_char_to_bool_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/char_to_bool"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_container_of_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/container_of"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_confusion_lib(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/confusion_lib"
    eval_tractor_ta3_corpus_lib(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_generic_foreach_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/generic_foreach"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_hashmap_tree_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/hashmap_tree"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_macrodepth_add_5_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/macrodepth_add_5"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_macrodepth_mul_4_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/macrodepth_mul_4"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_macrodepth_sub_6_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/macrodepth_sub_6"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_memcpy_fun_buffers_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/memcpy_fun_buffers"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_memmove_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/memmove"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_mutable_duplication_dag_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/mutable_duplication_dag"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_pointer_comparison_ascii_art_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/pointer_comparison_ascii_art"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_strcmp_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/strcmp"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_synthetic_strcpy_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_synthetic/strcpy"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_qmath_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/qmath"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)


@pytest.mark.slow
def test_tractor_b2_organic_underhanded_c_luggage_app(tenjin_fixtures: TenjinFixtures):
    case_dir = "Public-Tests/B02_organic/underhanded_c_luggage"
    eval_tractor_ta3_corpus_app(tenjin_fixtures, case_dir)
