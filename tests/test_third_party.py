import json
from pathlib import Path
import shutil

import pytest

from tenjin_pytest_helpers import (
    annotate_pytest_request_with_translation_notes,
    cached_git_clone_at_commit,
    run_cargo_on_final,
    run_tractor_test_vector,
)
import translation_preparation
import translation
import hermetic
import os


def suckless_sbase_git_clone() -> Path:
    return cached_git_clone_at_commit(
        "git://git.suckless.org/sbase", "004a51426e42d42150a746dc113ad86fb3fbed3c"
    )

    # clang libutil/mode.c libutil/eprintf.c libutil/parseoffset.c libutil/fshut.c uudecode.c -o uudecode.exe


def lua_5_4_0_immunant_git_clone() -> Path:
    return cached_git_clone_at_commit(
        "https://github.com/immunant/lua.git", "b13c3c5b9caed83d0543bbea9b0d4e637ba3340d"
    )


def tractor_public_tests_git_clone() -> Path:
    return cached_git_clone_at_commit(
        "https://github.com/DARPA-TRACTOR-Program/PUBLIC-Test-Corpus.git",
        "6ec7ae65c906bffded2a24544825de4087bc2a61",
    )


@pytest.mark.slow
def test_sbase_cal(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    extras: list,
):
    codebase = suckless_sbase_git_clone()

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Ensure it compiles and runs as expected
    srcfiles = ["libutil/fshut.c", "libutil/eprintf.c", "libutil/strtonum.c", "cal.c"]
    srcs = [(tmp_codebase / src).read_text(encoding="utf-8") for src in srcfiles]
    combined = "\n".join(srcs).replace('#include "../util.h"', '#include "util.h"')
    (tmp_codebase / "cal_combined.c").write_text(combined, encoding="utf-8")
    # Note: if we try compiling all C files via the driver we hit
    #           https://github.com/Aarno-Labs/tenjin/issues/213
    # Note: if we try compiling the libutil files to object files "opaquely",
    #       we encounter an issue with localization-of-globals because
    #       cal.c accesses an extern global which is defined in one of the libutil files,
    #       and we wrongly generate accesses for the global through XjGlobals without
    #       actually having that global as a field.
    buildcmd_args = [
        "cc",
        "cal_combined.c",
        "-o",
        "cal.exe",
    ]
    hermetic.run(buildcmd_args, cwd=str(tmp_codebase), check=True)
    c_prog_output = hermetic.run(
        [str(tmp_codebase / "cal.exe"), "2024"], check=True, capture_output=True
    )
    assert (
        c_prog_output.stdout
        == b"""    January 2024           February 2024           March 2024        
Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   
    1  2  3  4  5  6                1  2  3                   1  2   
 7  8  9 10 11 12 13    4  5  6  7  8  9 10    3  4  5  6  7  8  9   
14 15 16 17 18 19 20   11 12 13 14 15 16 17   10 11 12 13 14 15 16   
21 22 23 24 25 26 27   18 19 20 21 22 23 24   17 18 19 20 21 22 23   
28 29 30 31            25 26 27 28 29         24 25 26 27 28 29 30   
                                              31                     
     April 2024              May 2024               June 2024        
Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   
    1  2  3  4  5  6             1  2  3  4                      1   
 7  8  9 10 11 12 13    5  6  7  8  9 10 11    2  3  4  5  6  7  8   
14 15 16 17 18 19 20   12 13 14 15 16 17 18    9 10 11 12 13 14 15   
21 22 23 24 25 26 27   19 20 21 22 23 24 25   16 17 18 19 20 21 22   
28 29 30               26 27 28 29 30 31      23 24 25 26 27 28 29   
                                              30                     
      July 2024             August 2024          September 2024      
Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   
    1  2  3  4  5  6                1  2  3    1  2  3  4  5  6  7   
 7  8  9 10 11 12 13    4  5  6  7  8  9 10    8  9 10 11 12 13 14   
14 15 16 17 18 19 20   11 12 13 14 15 16 17   15 16 17 18 19 20 21   
21 22 23 24 25 26 27   18 19 20 21 22 23 24   22 23 24 25 26 27 28   
28 29 30 31            25 26 27 28 29 30 31   29 30                  
                                                                     
    October 2024           November 2024          December 2024      
Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   Su Mo Tu We Th Fr Sa   
       1  2  3  4  5                   1  2    1  2  3  4  5  6  7   
 6  7  8  9 10 11 12    3  4  5  6  7  8  9    8  9 10 11 12 13 14   
13 14 15 16 17 18 19   10 11 12 13 14 15 16   15 16 17 18 19 20 21   
20 21 22 23 24 25 26   17 18 19 20 21 22 23   22 23 24 25 26 27 28   
27 28 29 30 31         24 25 26 27 28 29 30   29 30 31               
                                                                     
"""  # noqa: W291, W293
    ), f"Got: {c_prog_output.stdout!r}"

    # Run translation
    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="sbase_cal",
        buildcmd=hermetic.shellize(buildcmd_args),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(
        tmp_resultsdir / "final", ["run", "2024"], capture_output=True
    )

    assert rs_prog_output.stdout == c_prog_output.stdout, (
        f"Rust and C output differed; Rust output was: {rs_prog_output.stdout!r}"
    )

    annotate_pytest_request_with_translation_notes(request, tmp_resultsdir, extras)


