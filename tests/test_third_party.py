from pathlib import Path
import shutil
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
import translation_types
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


@pytest.mark.slow  # expected runtime: 35 s
def test_nhjschulz_cfsm(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/nhjschulz/cfsm.git", "73315639cce1f6101091323fc5568304b218a4dc"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="nhjschulz_cfsm",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(
        tmp_resultsdir / "final", ["run", "--bin", "test_c_fsm"], capture_output=True
    )
    # The test output includes absolute paths, so we just check that the last few lines look right.
    stdout_lines_b = rs_prog_output.stdout.split(b"\n")
    assert stdout_lines_b[-4:] == [
        b"-----------------------",
        b"4 Tests 0 Failures 0 Ignored ",
        b"OK",
        b"",
    ], f"Got: {rs_prog_output.stdout!r}"

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 20 s
def test_marc_q__libbmp(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/marc-q/libbmp.git", "66bec6d7daf254e6dc07d55c9383fd68276a6a39"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="marc_q_libbmp",
            buildcmd="make -C test CC=cc",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(tmp_resultsdir / "final", ["run"], capture_output=True)
    # The test output includes absolute paths, so we just check that the last few lines look right.

    assert (
        rs_prog_output.stdout
        == b"""LibBMP-Test v. 0.0.1 A (C) 2016 - 2017 Marc Volker Dickmann

BMP_GET_PADDING		PASSED!
header_size		PASSED!
header_init_df		PASSED!
pixel_init		PASSED!


Points	4/4
Failed	0
"""
    )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 70 s
def test_rupertwh__bmplib(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/rupertwh/bmplib.git", "e7910ac36bfdc6c46fcaf5f8291ed9956ba98fd8"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # The codebase symlinks the old and new style config files but meson doesn't like that.
    (tmp_codebase / "meson_options.txt").unlink()

    hermetic.run(["meson", "setup", "builddir"], cwd=str(tmp_codebase), check=True)
    # Meson sets up placeholder symlinks but they interfere with `shutil.copytree`.
    for f in (tmp_codebase / "builddir").glob("libbmp.*"):
        if f.is_symlink():
            f.unlink()
    # The main build needs to generate this file but the program that generates it
    # should not be part of the translation.
    hermetic.run(["ninja", "-C", "builddir", "huffman-codes.h"], cwd=str(tmp_codebase), check=True)

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="rupertwh_bmplib",
            buildcmd="ninja -C builddir",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    specs = {
        "test_read_io": [
            "read_u32_le",
            "read_s32_le",
            "read_u16_le",
            "read_s16_le",
        ],
        "test_write_io": [
            "write_u32_le",
            "write_s32_le",
            "write_u16_le",
            "write_s16_le",
            "s_imgrgb_to_outbytes int",
            "s_imgrgb_to_outbytes float",
            "s_imgrgb_to_outbytes s2.13",
        ],
        "test_read_conversions": [
            "s_s2_13_to_float",
            "s_float_to_s2_13",
            "s_convert64",
            "roundtrip_s2.13-float-s2.13",
            "s_srgb_gamma_float",
            "s_int8_to_result_format float",
            "s_int8_to_result_format s2.13",
            "s_int8_to_result_format int",
        ],
    }
    for binname, argstrs in specs.items():
        binpath = tmp_resultsdir / "final" / "target" / "debug" / binname
        for argstr in argstrs:
            args = argstr.split()
            rs_prog_output = hermetic.run([str(binpath), *args], check=False, capture_output=True)
            c_prog_output = hermetic.run(
                [str(tmp_resultsdir / "_build_1" / "builddir" / binname), *args],
                check=False,
                capture_output=True,
            )
            assert rs_prog_output.stdout == c_prog_output.stdout, (
                f"Failed on {binname} with args {args!r}; got stdout: {rs_prog_output.stdout!r}, expected: {c_prog_output.stdout!r}"
            )
            assert rs_prog_output.stderr == c_prog_output.stderr, (
                f"Failed on {binname} with args {args!r}; got stderr: {rs_prog_output.stderr!r}, expected: {c_prog_output.stderr!r}"
            )
            assert rs_prog_output.returncode == c_prog_output.returncode, (
                f"Failed on {binname} with args {args!r}; got return code: {rs_prog_output.returncode}, expected: {c_prog_output.returncode}"
            )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


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
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="sbase_cal",
            buildcmd=hermetic.shellize(buildcmd_args),
        ),
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
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd="make",
        ),
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
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd="make url-test",
        ),
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


