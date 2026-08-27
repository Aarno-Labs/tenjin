import hashlib
from pathlib import Path
import re
import shutil
import platform
import struct
import subprocess

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
from provisioning import download


def sha256hex(filepath: Path) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(8192):
            h.update(chunk)
    return h.hexdigest()


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


@pytest.mark.slow  # expected runtime: 180 s
def test_cmatsuoka_figlet(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/cmatsuoka/figlet.git", "202a0a8110650a943f1125f536b3bb455cf72ee1"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="cmatsuoka_figlet",
            buildcmd="make CC=cc LD=cc figlet",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(
        tmp_resultsdir / "final",
        [
            "run",
            "--",
            "-f",
            (codebase / "fonts" / "banner.flf").as_posix(),
            "-C",
            (codebase / "fonts" / "upper.flc").as_posix(),
            "y0",
        ],
        capture_output=True,
    )
    # The test output includes absolute paths, so we just check that the last few lines look right.
    stdout_b = rs_prog_output.stdout
    assert stdout_b.split(b"\n") == [
        b"#     #   ###   ",
        b" #   #   #   #  ",
        b"  # #   #     # ",
        b"   #    #     # ",
        b"   #    #     # ",
        b"   #     #   #  ",
        b"   #      ###   ",
        b"                ",
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
def test_uxnmin(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir

    codebase = cached_git_clone_at_commit(
        "https://github.com/brk/uxnzoo.git", "617697ebe9c6e178db66fcb5b203ab5a3d05607c"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase / "uxnmin" / "src" / "uxnmin.c",
            resultsdir=tmp_resultsdir,
        ),
        guidance_path_or_literal="{}",
    )

    hermetic.run_cargo_on_translated_code(
        ["build"],
        cwd=tmp_resultsdir / "final",
        capture_output=False,
        check=True,
    )

    unxmin_exe = tmp_resultsdir / "final" / "target" / "debug" / "uxnmin"

    hermetic.run(
        f"{unxmin_exe.as_posix()} ./uxnmin/etc/utils/xh.txt < drifblim/etc/drifloon.rom.txt > drifloon.rom",
        cwd=tmp_codebase,
        check=True,
        capture_output=False,
        shell=True,
    )

    drifloon_rom_hash = hashlib.sha256(open(tmp_codebase / "drifloon.rom", "rb").read()).hexdigest()
    assert drifloon_rom_hash == "ffb639c0b52e212402e3f88897e9b3a16df472a1c00d73fe914f78f00c54330f"

    hermetic.run(
        f"{unxmin_exe.as_posix()} drifloon.rom < tictactoe.tal > tictactoe.rom",
        cwd=tmp_codebase,
        check=True,
        capture_output=False,
        shell=True,
    )

    tictactoe_rom_hash = hashlib.sha256(
        open(tmp_codebase / "tictactoe.rom", "rb").read()
    ).hexdigest()
    assert tictactoe_rom_hash == "15d387e1d8568d53cf996190aabf7f5119d5bcd9a5f775711c8a8b1e6cbe4d4e"

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
@pytest.mark.skip(reason="This test fails the cclyzer globals-localization phase")
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


@pytest.mark.slow  # expected runtime: ~30 minutes
@pytest.mark.skip(
    reason="dbcc does not yet translate end-to-end; the C sources handed to c2rust "
    "fail to parse, so no final/ crate is produced. Refolding itself is not at "
    "fault (the c_16 output was verified token-faithful to the modified program); "
    "both error classes originate elsewhere: (1) every TU except getopt/util calls "
    "assert(<pointer>); with assert a blocked macro during translation, the calls "
    "bind to the autoincluded 'void assert(int);' marker decl and Clang rejects "
    "the pointer-to-int conversions as errors. (2) The _xjw unmodified-function "
    "wrappers from xj-prepare-findfnptrdecls are inserted (at c_13, pre-refold) "
    "between a forward declaration and its ';' in mpc.c (the insertion-point "
    "lookup sees hasBody() true via a later redecl, looks for a '}' after the "
    "prototype, and falls back to just after the ')'), yielding invalid C plus "
    "knock-on conflicting-type and incompatible-function-pointer errors "
    "(mpc_fold_t vs xjg-threaded mpc_fold_t_xjtp, etc.); the invalid Clang AST "
    "makes xj-c2rust panic (exit 101, conversion.rs 'Type conversion not "
    "implemented for TagTypeUnknown').",
)
def test_howerj_dbcc(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/howerj/dbcc.git", "2f5031d8013aafed199a35c2dfa92db2bb33a5de"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="howerj_dbcc",
            buildcmd="make dbcc CC=cc",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    # dbcc compiles a CAN DBC description into C/XML/CSV/JSON source. Each output
    # format writes its generated files into the directory named by `-o`, so we
    # point the C and Rust builds at separate output directories and compare the
    # generated files byte-for-byte (as well as stdout/stderr/exit code) across a
    # range of inputs and every conversion mode.
    c_dbcc = tmp_resultsdir / "_build_1" / "dbcc"
    rs_dbcc = tmp_resultsdir / "final" / "target" / "debug" / "howerj_dbcc"

    dbc_files = [
        "ex1.dbc",
        "ex2.dbc",
        "enum.dbc",
        "mul-val.dbc",
        "single-enum.dbc",
        "double_signal.dbc",
        "float_signal.dbc",
    ]
    # Each flag selects an output format: "" is the default C codec, -x is XML,
    # -C is CSV, and -j is JSON.
    mode_flags = ["", "-x", "-C", "-j"]

    c_out = tmp_resultsdir / "dbcc_c_out"
    rs_out = tmp_resultsdir / "dbcc_rs_out"

    for dbc in dbc_files:
        dbc_path = tmp_codebase / dbc
        for flag in mode_flags:
            flag_args = [flag] if flag else []
            shutil.rmtree(c_out, ignore_errors=True)
            shutil.rmtree(rs_out, ignore_errors=True)
            c_out.mkdir()
            rs_out.mkdir()

            c_proc = hermetic.run(
                [str(c_dbcc), *flag_args, "-o", str(c_out), str(dbc_path)],
                check=False,
                capture_output=True,
            )
            rs_proc = hermetic.run(
                [str(rs_dbcc), *flag_args, "-o", str(rs_out), str(dbc_path)],
                check=False,
                capture_output=True,
            )

            label = f"{dbc} (flag {flag!r})"
            assert rs_proc.returncode == c_proc.returncode, (
                f"{label}: different exit codes; Rust got {rs_proc.returncode} vs C {c_proc.returncode}"
            )
            assert rs_proc.stdout == c_proc.stdout, (
                f"{label}: stdout differed; Rust output was: {rs_proc.stdout!r}"
            )
            assert rs_proc.stderr == c_proc.stderr, (
                f"{label}: stderr differed; Rust error was: {rs_proc.stderr!r}"
            )

            c_files = sorted(p.name for p in c_out.iterdir())
            rs_files = sorted(p.name for p in rs_out.iterdir())
            assert rs_files == c_files, (
                f"{label}: generated a different set of files; Rust {rs_files} vs C {c_files}"
            )
            for name in c_files:
                c_bytes = (c_out / name).read_bytes()
                rs_bytes = (rs_out / name).read_bytes()
                assert rs_bytes == c_bytes, (
                    f"{label}: generated file {name!r} differed between Rust and C"
                )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 110 s
def test_blackle_megalania(tenjin_fixtures: TenjinFixtures):
    """Translate Megalania's compressor and require it to behave exactly as the C
    build does: byte-identical compressed output on several inputs, and matching
    diagnostics and exit codes on its failure paths.

    Megalania's Makefile builds two programs out of the same sources: `megalania`
    (src/main.c plus the library) and `megalania_tests` (tests/*.c plus the same
    library, with src/main.c filtered out). We translate only the former, for two
    reasons. The repo's unit tests reach just 5 of the 16 library files: the LZMA
    core (range coder, probability model, packet encoder, state machine) and the
    annealing search (top-k finder, slab neighbour, packet enumerator) are compiled
    but never run there, whereas compressing even a 64-byte input executes
    essentially all of the library. And translating both programs at once is not an
    option: a codebase with more than one build target skips cclyzer++'s
    globals localization and preprocessor refolding, which are two of the passes this
    test is here to exercise.
    """
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/blackle/Megalania.git",
        "8246d38223b653ec22d99308b630962daa3a3b16",
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    megalania_lib_srcs = [
        "src/file_output.c",
        "src/lzma_header_encoder.c",
        "src/lzma_packet.c",
        "src/lzma_packet_encoder.c",
        "src/lzma_state.c",
        "src/max_heap.c",
        "src/memory_mapper.c",
        "src/packet_enumerator.c",
        "src/packet_slab.c",
        "src/packet_slab_neighbour.c",
        "src/packet_slab_undo_stack.c",
        "src/perplexity_encoder.c",
        "src/probability_model.c",
        "src/range_encoder.c",
        "src/substring_enumerator.c",
        "src/top_k_packet_finder.c",
    ]

    # We spell out the build rather than using the Makefile: it hardcodes gcc and
    # builds both programs, and we want the translation to see exactly one target.
    # `-flto` and `-Werror` are dropped (the former is the build system's concern,
    # the latter turns clang's differing warnings into build failures), and the
    # Makefile's -O3 is not needed, since the comparison is on program output.
    buildcmd_args = [
        "cc",
        "-o",
        "megalania",
        "src/main.c",
        *megalania_lib_srcs,
        "-lm",
        "-g",
        "-Wall",
        "-Wextra",
    ]

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd=hermetic.shellize(buildcmd_args),
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    c_compressor = tmp_resultsdir / "_build_1" / "megalania"
    # The crate is named after the build's output artifact (megalania) and the binary
    # after the file holding `main` (src/main.c).
    rs_compressor = tmp_resultsdir / "final" / "target" / "debug" / "main"

    def run_both(
        args: list[str], label: str
    ) -> tuple[subprocess.CompletedProcess, subprocess.CompletedProcess]:
        """Run the C build and the translated build on the same arguments, requiring
        that they agree on exit code, stdout and stderr; returns both results so the
        caller can check the C side against what it is known to produce."""
        c_proc = hermetic.run([str(c_compressor), *args], check=False, capture_output=True)
        rs_proc = hermetic.run([str(rs_compressor), *args], check=False, capture_output=True)
        assert rs_proc.returncode == c_proc.returncode, (
            f"[{label}] Different exit codes; Rust got {rs_proc.returncode}"
            f" vs C {c_proc.returncode}; Rust stderr: {rs_proc.stderr!r}"
        )
        assert rs_proc.stdout == c_proc.stdout, (
            f"[{label}] Rust and C output differed;"
            f" Rust produced {len(rs_proc.stdout)} bytes vs C {len(c_proc.stdout)}"
        )
        assert rs_proc.stderr == c_proc.stderr, (
            f"[{label}] Rust and C error output differed; Rust error was: {rs_proc.stderr!r}"
        )
        return c_proc, rs_proc

    megalania_inputs = [
        # (name, contents, size of the C build's compressed output)
        # Text: repeated words and shared prefixes, so the match finder and the packet
        # encoder both get real work to do.
        ("mixed_text", b"the quick brown fox jumps over the lazy dog, the quick brown fox", 61),
        # Binary, with bytes outside ASCII and a short period.
        ("binary_repeat", bytes([0x00, 0xFF] * 16), 22),
        # Degenerate: one long run, so nearly every packet is a match.
        ("single_run", b"a" * 16, 20),
    ]

    # The annealing search calls rand(), but main() seeds it with srand(1673551), so
    # the search -- and hence the packet sequence, and hence the compressed bytes --
    # is fully determined by the input. Verified stable across repeated runs and
    # across -O0/-O3 on the C side.
    for name, contents, expected_c_size in megalania_inputs:
        # main() mmaps its argument, so the input has to be a real file on disk.
        input_path = tmp_resultsdir / f"compressor_input_{name}.bin"
        input_path.write_bytes(contents)

        c_proc, _ = run_both([str(input_path)], name)
        assert c_proc.returncode == 0, (
            f"[{name}] The C megalania failed (rc={c_proc.returncode}); stderr: {c_proc.stderr!r}"
        )
        assert len(c_proc.stdout) == expected_c_size, (
            f"[{name}] The C megalania produced {len(c_proc.stdout)} bytes of compressed"
            f" output, expected {expected_c_size}"
        )
        assert b"current file size:" in c_proc.stderr, (
            f"[{name}] The C megalania reported no annealing progress; stderr: {c_proc.stderr!r}"
        )

    # The failure paths, which the successful runs above never touch: mmap of an
    # empty file fails, and open of a missing file fails. Both print a diagnostic
    # naming the file (the same path string for both builds) and return -1.
    empty_path = tmp_resultsdir / "compressor_input_empty.bin"
    empty_path.write_bytes(b"")
    c_proc, _ = run_both([str(empty_path)], "empty_file")
    assert c_proc.returncode != 0 and c_proc.stderr == f"could not mmap {empty_path}\n".encode(), (
        f"The C megalania did not report a failed mmap for an empty file;"
        f" rc={c_proc.returncode}, stderr: {c_proc.stderr!r}"
    )

    missing_path = tmp_resultsdir / "compressor_input_does_not_exist.bin"
    c_proc, _ = run_both([str(missing_path)], "missing_file")
    assert (
        c_proc.returncode != 0 and c_proc.stderr == f"could not open {missing_path}\n".encode()
    ), (
        f"The C megalania did not report a failed open for a missing file;"
        f" rc={c_proc.returncode}, stderr: {c_proc.stderr!r}"
    )

    # The usage path is checked separately: its message contains argv[0], which is
    # necessarily a different path for the two builds, so only the exit code and the
    # shape of the message can be compared.
    c_usage = hermetic.run([str(c_compressor)], check=False, capture_output=True)
    rs_usage = hermetic.run([str(rs_compressor)], check=False, capture_output=True)
    assert rs_usage.returncode == c_usage.returncode, (
        f"Different exit codes for the usage path; Rust got {rs_usage.returncode}"
        f" vs C {c_usage.returncode}; Rust stderr: {rs_usage.stderr!r}"
    )
    assert (
        c_usage.returncode != 0 and c_usage.stderr == f"usage: {c_compressor} filename\n".encode()
    ), (
        f"The C megalania did not print its usage message; rc={c_usage.returncode},"
        f" stderr: {c_usage.stderr!r}"
    )
    assert rs_usage.stderr == f"usage: {rs_compressor} filename\n".encode(), (
        f"Rust did not print the usage message C prints; Rust stderr: {rs_usage.stderr!r}"
    )
    assert rs_usage.stdout == c_usage.stdout, (
        f"Rust and C usage-path output differed; Rust output was: {rs_usage.stdout!r}"
    )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 15 minutes
def test_lemon_exe(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/tenjin-corpus/lemon-grove.git",
        "54285f4d6d9129666e08d769eb9deee8210922ba",
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase / "lemon",
            resultsdir=tmp_resultsdir,
            buildcmd="cc lemon.c -o lemon",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    help_output: bytes = hermetic.run(
        ["target/debug/lemon", "-?"],
        capture_output=True,
        cwd=tmp_resultsdir / "final",
    ).stderr

    assert (
        help_output
        == b"""Command line syntax error: undefined option.
target/debug/lemon -?
             here --^
Valid command line options for "target/debug/lemon" are:
  -b           Print only the basis in report.
  -c           Don't compress the action table.
  -d<string>   Output directory.  Default '.'
  -D<string>   Define an %ifdef macro.
  -E           Print input file after preprocessing.
  -f<string>   Ignored.  (Placeholder for -f compiler options.)
  -g           Print grammar without actions.
  -I<string>   Ignored.  (Placeholder for '-I' compiler options.)
  -m           Output a makeheaders compatible file.
  -l           Do not print #line statements.
  -O<string>   Ignored.  (Placeholder for '-O' compiler options.)
  -p           Show conflicts resolved by precedence rules
  -q           (Quiet) Don't print the report file.
  -r           Do not sort or renumber states
  -s           Print parser stats to standard output.
  -S           Generate the *.sql file describing the parser tables.
  -x           Print the version number.
  -T<string>   Specify a template file.
  -W<string>   Ignored.  (Placeholder for '-W' compiler options.)
"""
    )


@pytest.mark.slow  # expected runtime: 220 seconds
def test_zopfli_exe(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/brk/zopfli.git", "87b306de5260bfb8197feee89c81b9195447ffc6"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd="make zopfli",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    help_output: bytes = hermetic.run(
        [tmp_resultsdir / "final" / "target" / "debug" / "zopfli_bin", "-h"],
        capture_output=True,
    ).stderr

    assert (
        help_output
        == b"""Usage: zopfli [OPTION]... FILE...
  -h    gives this help
  -c    write the result on standard output, instead of disk filename + '.gz'
  -v    verbose mode
  --i#  perform # iterations (default 15). More gives more compression but is slower. Examples: --i10, --i50, --i1000
  --gzip        output to gzip format (default)
  --zlib        output to zlib format instead of gzip
  --deflate     output to deflate format instead of gzip
  --splitlast   ignored, left for backwards compatibility
"""
    )

    run_cargo_on_final(
        tmp_resultsdir / "final", ["run", "--", (tmp_codebase / "COPYING").as_posix()]
    )

    def reset_gzip_mtime(path: Path) -> None:
        """Overwrite the MTIME field in a gzip file's header, in place."""

        GZIP_MAGIC = b"\x1f\x8b"
        MTIME_OFFSET = 4

        with path.open("r+b") as f:
            if f.read(2) != GZIP_MAGIC:
                raise ValueError(f"{path} is not a gzip file")
            f.seek(MTIME_OFFSET)
            f.write(b"\x00\x00\x00\x00")

    assert (
        sha256hex(tmp_codebase / "COPYING")
        == "018b1cb87efdf7a04c2fcc13d57ed63f62149113fb207b27ea13430d64f13513"
    )

    reset_gzip_mtime(tmp_codebase / "COPYING.gz")

    assert (
        sha256hex(tmp_codebase / "COPYING.gz")
        == "c7d0f6d70256238349e9f682d7d1362a832cc955b653cee712b8a1db92a15acd"
    )


@pytest.mark.slow  # expected runtime: 120 seconds
def test_silentbicycle__guff(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/silentbicycle/guff.git", "a6f11ad8973e83dcb9650c256cdee3caf87a12ca"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd="make guff",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    input_str = "\n".join([
        "218",
        "212",
        "210",
        "196",
        "136",
        "81",
        "75",
        "67",
        "49",
        "16",
    ])
    # (tmp_resultsdir / "in_1").write_text(input_str)

    target_out = tmp_resultsdir / "final" / "target" / "debug"
    rs_guff = target_out / "main"
    if not rs_guff.exists():
        rs_guff = target_out / "main_nolines"

    guff_out_1: str = hermetic.run(
        [str(rs_guff), "-d", "40x20"],
        input=input_str.encode(encoding="utf-8"),
        check=True,
        capture_output=True,
    ).stdout.decode(encoding="utf-8")

    assert guff_out_1.splitlines() == [
        "    x: [0 - 9]    y: [0 - 218] -- 0: #",
        "+                                       ",
        "#                                       ",
        "|   #   #                               ",
        "|            #                          ",
        "|                                       ",
        "+                                       ",
        "|                                       ",
        "|                                       ",
        "|                #                      ",
        "|                                       ",
        "+                                       ",
        "|                                       ",
        "|                    #                  ",
        "|                        #              ",
        "|                             #         ",
        "+                                 #     ",
        "|                                       ",
        "|                                       ",
        "|                                     # ",
        "+----+----+----+----+----+----+----+----",
    ]


@pytest.mark.slow  # expected runtime: 9 seconds
def test_silentbicycle__rollavg(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/silentbicycle/rollavg.git", "30ccedee7dcc499bb07d26cd78f539bb550deeb8"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Inject missing header; getopt does not come via unistd.h in strict c99 mode.
    r_c = tmp_codebase / "rollavg.c"
    r_c.write_text("#include <getopt.h>\n" + r_c.read_text())

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd="make rollavg",
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    input_str = "\n".join([
        "10",
        "88",
        "93",
        "02",
        "1",
        "12000",
    ])

    rollavg_out_1: str = hermetic.run(
        [str(tmp_resultsdir / "final" / "target" / "debug" / "rollavg")],
        input=input_str.encode(encoding="utf-8"),
        check=True,
        capture_output=True,
    ).stdout.decode(encoding="utf-8")

    assert (
        rollavg_out_1
        == """10.000000
51.052631
66.531357
47.766792
36.346607
2589.633301
"""
    )


@pytest.mark.slow  # expected runtime: 9 seconds
def test_silentbicycle__skel(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/silentbicycle/skel.git", "5efbd30682abbe519008885e241b6498d01381f9"
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
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    (tmp_codebase / "build").mkdir(exist_ok=False)
    shutil.copy2(
        tmp_resultsdir / "final" / "target" / "debug" / "main",
        tmp_codebase / "build" / "skel",
    )

    cp = hermetic.run(
        [str(tmp_codebase / "test" / "run_tests")],
        cwd=tmp_codebase.as_posix(),
        check=True,
        capture_output=True,
    )
    assert cp.stdout == b"...................tests complete\n"


@pytest.mark.slow  # expected runtime: 120 seconds
def test_atomicobject__odo(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/atomicobject/odo.git", "be7f07b2f0f363ec3c69d86d2be98822ae0acb2c"
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
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    shutil.copyfile(
        tmp_resultsdir / "final" / "target" / "debug" / "main",
        tmp_codebase / "odo",
    )

    cp = hermetic.run(["./test_odo"], capture_output=True, check=True, cwd=tmp_codebase)
    assert cp.stdout.decode("utf-8") == "all tests passed\n"


@pytest.mark.slow  # expected runtime: 700 seconds (~12 minutes, up to the xfail below)
@pytest.mark.skip(
    reason="file(1) does not yet translate end-to-end: refold emits valid C for all 27 "
    "TUs, but xj-c2rust's raw output (00_out) fails `cargo check` with 6 errors from "
    "two causes. (1) softmagic.c's magiccheck() uses isunordered(); xj-c2rust reports "
    "'Unimplemented builtin __builtin_isunordered' and drops the definition while "
    "keeping its two call sites -> 2x E0425 in softmagic.rs. (2) compress.c calls "
    "FD_ZERO, which glibc implements as x86-64 inline asm; the translation passes "
    "`&raw mut` locals (*mut i32) to c2rust_asm_casts::AsmCast::cast_in/cast_out, "
    "which expect `&mut _` -> 4x E0308 in compress.rs. The `cargo check` gate after "
    "improvement_pass_02_lift-call-args then raises CalledProcessError, so no final/ "
    "crate is produced.",
)
def test_file_file(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/file/file.git", "eb754ace19fed5481d8142426543100a2d6bae4e"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # autoreconf/configure/make must run before there is anything to translate:
    # configure writes config.h, make generates src/magic.h and compiles the magic
    # database into magic/magic.mgc. Done outside `do_translate` so the interceptor
    # never sees configure's hundreds of conftest.c probes.
    #
    # The decompression back-ends are disabled so the translation doesn't depend on
    # which of zlib/bzlib/xz/... are installed; the sandboxes (libseccomp, Landlock)
    # because they'd restrict the C and Rust binaries unevenly.
    #
    # Tenjin's `aclocal` and `libtoolize` live under different prefixes, so aclocal
    # can't find libtool.m4 and autoreconf dies with "Libtool library used but LIBTOOL
    # is undefined". Deriving ACLOCAL_PATH from `libtoolize` works for both that split
    # layout and a normal system install.
    configure_args = [
        "--disable-shared",
        "--disable-landlock",
        "--disable-libseccomp",
        "--disable-zlib",
        "--disable-bzlib",
        "--disable-xzlib",
        "--disable-zstdlib",
        "--disable-lzlib",
        "--disable-lrziplib",
        "--disable-lz4lib",
    ]
    # `CC=cc` picks Tenjin's clang, which compiles against a bundled glibc 2.26
    # sysroot. Otherwise configure probes the host gcc/libc and on a modern glibc
    # concludes HAVE_STRLCPY/HAVE_STRLCAT (glibc 2.38+), dropping src/strlcpy.c and
    # src/strlcat.c from $(LIBOBJS) -- and the clang build then fails on the
    # undeclared functions.
    hermetic.run(
        "ACLOCAL_PATH=$(dirname $(dirname $(command -v libtoolize)))/share/aclocal"
        f" autoreconf -fi && ./configure CC=cc {' '.join(configure_args)} && make",
        cwd=str(tmp_codebase),
        shell=True,
        check=True,
    )

    # Building via `make` would give Tenjin a multi-target codebase (libmagic.a plus
    # src/file), which disables the non-trivial-refactoring passes. Instead we link
    # every object into `file` in one `cc` invocation, leaving exactly one target.
    #
    # The object list is platform-dependent -- $(LIBOBJS) holds only the
    # AC_REPLACE_FUNCS fallbacks this host lacks -- so ask the generated Makefile
    # rather than hardcoding it.
    objs = hermetic.run(
        [
            "make",
            "-C",
            "src",
            "-s",
            "--eval=xj-print-objs: ; @echo $(libmagic_la_OBJECTS) $(file_OBJECTS) $(LIBOBJS)",
            "xj-print-objs",
        ],
        cwd=str(tmp_codebase),
        check=True,
        capture_output=True,
    ).stdout.decode("utf-8")
    srcs = [f"src/{Path(o).stem}.c" for o in objs.split()]
    assert "src/file.c" in srcs and "src/apprentice.c" in srcs, (
        f"Object list from the generated Makefile looks wrong: {objs!r}"
    )

    buildcmd_args = [
        "cc",
        "-DHAVE_CONFIG_H",
        "-I.",
        "-Isrc",
        # Compiled-in default database path; unused here (we override it with $MAGIC
        # below) but src/apprentice.c doesn't compile without it.
        '-DMAGIC="/usr/local/share/misc/magic"',
        *srcs,
        "-lm",
        "-o",
        "file.exe",
    ]
    hermetic.run(buildcmd_args, cwd=str(tmp_codebase), check=True)

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="file_file",
            buildcmd=hermetic.shellize(buildcmd_args),
        ),
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    c_file = tmp_codebase / "file.exe"
    rs_file = tmp_resultsdir / "final" / "target" / "debug" / "file"

    # The compiled-in default database path is only populated by `make install`, so
    # point both binaries at the magic.mgc that `make` compiled above. TZ is pinned
    # because many magic entries render timestamps.
    env_ext = {
        "MAGIC": str(tmp_codebase / "magic" / "magic.mgc"),
        "TZ": "UTC",
    }

    # The upstream corpus: ~88 `<name>.testfile` samples of formats the magic database
    # should recognize. Upstream checks them via tests/test.c, a separately linked
    # libmagic client that would make the codebase multi-target again, so we drive the
    # same corpus through the `file` CLI and require Rust to match C byte-for-byte.
    testfiles = sorted((tmp_codebase / "tests").glob("*.testfile"))
    assert len(testfiles) > 50, f"Expected the upstream test corpus, found {len(testfiles)} files"

    # Each flag set exercises a different libmagic output path: default description,
    # MIME type/encoding, `-k` (MAGIC_CONTINUE: print every match, not just the
    # first), and the two combined.
    flag_sets = [["-b"], ["-b", "--mime"], ["-b", "-k"], ["-b", "-k", "--mime-type"]]

    for testfile in testfiles:
        for flags in flag_sets:
            args = [*flags, str(testfile)]
            c_proc = hermetic.run(
                [str(c_file), *args], check=False, capture_output=True, env_ext=env_ext
            )
            rs_proc = hermetic.run(
                [str(rs_file), *args], check=False, capture_output=True, env_ext=env_ext
            )
            label = f"{testfile.name} (flags {flags!r})"
            assert rs_proc.returncode == c_proc.returncode, (
                f"{label}: different exit codes; Rust got {rs_proc.returncode} "
                f"vs C {c_proc.returncode}"
            )
            assert rs_proc.stdout == c_proc.stdout, (
                f"{label}: stdout differed; Rust output was: {rs_proc.stdout!r}, "
                f"C output was: {c_proc.stdout!r}"
            )
            assert rs_proc.stderr == c_proc.stderr, (
                f"{label}: stderr differed; Rust error was: {rs_proc.stderr!r}, "
                f"C error was: {c_proc.stderr!r}"
            )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 40 s
def test_xiph_speex_speexenc_only(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/xiph/speex.git", "05895229896dc942d453446eba6f9f5ddcf95422"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # We have to do a pretty careful dance to translate just the test executable(s)
    # and then run them against the C version of the library.

    hermetic.run(
        ["meson", "setup", "builddir", "-Dsse=disabled"], cwd=str(tmp_codebase), check=True
    )
    # In this configuration, we pre-build the libspeex library so that
    # we only capture the commands for building speexenc itself.
    hermetic.run(
        ["ninja", "-C", "builddir", "libspeex/libspeex.so.1.5.2"], cwd=str(tmp_codebase), check=True
    )

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            buildcmd="ninja -C builddir src/speexenc",
        ),
        guidance_path_or_literal="{}",
    )

    hermetic.run(["ninja", "-C", "builddir", "src/speexenc"], cwd=str(tmp_codebase), check=True)

    # modify cargo config to add an unconditional link search flag
    target_dir_str = (tmp_resultsdir / "final" / "target").as_posix()
    config_toml_path = tmp_resultsdir / "final" / ".cargo" / "config.toml"
    config_toml_path.write_text(
        config_toml_path.read_text().replace(
            "rustflags = [",
            f'rustflags = ["-L", "{target_dir_str}", ',
        )
    )

    # Copy the prebuilt shared library so Cargo will find it when it links.
    # XREF:legalize_name_for_ld in `cli/targets.py`
    (tmp_resultsdir / "final" / "target").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(
        tmp_codebase / "builddir" / "libspeex" / "libspeex.so.1.5.2",
        tmp_resultsdir / "final" / "target" / "libspeex.so",
    )

    hermetic.run_cargo_on_translated_code(["build"], cwd=tmp_resultsdir / "final", check=True)

    download("https://speex.org/samples/audio/male.wav", Path(tmp_codebase, "male.wav"))

    male_c_spx: bytes = hermetic.run(
        ["builddir/src/speexenc", "male.wav", "-"],
        cwd=str(tmp_codebase),
        check=True,
        capture_output=True,
    ).stdout

    male_rs_spx: bytes = hermetic.run(
        [(tmp_resultsdir / "final" / "target" / "debug" / "speexenc").as_posix(), "male.wav", "-"],
        cwd=str(tmp_codebase),
        check=True,
        capture_output=True,
    ).stdout

    def normalize_spx_for_compare(data: bytes) -> bytes:
        # speexenc seeds rand() with the current time, so its output is
        # not fully deterministic.
        # Zeros Ogg stream serial numbers and page CRCs for bytewise comparison.
        out = bytearray(data)
        pos = 0

        while pos < len(out):
            if out[pos : pos + 4] != b"OggS":
                raise ValueError(f"expected Ogg page at offset {pos}")

            page_segments = out[pos + 26]
            header_len = 27 + page_segments
            body_len = sum(out[pos + 27 : pos + header_len])

            out[pos + 14 : pos + 18] = b"\0" * 4  # stream serial number
            out[pos + 22 : pos + 26] = b"\0" * 4  # page checksum
            pos += header_len + body_len

        return bytes(out)

    assert normalize_spx_for_compare(male_rs_spx) == normalize_spx_for_compare(male_c_spx), (
        "Rust and C outputs for speexenc differ"
    )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


@pytest.mark.slow  # expected runtime: 650 s
def test_xiph_speex_libspeex(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/xiph/speex.git", "05895229896dc942d453446eba6f9f5ddcf95422"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            prebuildcmd="meson setup builddir -Dsse=disabled",
            buildcmd="ninja -C builddir libspeex/libspeex.so.1.5.2",
        ),
        guidance_path_or_literal="{}",
    )

    builddir = tmp_resultsdir / "_build_1" / "builddir"

    # Build speexenc and speexdec binaries; these embed a dependency on
    # their in-tree copies of libspeex.so
    hermetic.run(
        ["ninja", "src/speexenc", "src/speexdec"],
        cwd=str(builddir),
        check=True,
    )

    download("https://speex.org/samples/audio/male.wav", Path(tmp_codebase, "male.wav"))

    # Do one round-trip test with the pure-C versions of everything
    hermetic.run(
        [str(builddir / "src/speexenc"), "male.wav", "male.c.spx"],
        cwd=str(tmp_codebase),
        check=True,
        capture_output=False,
    )
    male_c_wav: bytes = hermetic.run(
        [str(builddir / "src/speexdec"), "male.c.spx", "-"],
        cwd=str(tmp_codebase),
        check=True,
        capture_output=True,
    ).stdout

    hermetic.run_cargo_on_translated_code(["build"], cwd=tmp_resultsdir / "final", check=True)

    shutil.copyfile(
        tmp_resultsdir / "final" / "target" / "debug" / "libspeex_1_5_2.so",
        builddir / "libspeex" / "libspeex.so.1.5.2",
    )

    hermetic.run(
        [str(builddir / "src/speexenc"), "male.wav", "male.rs.spx"],
        cwd=str(tmp_codebase),
        check=True,
        capture_output=False,
    )
    male_rs_wav = hermetic.run(
        [str(builddir / "src/speexdec"), "male.rs.spx", "-"],
        cwd=str(tmp_codebase),
        check=True,
        capture_output=True,
    ).stdout

    assert male_rs_wav == male_c_wav, "wav file via Rust library did not have the same output"

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def build_cmocka(prefix: Path) -> Path:
    """
    Build the cmocka unit-testing library that libdaq's test/ directory needs, and
    return the pkg-config directory to point libdaq's configure at. Neither Tenjin's
    dependencies nor a typical host provides cmocka, and libdaq's test/Makefile.am
    does not guard on the HAVE_CMOCKA conditional that its m4 macro defines -- so
    without cmocka, configure merely warns and test/ then fails to compile.

    Built static-only so that the test programs need no LD_LIBRARY_PATH at run time.
    """
    cached_clone = cached_git_clone_at_commit(
        # Tag cmocka-1.1.8; libdaq's tests are written against the cmocka 1.x API.
        "https://gitlab.com/cmocka/cmocka.git",
        "eba4d6ffca53b500ab8dfabc30256bb6c3088b2b",
    )
    # `cached_git_clone_at_commit` names its cache directory after the URL, so the
    # path contains the colon from "https:". Make treats a colon in a prerequisite as
    # a rule separator, and CMake's generated makefiles then die with "target pattern
    # contains no '%'" -- so build from a copy at a colon-free path.
    #
    # `symlinks=True` matters: cmocka's CMakeLists unconditionally drops a
    # `compile_commands.json` symlink into its *source* directory pointing at the build
    # directory, so a source tree that has been configured once contains a symlink whose
    # target may not exist. Copying symlinks as symlinks keeps copytree from trying to
    # dereference it.
    source = prefix / "_src"
    shutil.copytree(cached_clone, source, symlinks=True, dirs_exist_ok=True)

    builddir = prefix / "_build"
    hermetic.run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(builddir),
            f"-DCMAKE_INSTALL_PREFIX={prefix}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DUNIT_TESTING=OFF",
            "-DCMAKE_C_COMPILER=cc",
        ],
        check=True,
    )
    hermetic.run(["make", "-C", str(builddir), "install"], check=True)
    return prefix / "lib" / "pkgconfig"


def snort3_libdaq_savefile_capture() -> bytes:
    """
    Build a three-packet Ethernet/IPv4 capture -- a UDP datagram, an ICMP echo
    request, and a second UDP datagram -- in classic libpcap savefile format,
    which is what libdaq's `savefile` module reads. Generated rather than checked
    in so that the expected decode in `test_snort3_libdaq` can be read next to
    the packets that produce it.
    """

    def checksum(data: bytes) -> int:
        if len(data) % 2:
            data += b"\0"
        total = sum(struct.unpack(f"!{len(data) // 2}H", data))
        while total >> 16:
            total = (total & 0xFFFF) + (total >> 16)
        return ~total & 0xFFFF

    src_mac, dst_mac = bytes.fromhex("020000000001"), bytes.fromhex("020000000002")
    src_ip, dst_ip = bytes([10, 0, 0, 1]), bytes([10, 0, 0, 2])

    def ethernet_ipv4(proto: int, payload: bytes) -> bytes:
        ip_hdr = (
            struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(payload), 0x1234, 0, 64, proto, 0)
            + src_ip
            + dst_ip
        )
        ip_hdr = ip_hdr[:10] + struct.pack("!H", checksum(ip_hdr)) + ip_hdr[12:]
        return dst_mac + src_mac + b"\x08\x00" + ip_hdr + payload

    def udp(sport: int, dport: int, data: bytes) -> bytes:
        segment = struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data
        pseudo_hdr = src_ip + dst_ip + struct.pack("!BBH", 0, 17, len(segment))
        return segment[:6] + struct.pack("!H", checksum(pseudo_hdr + segment)) + segment[8:]

    def icmp_echo_request(ident: int, seq: int, data: bytes) -> bytes:
        message = struct.pack("!BBHHH", 8, 0, 0, ident, seq) + data
        return message[:2] + struct.pack("!H", checksum(message)) + message[4:]

    packets = [
        ethernet_ipv4(17, udp(1234, 5678, b"hello daq")),
        ethernet_ipv4(1, icmp_echo_request(1, 1, b"abcdefgh")),
        ethernet_ipv4(17, udp(4321, 8765, b"second udp payload")),
    ]

    # File header: magic, version 2.4, no timezone or sigfigs, 1518-byte snaplen,
    # linktype 1 (DLT_EN10MB), which is the only one the savefile module accepts.
    out = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 1518, 1)
    for i, packet in enumerate(packets):
        out += struct.pack("<IIII", 1700000000 + i, 0, len(packet), len(packet)) + packet
    return out