@pytest.mark.slow  # expected runtime: 470 seconds (~8 minutes)
def test_lua_5_4_0_immunant(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    extras: list,
):
    codebase = lua_5_4_0_immunant_git_clone()

    translation_preparation.copy_codebase(codebase, tmp_codebase)
    buildcmd_args = [
        "make",
        "-j3",
        "MYCFLAGS=-std=c99 -DLUA_USE_POSIX -DLUA_USE_JUMPTABLE=0",
        "CC=clang",
        "MYLIBS=-ldl",
        "lua",
    ]

    # cclyzer takes 7+ hours to analyze Lua, ain't nobody got time for that.
    os.environ["XJ_SKIP_CCLYZERPP"] = "1"
    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="tenjinized",
        buildcmd=hermetic.shellize(buildcmd_args),
        guidance_path_or_literal="{}",
    )
    del os.environ["XJ_SKIP_CCLYZERPP"]

    c_prog_output = hermetic.run(
        [str(tmp_resultsdir / "_build_1" / "lua"), "-v"], check=True, capture_output=True
    )
    assert c_prog_output.stdout == b"Lua 5.4.0  Copyright (C) 1994-2019 Lua.org, PUC-Rio\n", (
        f"Got: {c_prog_output.stdout!r}"
    )

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(
        tmp_resultsdir / "final", ["run", "--", "-v"], capture_output=True
    )

    assert rs_prog_output.stdout == c_prog_output.stdout, (
        f"Rust and C output differed; Rust output was: {rs_prog_output.stdout!r}"
    )

    annotate_pytest_request_with_translation_notes(request, tmp_resultsdir, extras)


def eval_tractor_ta3_corpus_app(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    extras: list,
    case_dir: str,
):
    codebase = tractor_public_tests_git_clone()

    # Copying the whole Test-Corpus repo results in huge numbers of temporary files,
    # resulting in noticeable delays both for test steps and for post-test cleanups.
    translation_preparation.copy_codebase(codebase / case_dir, tmp_codebase)

    exe_name = "driver"  # Some test vectors require the binary to be named "driver".
    buildcmd_args = [
        "cc",
        "test_case/src/main.c",
        "-lm",
        "-o",
        exe_name,
    ]

    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="tractor_ta3_corpus_app",
        buildcmd=hermetic.shellize(buildcmd_args),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    # The binary may be `main` or `main_nolines`
    rs_bins = list((tmp_resultsdir / "final" / "target" / "debug").glob("main*"))
    rs_bins = [p for p in rs_bins if p.is_file() and os.access(p, os.X_OK)]
    assert len(rs_bins) == 1, (
        f"Expected exactly one binary in {tmp_resultsdir / 'final' / 'target' / 'debug'}, but found: {[p.name for p in rs_bins]}"
    )

    # Some of TA3's tests require the binary be named `driver`, and in some build setups
    # we end up with a workspace member also called `driver`. To avoid clashes, we simply
    # put the binary in a fresh empty directory.
    freshdir = tmp_resultsdir / "final" / "_fresh_"
    freshdir.mkdir(exist_ok=False)
    rs_bin = freshdir / exe_name
    shutil.copy(rs_bins[0], rs_bin)

    vectors_run = 0
    vectors_skipped = 0
    for test_vector in (tmp_codebase / "test_vectors").glob("*.json"):
        spec = json.loads(test_vector.read_text(encoding="utf-8"))
        outcome_c = run_tractor_test_vector(
            tmp_resultsdir / "_build_1" / exe_name,
            test_vector.stem,
            spec,
            cwd=tmp_resultsdir / "final",
        )
        if outcome_c.skipped:
            vectors_skipped += 1
            continue

        vectors_run += 1

        assert outcome_c.ok, (
            f"Test vector {test_vector.stem} failed on the C version: {outcome_c.message}"
        )

        outcome_rs = run_tractor_test_vector(
            rs_bin, test_vector.stem, spec, cwd=tmp_resultsdir / "final"
        )
        assert outcome_rs.ok, (
            f"Test vector {test_vector.stem} failed on the Rust version: {outcome_rs.message}"
        )

    print(f"Ran {vectors_run} test vectors, skipped {vectors_skipped}.")
    annotate_pytest_request_with_translation_notes(request, tmp_resultsdir, extras)


