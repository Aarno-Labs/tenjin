from pathlib import Path
import shutil
import os
import platform

import pytest

from tenjin_pytest_helpers import (
    annotate_pytest_request_with_translation_notes,
    cached_git_clone_at_commit,
    clean_up_resultsdir,
    run_cargo_on_final,
    TenjinFixtures,
)
import translation_preparation
import translation
import hermetic


def suckless_sbase_git_clone() -> Path:
    return cached_git_clone_at_commit(
        "git://git.suckless.org/sbase", "004a51426e42d42150a746dc113ad86fb3fbed3c"
    )

    # clang libutil/mode.c libutil/eprintf.c libutil/parseoffset.c libutil/fshut.c uudecode.c -o uudecode.exe


def lua_5_4_0_immunant_git_clone() -> Path:
    return cached_git_clone_at_commit(
        "https://github.com/immunant/lua.git", "b13c3c5b9caed83d0543bbea9b0d4e637ba3340d"
    )


@pytest.mark.slow  # expected runtime: 30 s
def test_sbase_cal(
    tenjin_fixtures: TenjinFixtures,
):
    if platform.system() == "Darwin":
        pytest.skip("c2rust drops the `drawcal` function on macOS but not Linux (!)")
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = suckless_sbase_git_clone()

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Ensure it compiles and runs as expected
    srcfiles = ["libutil/fshut.c", "libutil/eprintf.c", "libutil/strtonum.c", "cal.c"]
    srcs = [(tmp_codebase / src).read_text(encoding="utf-8") for src in srcfiles]
    combined = "\n".join(srcs).replace('#include "../util.h"', '#include "util.h"')
    # Currently having one header multiple included (unguarded) interferes with
    # macro refolding (or, rather, the pre-refold consolidation step).
    combined = (
        combined.replace('#include "util.h"', '#include "util.keep.h"', count=1)
        .replace('#include "util.h"', "")
        .replace('#include "util.keep.h"', '#include "util.h"')
    )
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
        tenjin_fixtures.root,
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

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 540 seconds (~9 minutes)
def test_Old_Man_Programmer__tree_2_3_2(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/brk/Old-Man-Programmer__tree.git",
        "3f3077dbd87fc89396c8dc74fcf7920ec8b0c7d5",
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    buildcmd_args = ["make"]
    translation.do_translate(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="tenjinized",
        buildcmd=hermetic.shellize(buildcmd_args),
        guidance_path_or_literal="{}",
    )

    c_prog_output = hermetic.run(
        [str(tmp_resultsdir / "_build_1" / "tree"), "--version"], check=True, capture_output=True
    )
    assert (
        c_prog_output.stdout
        == b"tree v2.3.2 \xc2\xa9 1996 - 2026 by Steve Baker, Thomas Moore, Francesc Rocher, Florian Sesser, Kyosuke Tokoro\n"
    ), f"Got: {c_prog_output.stdout!r}"

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(
        tmp_resultsdir / "final", ["run", "--", "--version"], capture_output=True
    )

    assert rs_prog_output.stdout == c_prog_output.stdout, (
        f"Rust and C output differed; Rust output was: {rs_prog_output.stdout!r}"
    )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


# Expected runtime: 10 s
def test_url_h_aka_urlparser(
    tenjin_fixtures: TenjinFixtures,
):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/jwerle/url.h.git", "752635e46be6b13ad045f7216a28417fdf533950"
    )

    translation_preparation.copy_codebase(codebase, tmp_codebase)
    buildcmd_args = ["make", "url-test"]

    translation.do_translate(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="tenjinized",
        buildcmd=hermetic.shellize(buildcmd_args),
        guidance_path_or_literal="{}",
    )

    c_prog_output = hermetic.run(
        [str(tmp_resultsdir / "_build_1" / "url-test")], check=True, capture_output=True
    )

    assert (
        c_prog_output.stdout
        == b"""#url =>
    .protocol: "http"
    .host: "subdomain.host.com"
    .userinfo: "user:pass"
    .host: "subdomain.host.com"
    .port: "8080"
    .path: "/p/\xc3\xa5/t/h"
    .query[0]: "qu\xc3\xabry" -> "strin\xc4\x9f"
    .query[1]: "foo" -> "bar=yuk"
    .query[2]: "key#&=" -> "%"
    .query[3]: "lol" -> ""
    .fragment: "h\xc3\xa6sh"
#url =>
    .protocol: "git"
    .host: "github.com"
    .userinfo: "git"
    .host: "github.com"
    .port: (NULL)
    .path: "jwerle/url.h.git"
    .fragment: (NULL)
"""
    ), f"Got: {c_prog_output.stdout!r}"

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(
        tmp_resultsdir / "final", ["run", "--bin", "test"], capture_output=True
    )

    assert rs_prog_output.stdout == c_prog_output.stdout, (
        f"Rust and C output differed; Rust output was: {rs_prog_output.stdout!r}"
    )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 470 seconds (~8 minutes)
def test_lua_5_4_0_immunant(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
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
        tenjin_fixtures.root,
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

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


# g0 = empty guidance
@pytest.mark.slow  # expected runtime: 60 seconds
def test_ronomon_pure_cli_g0(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/brk/ronomon-pure.git", "242bb30df50610d73907de26495c5d1344888abe"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    with tenjin_fixtures.monkeypatch.context() as _m:
        # m.setenv("XJ_EXTRA_PREPARATION_PASSES", "1")
        translation.do_translate(
            tenjin_fixtures.root,
            tmp_codebase,
            tmp_resultsdir,
            cratename="tenjinized",
            buildcmd="make -f Makefile.pure_cli",
            guidance_path_or_literal="{}",
        )

    shutil.copytree(tmp_codebase / "tests", tmp_resultsdir / "final" / "tests")

    n_tests_passed = 0
    for zip_file_path in (tmp_resultsdir / "final" / "tests").glob("*.zip"):
        cp_c = hermetic.run(
            [str(tmp_resultsdir / "_build_1" / "pure_cli"), zip_file_path],
            check=False,
            capture_output=True,
            cwd=tmp_resultsdir / "final",
        )
        cp_rs = hermetic.run_cargo_on_translated_code(
            ["run", str(zip_file_path)],
            cwd=tmp_resultsdir / "final",
            capture_output=True,
            check=False,
        )
        assert cp_c.returncode == cp_rs.returncode, (
            f"Test vector {zip_file_path.stem} had different exit codes for C and Rust: {cp_c.returncode} vs {cp_rs.returncode}\nC stderr: {cp_c.stderr!r}\nRust stderr: {cp_rs.stderr!r}\n{zip_file_path}"
        )
        assert cp_rs.stdout == cp_c.stdout, (
            f"Rust and C output differed for {zip_file_path}; Rust output was: {cp_rs.stdout!r}"
        )
        n_tests_passed += 1

    print(f"ronomon_pure_cli passed {n_tests_passed} test vectors.")

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)