@pytest.mark.slow  # expected runtime: 510 s
#                      of which 265 s is refolding, 100 s is numeric cast removal
def test_fribidi_g0(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/fribidi/fribidi.git", "069a7e3d31e6aa74f2068a8e0804106ce7906639"
    )

    # fribidi builds irrelevant utilities by default.
    # We first do a full minimal build, then remove all the artifacts from the library.
    prebuildcmd = " && ".join([
        "meson setup _builddir -Dbin=false -Dtests=false -Ddocs=false",
        "ninja -C _builddir",
        "rm -rf _builddir/lib/libfribidi.so.0.4.0",
        "rm -rf _builddir/lib/libfribidi.so.0.4.0.p/*",
    ])
    # Then, invoking ninja will re-build just the library artifacts.
    buildcmd = "ninja -C _builddir"

    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            prebuildcmd=prebuildcmd,
            buildcmd=buildcmd,
        ),
        guidance_path_or_literal="{}",
    )

    # To test the resulting shared object, we'd need to re-build
    # with tests=true, and replace the built shared object (`_builddir/lib/libfribidi.so.0.4.0`)
    # with (tmp_resultsdir / "final" / "target" / "debug" / "libfribidi_0_4_0.so")
    # then run `top_builddir=$PWD/_builddir ./test/run.tests`
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 1600 s (about half an hour)
#                      of which 21 minutes is cclyzerpp and 4.5 minutes is refolding.
def test_libusb_shared_g0(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/libusb/libusb.git", "87a55632db62c9bdc58cd31d3ccfa673f1bb017f"
    )

    prebuildcmd = "NOCONFIGURE=1 ./autogen.sh && ./configure --disable-static --disable-udev CC=cc"
    buildcmd = "make -j3"

    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            prebuildcmd=prebuildcmd,
            buildcmd=buildcmd,
        ),
        guidance_path_or_literal="{}",
    )

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    hermetic.run(
        "make -j3 test_static_link_flag=".split(),
        cwd=str(tmp_resultsdir / "_build_1" / "tests"),
        check=True,
    )
    # Run the test suite against the original C shared library
    hermetic.run("./.libs/stress", cwd=str(tmp_resultsdir / "_build_1" / "tests"), check=True)
    # Copy the Rust shared library over the C version
    shutil.copyfile(
        tmp_resultsdir / "final" / "target" / "debug" / "libusb_1_0_0_6_0.so",
        tmp_resultsdir / "_build_1" / "libusb" / ".libs" / "libusb-1.0.so.0.6.0",
    )
    # Re-run the test suite against the Rust code
    hermetic.run("./.libs/stress", cwd=str(tmp_resultsdir / "_build_1" / "tests"), check=True)

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

    # Note that cclyzer++ currently does not run on this codebase due to two
    # incidental restrictions: we don't run it on multi-target codebases (lua + liblua),
    # and we don't run it on bitcode files as large as liblua's.
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd=hermetic.shellize(buildcmd_args),
        ),
        guidance_path_or_literal="{}",
    )

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

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd="make -f Makefile.pure_cli",
        ),
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


@pytest.mark.slow
def test_pkhuong_ppb__picoscope(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir

    codebase = cached_git_clone_at_commit(
        "https://github.com/pkhuong/ppb.git", "26a68330cc6265771aa159a520b6db4483e1586e"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="ppb_picoscope",
            buildcmd="make CC=cc build/picoscope",
        ),
        guidance_path_or_literal="{}",
    )

    c_prog_output = hermetic.run(
        [
            "bash",
            tmp_codebase / "test_picoscope.sh",
            str(tmp_resultsdir / "_build_1" / "build" / "picoscope"),
        ],
        cwd=str(tmp_codebase),
        check=False,
        capture_output=True,
    )

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    rs_prog_output = hermetic.run(
        [
            "bash",
            tmp_codebase / "test_picoscope.sh",
            str(tmp_resultsdir / "final" / "target" / "debug" / "picoscope"),
        ],
        cwd=str(tmp_codebase),
        check=False,
        capture_output=True,
    )

    assert rs_prog_output.stdout == c_prog_output.stdout, (
        f"Rust and C output differed; Rust output was: {rs_prog_output.stdout!r}"
    )
    assert rs_prog_output.stderr == c_prog_output.stderr, (
        f"Rust and C error output differed; Rust error was: {rs_prog_output.stderr!r}"
    )
    assert rs_prog_output.returncode == c_prog_output.returncode, (
        f"Different exit codes; Rust got {rs_prog_output.returncode} vs C {c_prog_output.returncode}"
    )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow
@pytest.mark.xfail(reason="This test fails the cclyzer globals-localization phase")
def test_libtom_libtommath(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir

    codebase = cached_git_clone_at_commit(
        "https://github.com/libtom/libtommath.git",
        "ae40a87a920099a7d9d00979570e0c8d917a1fd7",
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    buildcmd = (
        "for f in mp_*.c s_mp_*.c demo/test.c demo/shared.c; do "
        'o=$(basename "${f%.c}").o; cc -O1 -I. -c "$f" -o "$o" || exit 1; '
        "done && cc -O1 -o test *.o"
    )

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="libtom_libtommath",
            buildcmd=buildcmd,
        ),
        guidance_path_or_literal="{}",
    )

    c_prog_output = hermetic.run(
        [str(tmp_resultsdir / "_build_1" / "test")],
        check=False,
        capture_output=True,
    )

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = hermetic.run_cargo_on_translated_code(
        ["run"],
        cwd=tmp_resultsdir / "final",
        capture_output=True,
        check=False,
    )

    # The test suite seeds its RNG from time(NULL) and prints the seed plus
    # random intermediate values, so full stdout is not reproducible. The exit
    # code and the final "Tests OK/NOP/FAIL: <ok>/<nop>/<fail>" summary line are
    # deterministic, so we compare those.
    def summary_line(stdout: bytes) -> bytes:
        for line in reversed(stdout.split(b"\n")):
            if line.startswith(b"Tests OK/NOP/FAIL:"):
                return line
        raise AssertionError(f"No summary line found in output: {stdout!r}")

    assert c_prog_output.returncode == 0, (
        f"C test binary failed (rc={c_prog_output.returncode}); stderr: {c_prog_output.stderr!r}"
    )
    assert rs_prog_output.returncode == c_prog_output.returncode, (
        f"Different exit codes; Rust got {rs_prog_output.returncode} vs C {c_prog_output.returncode};"
        f" Rust stderr: {rs_prog_output.stderr!r}"
    )
    assert summary_line(rs_prog_output.stdout) == summary_line(c_prog_output.stdout), (
        f"Rust and C summary lines differed; Rust: {summary_line(rs_prog_output.stdout)!r},"
        f" C: {summary_line(c_prog_output.stdout)!r}"
    )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)