def eval_tractor_ta3_corpus_lib(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    extras: list,
    case_dir: str,
):
    # cando2 requires the candidate name (e.g. "collided_lib") end in `_lib`.
    candidate_name = Path(case_dir).name
    assert candidate_name.endswith("_lib"), (
        f"Expected case_dir name to end in '_lib', got {candidate_name}"
    )
    candidate_stem = candidate_name[: -len("_lib")]

    codebase = tractor_public_tests_git_clone()

    # Copying the whole Test-Corpus repo results in huge numbers of temporary files,
    # resulting in noticeable delays both for test steps and for post-test cleanups.
    translation_preparation.copy_codebase(codebase / case_dir, tmp_codebase)

    buildcmd_args = [
        "cc",
        "-I",
        "test_case/include",
        "test_case/src/lib.c",
        "-shared",
        "-o",
        f"lib{candidate_stem}.so",
    ]

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

    translation.do_translate(
        root,
        tmp_codebase,
        candidate_resultsdir,
        cratename="tractor_ta3_corpus_lib",
        buildcmd=hermetic.shellize(buildcmd_args),
        guidance_path_or_literal="{}",
    )

    run_cargo_on_final(candidate_resultsdir / "final", ["build"])

    # cando2 requires the built library exist in a `build-ninja` directory
    # and be named `{candidate_name}.so`.
    build_ninja_dir = Path(candidate_resultsdir / "build-ninja")
    build_ninja_dir.mkdir(exist_ok=False)
    built_libs = list((candidate_resultsdir / "final" / "target" / "debug").glob("lib*.so"))
    assert len(built_libs) == 1, (
        f"Expected exactly one built library in {candidate_resultsdir / 'final' / 'target' / 'debug'}, but found: {[p.name for p in built_libs]}"
    )
    built_lib = built_libs[0]
    shutil.copyfile(built_lib, build_ninja_dir / f"lib{candidate_name}.so")

    # shutil.copytree(candidate_resultsdir, "/home/brk/ta3_lib_/" + candidate_name)

    for test_vector in (tmp_codebase / "test_vectors").glob("*.json"):
        spec = json.loads(test_vector.read_text(encoding="utf-8"))
        if spec.get("has_ub") is not None:
            print(f"Skipping test vector {test_vector.stem} because it has UB")
            continue

        cp = run_cargo_on_final(
            candidate_resultsdir / "runner",
            ["-q", "run", "lib", "-c", str(test_vector)],
            capture_output=False,
        )
        cp.check_returncode()

    annotate_pytest_request_with_translation_notes(request, tmp_resultsdir, extras)


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗     ██╗    ██╗     ██╗██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ███║    ██║     ██║██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝     ╚██║    ██║     ██║██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝       ██║    ██║     ██║██╔══██╗╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║        ██║    ███████╗██║██████╔╝███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝        ╚═╝    ╚══════╝╚═╝╚═════╝ ╚══════╝