@pytest.mark.slow  # expected runtime: 85 s
def test_snort3_libdaq(tenjin_fixtures: TenjinFixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = cached_git_clone_at_commit(
        "https://github.com/snort3/libdaq.git", "959b13d76aa3409acadfe22d1c30864698a297a7"
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Of the bundled DAQ modules only `savefile` and `trace` are enabled; the rest need
    # libpcap, libmnl, netmap headers or root privileges, so which of them configure
    # picks up would otherwise vary by host. Neither of these two needs anything beyond
    # libc -- `savefile` parses libpcap-format capture files itself. `trace` is enabled
    # only because configure.ac's BUILD_MODULES conditional does not list `savefile`:
    # with `savefile` alone, modules/ is skipped entirely and example/ fails to link.
    #
    # Tenjin's `aclocal` and `libtoolize` live under different prefixes, so aclocal can't
    # find libtool.m4 and autoreconf dies with "Libtool library used but LIBTOOL is
    # undefined". Deriving ACLOCAL_PATH from `libtoolize` works for both that split
    # layout and a normal system install.
    disabled_modules = ("afpacket", "bpf", "divert", "dump", "fst", "netmap", "nfq", "pcap", "gwlb")
    configure_args = [
        "--disable-static",
        "--enable-savefile-module",
        "--enable-trace-module",
        *(f"--disable-{module}-module" for module in disabled_modules),
    ]
    cmocka_pkgconfig = build_cmocka(tmp_codebase.parent / "cmocka")
    prebuildcmd = (
        "ACLOCAL_PATH=$(dirname $(dirname $(command -v libtoolize)))/share/aclocal autoreconf -fi"
        f" && PKG_CONFIG_PATH={cmocka_pkgconfig}:$PKG_CONFIG_PATH"
        f" ./configure CC=cc {' '.join(configure_args)}"
    )

    # Only api/ is built under interception, so libdaq.so is Tenjin's single target;
    # building everything would instead give a multi-target codebase (the library, one
    # .la per module, and two flavors of the example program), which disables the
    # non-trivial-refactoring passes. The modules and example/daqtest stay C and are
    # built afterwards against whichever libdaq.so is in place, which is what lets the
    # same C driver exercise the C and the Rust library.
    translation.do_translate(
        translation_types.TranslationFlags.simple(
            root=tenjin_fixtures.root,
            codebase=tmp_codebase,
            resultsdir=tmp_resultsdir,
            cratename="snort3_libdaq",
            prebuildcmd=prebuildcmd,
            buildcmd="make -C api",
        ),
        guidance_path_or_literal="{}",
    )

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    builddir = tmp_resultsdir / "_build_1"
    hermetic.run(["make", "-C", "modules"], cwd=str(builddir), check=True)
    hermetic.run(["make", "-C", "example"], cwd=str(builddir), check=True)
    # The unit tests are automake check_PROGRAMS, so `make all` skips them. They are
    # built rather than run via `make check` so that each program's exit status and
    # output can be compared directly between the C and the Rust library.
    #
    # test/mock_stdio.c calls glibc's __vsnprintf_chk / __vprintf_chk / __vfprintf_chk,
    # which bits/stdio2.h only declares when _FORTIFY_SOURCE is set. Tenjin's clang does
    # not turn it on by default the way a distro gcc typically does.
    hermetic.run(
        [
            "make",
            "-C",
            "test",
            "api_base_test",
            "api_config_test",
            "CPPFLAGS=-D_FORTIFY_SOURCE=2",
        ],
        cwd=str(builddir),
        check=True,
    )

    (builddir / "daqtest_input.pcap").write_bytes(snort3_libdaq_savefile_capture())

    # Paths are relative to `builddir` so that daqtest's echo of its own configuration
    # is identical between the two runs below. `example/daqtest` is libtool's wrapper
    # script, which points the real binary in example/.libs at api/.libs.
    def run_daqtest() -> bytes:
        return hermetic.run(
            [
                "example/daqtest",
                "-d",
                "savefile",
                "-m",
                "modules/savefile/.libs",
                "-i",
                "daqtest_input.pcap",
                "-M",
                "read-file",
            ],
            cwd=str(builddir),
            check=True,
            capture_output=True,
        ).stdout

    def run_unit_tests() -> dict[str, tuple[int, bytes, bytes]]:
        results = {}
        for program in ("api_base_test", "api_config_test"):
            completed = hermetic.run(
                [f"./{program}"],
                cwd=str(builddir / "test"),
                check=False,
                capture_output=True,
            )
            results[program] = (completed.returncode, completed.stdout, completed.stderr)
        return results

    def normalize(stdout: bytes) -> bytes:
        # daqtest seeds its RNG from time+pid and invents a local MAC address at
        # startup, so that one line differs between any two runs.
        return re.sub(rb"(?m)^Local MAC Address: .*$", b"Local MAC Address: <random>", stdout)

    c_prog_output = run_daqtest()

    # Confirm the C program really decoded the generated capture before using its
    # output as the oracle for the Rust build.
    decode_start = c_prog_output.index(b"Packet 1: ")
    decode_end = c_prog_output.index(b"Read the entire file!")
    assert c_prog_output[decode_start:decode_end] == (
        b"Packet 1: Size = 51/51, Ingress = -1 (Group = -1), Egress = -1 (Group = -1),"
        b" Addr Space ID = 0\n"
        b"MAC: 02:00:00:00:00:01 -> 02:00:00:00:00:02 (0800) (51 bytes)\n"
        b" IP: 10.0.0.1 -> 10.0.0.2 (37 bytes) (checksum: 37460) (protocol: 17)\n"
        b"  UDP: 1234 -> 5678  Checksum 46965  (17 bytes of data)\n"
        b"\n"
        b"Packet 2: Size = 50/50, Ingress = -1 (Group = -1), Egress = -1 (Group = -1),"
        b" Addr Space ID = 0\n"
        b"MAC: 02:00:00:00:00:01 -> 02:00:00:00:00:02 (0800) (50 bytes)\n"
        b" IP: 10.0.0.1 -> 10.0.0.2 (36 bytes) (checksum: 41812) (protocol: 1)\n"
        b"  ICMP: Type 8  Code 0  Checksum 26726  (8 bytes of data)\n"
        b"   Echo: ID 1  Sequence 1\n"
        b"\n"
        b"Packet 3: Size = 60/60, Ingress = -1 (Group = -1), Egress = -1 (Group = -1),"
        b" Addr Space ID = 0\n"
        b"MAC: 02:00:00:00:00:01 -> 02:00:00:00:00:02 (0800) (60 bytes)\n"
        b" IP: 10.0.0.1 -> 10.0.0.2 (46 bytes) (checksum: 35156) (protocol: 17)\n"
        b"  UDP: 4321 -> 8765  Checksum 40637  (26 bytes of data)\n"
    ), f"Got: {c_prog_output!r}"
    assert b"\n  Packets Received:   3\n" in c_prog_output, f"Got: {c_prog_output!r}"

    c_unit_tests = run_unit_tests()

    # cmocka splits its report across streams: the per-case [ RUN ]/[ OK ] lines go to
    # stdout, the [ PASSED ]/[ FAILED ] summary to stderr.
    _, _, c_config_stderr = c_unit_tests["api_config_test"]
    assert c_unit_tests["api_config_test"][0] == 0, (
        f"api_config_test failed against the C library: {c_unit_tests['api_config_test']!r}"
    )
    assert b"[  PASSED  ] 7 test(s)." in c_config_stderr, f"Got: {c_config_stderr!r}"

    # api_base_test mocks printf/opendir/dlopen and friends via the linker's --wrap,
    # which only redirects calls made from the test's own objects; the calls libdaq.so
    # makes internally still reach the real libc. Three of its four cases therefore fail
    # regardless of what language libdaq is written in -- upstream's `make check` only
    # passes with libdaq linked statically, which would defeat the point of swapping the
    # shared library below. Its pass/fail counts are left to the C-versus-Rust comparison
    # rather than pinned here, so this keeps working if that ever changes.
    #
    # Only the stderr summary is a usable liveness check: once a case turns the printf
    # mock on, cmocka's own [ RUN ] lines land in the mock's capture buffer instead of
    # stdout, so api_base_test never prints a "N test(s) run." tally.
    base_status, _, base_stderr = c_unit_tests["api_base_test"]
    assert base_status >= 0, f"api_base_test died on signal {-base_status}"
    assert b"[  PASSED  ]" in base_stderr, f"Got: {base_stderr!r}"

    # Copy the Rust shared library over the C version and re-run the same C driver.
    # daqtest links against the `libdaq.so.3` soname, so confirm that name still
    # resolves to the file being replaced -- otherwise the swap below would be a
    # no-op and the comparison would pass without ever running any Rust.
    c_shared_lib = builddir / "api" / ".libs" / "libdaq.so.3.0.0"
    assert (builddir / "api" / ".libs" / "libdaq.so.3").resolve() == c_shared_lib
    c_shared_lib_digest = sha256hex(c_shared_lib)
    shutil.copyfile(
        tmp_resultsdir / "final" / "target" / "debug" / "libdaq_3_0_0.so",
        c_shared_lib,
    )
    # Everything below re-runs the same C binaries and re-resolves this file, so the
    # runs are only telling us anything if its contents actually changed here.
    assert sha256hex(c_shared_lib) != c_shared_lib_digest, (
        "the Rust library did not replace the C one; every comparison below would"
        " be comparing the C library against itself"
    )
    rs_prog_output = run_daqtest()

    assert normalize(rs_prog_output) == normalize(c_prog_output), (
        f"Rust and C output differed; Rust output was: {rs_prog_output!r}"
    )

    rs_unit_tests = run_unit_tests()
    for program, c_result in c_unit_tests.items():
        assert rs_unit_tests[program] == c_result, (
            f"Rust and C runs of {program} differed;"
            f" Rust gave {rs_unit_tests[program]!r}, C gave {c_result!r}"
        )

    clean_up_resultsdir(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)
