import json
from pathlib import Path

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

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    buildcmd_args = [
        "cc",
        f"{case_dir}/test_case/src/main.c",
        "-o",
        "app.exe",
    ]

    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="b1_synthetic_022",
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
    rs_bin = rs_bins[0]

    vectors_run = 0
    vectors_skipped = 0
    for test_vector in (codebase / case_dir / "test_vectors").glob("*.json"):
        spec = json.loads(test_vector.read_text(encoding="utf-8"))
        outcome_c = run_tractor_test_vector(
            tmp_resultsdir / "_build_1" / "app.exe",
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


@pytest.mark.slow
def test_tractor_b1_synthetic_022_app(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    extras: list,
):
    case_dir = "Public-Tests/B01_synthetic/022_stdlib_div"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, extras, case_dir)