@pytest.mark.slow
def test_tractor_b1_organic_collided_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/collided_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_bin2hex_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/bin2hex_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_bitwriter_add_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/bitwriter_add_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_colourblind_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/colourblind_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_contrast_ratio_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/contrast_ratio_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_crc16_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/crc16_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_dequantize_granule_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/dequantize_granule_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_div_euclid_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/div_euclid_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_encode_quant_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/encode_quant_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_flac_validate_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/flac_validate_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_flip_horizontal_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/flip_horizontal_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_float2half_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/float2half_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_gaussian_kernel_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/gaussian_kernel_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_half2float_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/half2float_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_hdr_bitrate_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hdr_bitrate_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_hdr_compare_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hdr_compare_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_hex2bin_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hex2bin_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_hsl_to_rgb_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hsl_to_rgb_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_hsv_to_rgb_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hsv_to_rgb_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_ima_parse_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/ima_parse_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_ldexp_q2_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/ldexp_q2_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_max_size_frame_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/max_size_frame_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_md5_digest_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/md5_digest_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_merge_sort_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/merge_sort_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_next_double_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/next_double_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_normalize_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/normalize_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_pow43_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/pow43_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_premultiply_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/premultiply_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_read_side_info_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/read_side_info_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_rev16_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/rev16_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_rgb_to_hsv_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/rgb_to_hsv_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_synth_pair_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/synth_pair_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_tfm_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/tfm_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_to_barycentric_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/to_barycentric_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_tritanopia_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/tritanopia_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_update_frame_header_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/update_frame_header_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_update_md5_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/update_md5_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_wcscat_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/wcscat_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_compress_bc5_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/compress_bc5_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_convex_clip_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/convex_clip_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_decorrelate_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/decorrelate_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_ima_decode_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/ima_decode_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_md5_transform_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/md5_transform_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_png_qsort_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/png_qsort_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_predict_sample_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/predict_sample_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_read_scalefactors_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/read_scalefactors_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_refine_block_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/refine_block_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_organic_stereo_samples_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/stereo_samples_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗     ██╗     █████╗ ██████╗ ██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ███║    ██╔══██╗██╔══██╗██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝     ╚██║    ███████║██████╔╝██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝       ██║    ██╔══██║██╔═══╝ ██╔═══╝ ╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║        ██║    ██║  ██║██║     ██║     ███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝        ╚═╝    ╚═╝  ╚═╝╚═╝     ╚═╝     ╚══════╝


@pytest.mark.slow
def test_tractor_b1_synthetic_002_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/002_stdin_echo"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_003_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/003_string_slicing"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_004_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/004_nineality_sieve"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_005_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/005_static_loop"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_006_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/006_static_alias"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_007_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/007_errno_pow"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


# omitting 008_long_run for now


@pytest.mark.slow
def test_tractor_b1_synthetic_009_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/009_stack_buffer_overflow"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_010_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/010_integer_overflow"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_011_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/011_uninit_char_ptr"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_012_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/012_uninit_int_ptr"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_013_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/013_poor_quality_addition"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_014_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/014_dead_code"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_015_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/015_return_stack_buffer"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_016_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/016_divide_by_zero_float"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_017_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/017_signed_length_confusion"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_018_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/018_stack_buffer_overflow_loop1"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_019_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/019_integer_overflow_char_max_multiply"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_021_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/021_complex_goto"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_022_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/022_stdlib_div"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_023_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/023_struct_and_errno"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_024_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/024_struct_and_static"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_025_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/025_struct_and_errno_and_static"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_026_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/026_goto_and_static"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_027_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/027_ctype_ascii"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_028_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/028_strchr"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_029_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/029_strcspn"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_030_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/030_mutable_buffer_overlap_extrahard"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_031_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/031_disjoint_arrays"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_032_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/032_comma_operator"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.skip(reason="need to pull in fixes for bitfields from upstream c2rust")
@pytest.mark.slow
def test_tractor_b1_synthetic_033_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/033_bitfield"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_034_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/034_cast_to_char_ptr_int"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_035_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/035_cast_to_char_ptr_float"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_036_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/036_cast_to_char_ptr_struct"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_037_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/037_cast_to_char_ptr_int_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_038_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/038_cast_to_char_ptr_float_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_039_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/039_cast_to_char_ptr_struct_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_040_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/040_storage_class_auto"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_041_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/041_storage_class_register"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.skip(reason="we print hex floats without an explicit + sign")
@pytest.mark.slow
def test_tractor_b1_synthetic_042_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/042_float_union"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)


@pytest.mark.slow
def test_tractor_b1_synthetic_043_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/043_iso646_and_digraphs"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)
