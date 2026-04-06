import json
from pathlib import Path
import shutil
import pytest
import os
import resource
from subprocess import SubprocessError

from tenjin_pytest_helpers import (
    annotate_pytest_request_with_translation_notes,
    cached_git_clone_at_commit,
    run_cargo_on_final,
    run_tractor_test_vector,
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


@pytest.mark.slow  # expected runtime: 540 seconds (~9 minutes)
def test_Old_Man_Programmer__tree_2_3_2(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    extras: list,
):
    codebase = cached_git_clone_at_commit(
        "https://github.com/brk/Old-Man-Programmer__tree.git",
        "3f3077dbd87fc89396c8dc74fcf7920ec8b0c7d5",
    )
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    buildcmd_args = ["make"]
    translation.do_translate(
        root,
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

    annotate_pytest_request_with_translation_notes(request, tmp_resultsdir, extras)


# Expected runtime: 10 s
def test_url_h_aka_urlparser(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    extras: list,
):
    codebase = cached_git_clone_at_commit(
        "https://github.com/jwerle/url.h.git", "752635e46be6b13ad045f7216a28417fdf533950"
    )

    translation_preparation.copy_codebase(codebase, tmp_codebase)
    buildcmd_args = ["make", "url-test"]

    translation.do_translate(
        root,
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


# This test requires <openssl/conf.h> to compile and dynamically links against `libcrypto`.
# On Mac, we should query `brew --prefix openssl`.
# It also appears that setrlimit doesn't work properly on Mac, so anyone running this test
# would need to set `ulimit -s 32000` before invoking pytest.
@pytest.mark.slow
def test_tractor_ta3_corpus_p01_005(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    extras: list,
):
    codebase = tractor_tests_git_clone_for("Public-Tests/P01_sphincs_plus")

    translation_preparation.copy_codebase(
        codebase
        / "Public-Tests"
        / "P01_sphincs_plus"
        / "005_sphincs_PQCgenKAT_sign_blake_128f_simple",
        tmp_codebase,
    )
    exe_name = "PQCgenKAT_sign"

    # blake256.c has a very large function (~1600 statements) which causes c2rust to
    # blow the stack with the default 8MB limit.
    try:
        mb_32 = 32 * 1024 * 1024
        resource.setrlimit(resource.RLIMIT_STACK, (mb_32, mb_32))
    except ValueError:
        return pytest.skip(
            "P01 requires a large stack which we can't set up on this platform; skipping test.",
        )

    os.environ["XJ_CMAKE_PRESET"] = "test"  # without this, we'll compile the wrong code
    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="tractor_ta3_corpus_p01",
        guidance_path_or_literal="{}",
    )

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])

    # The P01 build may or may not have a `_nolines` suffix; currently it will not
    # but let's not have this test break if that changes in the future.
    rs_bins = list((tmp_resultsdir / "final" / "target" / "debug").glob(f"{exe_name}*"))
    rs_bins = [p for p in rs_bins if p.is_file() and os.access(p, os.X_OK)]
    assert len(rs_bins) == 1, (
        f"Expected exactly one binary in {tmp_resultsdir / 'final' / 'target' / 'debug'}, but found: {[p.name for p in rs_bins]}"
    )
    rs_bin = rs_bins[0]

    test_vector = tmp_codebase / "test_vectors" / "test.json"
    spec = json.loads(test_vector.read_text(encoding="utf-8"))

    os.environ["XJ_LD_SYSROOT"] = (
        "1"  # Note: P01 dynamically links against libcrypto; this makes it available.
    )

    outcome_rs = run_tractor_test_vector(
        rs_bin, test_vector.stem, spec, cwd=tmp_resultsdir / "final"
    )
    assert outcome_rs.ok, (
        f"Test vector {test_vector.stem} failed on the Rust version: {outcome_rs.message}"
    )

    annotate_pytest_request_with_translation_notes(request, tmp_resultsdir, extras)


def translate_and_build_ta3_test(
    root: Path,
    cratename: str,
    orig_codebase: Path,
    tmp_codebase: Path,
    case_dir: str,
    resultsdir: Path,
    monkeypatch: pytest.MonkeyPatch,
):
    if (orig_codebase / case_dir / "CMakePresets.json").is_file():
        monkeypatch.setenv("XJ_CMAKE_PRESET", "test")  # without this, we'll compile the wrong code
        assert not (orig_codebase / case_dir / "test_case" / "CMakePresets.json").is_file(), (
            f"Found unexpected CMakePresets.json in the test_case directory {orig_codebase / case_dir / 'test_case'}."
        )
        shutil.copyfile(
            orig_codebase / case_dir / "CMakePresets.json",
            tmp_codebase / "test_case" / "CMakePresets.json",
        )
    translation.do_translate(
        root,
        tmp_codebase / "test_case",
        resultsdir,
        cratename,
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(resultsdir / "final", ["build"])


def eval_tractor_ta3_corpus_app(
    root: Path,
    tmp_codebase: Path,
    tmp_resultsdir: Path,
    request: pytest.FixtureRequest,
    monkeypatch: pytest.MonkeyPatch,
    extras: list,
    case_dir: str,
):
    try:
        codebase = tractor_tests_git_clone_for(case_dir)
    except SubprocessError:
        return pytest.skip(
            f"Could not clone repo for {case_dir}; likely requires authentication. Skipping test."
        )

    # Copying the whole Test-Corpus repo results in huge numbers of temporary files,
    # resulting in noticeable delays both for test steps and for post-test cleanups.
    translation_preparation.copy_codebase(codebase / case_dir, tmp_codebase)

    exe_name = "driver"  # Some test vectors require the binary to be named "driver".
    translate_and_build_ta3_test(
        root,
        cratename="tractor_ta3_corpus_app",
        orig_codebase=codebase,
        tmp_codebase=tmp_codebase,
        case_dir=case_dir,
        resultsdir=tmp_resultsdir,
        monkeypatch=monkeypatch,
    )

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
    monkeypatch: pytest.MonkeyPatch,
    extras: list,
    case_dir: str,
):
    # cando2 requires the candidate name (e.g. "collided_lib") end in `_lib`.
    candidate_name = Path(case_dir).name
    assert candidate_name.endswith("_lib"), (
        f"Expected case_dir name to end in '_lib', got {candidate_name}"
    )

    try:
        codebase = tractor_tests_git_clone_for(case_dir)
    except SubprocessError:
        return pytest.skip(
            f"Could not clone repo for {case_dir}; likely requires authentication. Skipping test."
        )

    # Copying the whole Test-Corpus repo results in huge numbers of temporary files,
    # resulting in noticeable delays both for test steps and for post-test cleanups.
    translation_preparation.copy_codebase(codebase / case_dir, tmp_codebase)

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

    translate_and_build_ta3_test(
        root,
        cratename="tractor_ta3_corpus_lib",
        orig_codebase=codebase,
        tmp_codebase=tmp_codebase,
        case_dir=case_dir,
        resultsdir=candidate_resultsdir,
        monkeypatch=monkeypatch,
    )

    # cando2 usually requires the built library exist in a `build-ninja` directory
    # and be named `{candidate_name}.so`. Sometimes they hardcode the name `driver.so`.
    # (B02_organic/encode_base64_lib is an example of the latter.)
    build_ninja_dir = Path(candidate_resultsdir / "build-ninja")
    build_ninja_dir.mkdir(exist_ok=False)
    built_dir = candidate_resultsdir / "final" / "target" / "debug"
    built_libs = list(built_dir.glob("lib*.so")) + list(built_dir.glob("lib*.dylib"))
    assert len(built_libs) == 1, (
        f"Expected exactly one built library in {candidate_resultsdir / 'final' / 'target' / 'debug'}, but found: {[p.name for p in built_libs]}"
    )
    built_lib = built_libs[0]
    shutil.copyfile(built_lib, build_ninja_dir / f"lib{candidate_name}{built_lib.suffix}")
    shutil.copyfile(built_lib, build_ninja_dir / f"libdriver{built_lib.suffix}")

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
def test_tractor_b1_organic_collided_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/collided_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_bin2hex_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/bin2hex_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_bitwriter_add_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/bitwriter_add_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_colourblind_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/colourblind_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_contrast_ratio_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/contrast_ratio_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_crc16_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/crc16_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_dequantize_granule_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/dequantize_granule_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_div_euclid_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/div_euclid_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_encode_quant_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/encode_quant_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_flac_validate_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/flac_validate_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_flip_horizontal_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/flip_horizontal_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


def test_tractor_b1_organic_float2half_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/float2half_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_gaussian_kernel_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/gaussian_kernel_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_half2float_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/half2float_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_hdr_bitrate_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hdr_bitrate_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_hdr_compare_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hdr_compare_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_hex2bin_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hex2bin_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_hsl_to_rgb_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hsl_to_rgb_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_hsv_to_rgb_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/hsv_to_rgb_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_ima_parse_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/ima_parse_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_ldexp_q2_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/ldexp_q2_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_max_size_frame_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/max_size_frame_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_md5_digest_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/md5_digest_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_merge_sort_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/merge_sort_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_next_double_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/next_double_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_normalize_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/normalize_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_pow43_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/pow43_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_premultiply_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/premultiply_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_read_side_info_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/read_side_info_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_rev16_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/rev16_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_rgb_to_hsv_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/rgb_to_hsv_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_synth_pair_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/synth_pair_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_tfm_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/tfm_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_to_barycentric_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/to_barycentric_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_tritanopia_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/tritanopia_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_update_frame_header_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/update_frame_header_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_update_md5_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/update_md5_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_wcscat_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_organic/wcscat_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_compress_bc5_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/compress_bc5_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_convex_clip_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/convex_clip_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_decorrelate_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/decorrelate_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_ima_decode_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/ima_decode_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


# This one fails with stacks of < 4 MB.
@pytest.mark.slow
def test_tractor_b1_organic_md5_transform_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/md5_transform_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_png_qsort_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/png_qsort_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_predict_sample_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/predict_sample_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_read_scalefactors_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/read_scalefactors_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_refine_block_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/refine_block_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_organic_stereo_samples_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_organic/stereo_samples_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗     ██╗     █████╗ ██████╗ ██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ███║    ██╔══██╗██╔══██╗██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝     ╚██║    ███████║██████╔╝██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝       ██║    ██╔══██║██╔═══╝ ██╔═══╝ ╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║        ██║    ██║  ██║██║     ██║     ███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝        ╚═╝    ╚═╝  ╚═╝╚═╝     ╚═╝     ╚══════╝


@pytest.mark.slow
def test_tractor_b1_synthetic_002_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/002_stdin_echo"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_003_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/003_string_slicing"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_004_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/004_nineality_sieve"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_005_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/005_static_loop"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_006_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/006_static_alias"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_007_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/007_errno_pow"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


# omitting 008_long_run for now


@pytest.mark.slow
def test_tractor_b1_synthetic_009_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/009_stack_buffer_overflow"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_010_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/010_integer_overflow"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_011_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/011_uninit_char_ptr"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_012_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/012_uninit_int_ptr"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_013_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/013_poor_quality_addition"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_014_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/014_dead_code"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_015_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/015_return_stack_buffer"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_016_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/016_divide_by_zero_float"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_017_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/017_signed_length_confusion"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_018_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/018_stack_buffer_overflow_loop1"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_019_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/019_integer_overflow_char_max_multiply"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_021_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/021_complex_goto"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_022_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/022_stdlib_div"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_023_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/023_struct_and_errno"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_024_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/024_struct_and_static"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_025_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/025_struct_and_errno_and_static"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_026_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/026_goto_and_static"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_027_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/027_ctype_ascii"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_028_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/028_strchr"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


def test_tractor_b1_synthetic_029_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/029_strcspn"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_030_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/030_mutable_buffer_overlap_extrahard"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_031_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/031_disjoint_arrays"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_032_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/032_comma_operator"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.skip(reason="need to pull in fixes for bitfields from upstream c2rust")
@pytest.mark.slow
def test_tractor_b1_synthetic_033_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/033_bitfield"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_034_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/034_cast_to_char_ptr_int"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_035_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/035_cast_to_char_ptr_float"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_036_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/036_cast_to_char_ptr_struct"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_037_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/037_cast_to_char_ptr_int_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_038_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/038_cast_to_char_ptr_float_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_039_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/039_cast_to_char_ptr_struct_no_strict_aliasing"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_040_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/040_storage_class_auto"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_041_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/041_storage_class_register"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.skip(reason="we print hex floats without an explicit + sign")
@pytest.mark.slow
def test_tractor_b1_synthetic_042_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/042_float_union"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_043_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B01_synthetic/043_iso646_and_digraphs"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_002_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/002_echo"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_004_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/004_loop"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_011_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/011_static_dag"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_014_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/014_errno-pow-subfunction"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_016_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/016_switch-arith"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_027_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/027_stack_buffer_overflow_loop2"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_030_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/030_integer_underflow_char_min_multiply"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_045_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/045_strtok"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b1_synthetic_048_app_hidden(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Hidden-Tests/B01_synthetic/048_mutable_buffer_overlap2"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗    ██████╗     ██╗     ██╗██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ╚════██╗    ██║     ██║██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝      █████╔╝    ██║     ██║██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝      ██╔═══╝     ██║     ██║██╔══██╗╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║       ███████╗    ███████╗██║██████╔╝███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝       ╚══════╝    ╚══════╝╚═╝╚═════╝ ╚══════╝


@pytest.mark.slow
def test_tractor_b2_synthetic_arity_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/arity_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_arrayfunc_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/arrayfunc_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_betagamma_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/betagamma_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_buffapp_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/buffapp_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_charinbuf_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/charinbuf_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_checkshift_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/checkshift_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_cleanup_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/cleanup_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_complexmode_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/complexmode_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_dataentry_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/dataentry_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_doubleneg_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/doubleneg_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_fallcalc_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/fallcalc_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_findrep_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/findrep_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_goto_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/goto_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_gotomach_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/gotomach_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_inreftree_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/inreftree_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_jumpnode_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/jumpnode_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_matrix_mult_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/matrix_mult_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_matrix_sum_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/matrix_sum_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_maxnmin_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/maxnmin_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_memchra2_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/memchra2_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_modeselect_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/modeselect_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_overunder_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/overunder_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_task_manager_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/task_manager_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_aabb_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/aabb_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_agglom_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/agglom_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_arr_del_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/arr_del_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_arr_ins_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/arr_ins_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_arr_push_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/arr_push_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_basename_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/basename_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_call_predict_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/call_predict_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_capsule_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/capsule_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_circle_collide_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/circle_collide_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_convert_pix_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/convert_pix_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_decode_base64_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/decode_base64_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_encode_base64_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/encode_base64_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_file_queue_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/file_queue_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_gen_ray_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/gen_ray_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_get_predict_func_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/get_predict_func_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_gjk_cache_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/gjk_cache_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_gjk_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/gjk_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_hm_geti_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/hm_geti_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_intput_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/intput_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_lines_in_buffer_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/lines_in_buffer_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_load_png_mem_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/load_png_mem_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_omni_collide_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/omni_collide_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_omni_manifold_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/omni_manifold_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_parse_number_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/parse_number_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_pinflate_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/pinflate_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_poly_ray_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/poly_ray_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_rdg_genstdout_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/rdg_genstdout_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_reverse_collide_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/reverse_collide_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_search_and_replace_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/search_and_replace_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_siphash_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/siphash_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_underhanded_c_nuke_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/underhanded_c_nuke_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_unfilter_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/unfilter_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_utf8_lib(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/utf8_lib"
    eval_tractor_ta3_corpus_lib(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


# ██████╗  █████╗ ████████╗████████╗███████╗██████╗ ██╗   ██╗    ██████╗     █████╗ ██████╗ ██████╗ ███████╗
# ██╔══██╗██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗╚██╗ ██╔╝    ╚════██╗   ██╔══██╗██╔══██╗██╔══██╗██╔════╝
# ██████╔╝███████║   ██║      ██║   █████╗  ██████╔╝ ╚████╔╝      █████╔╝   ███████║██████╔╝██████╔╝███████╗
# ██╔══██╗██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗  ╚██╔╝      ██╔═══╝    ██╔══██║██╔═══╝ ██╔═══╝ ╚════██║
# ██████╔╝██║  ██║   ██║      ██║   ███████╗██║  ██║   ██║       ███████╗   ██║  ██║██║     ██║     ███████║
# ╚═════╝ ╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝   ╚═╝       ╚══════╝   ╚═╝  ╚═╝╚═╝     ╚═╝     ╚══════╝


@pytest.mark.slow
def test_tractor_b2_synthetic_char_to_bool_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/char_to_bool"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_generic_foreach_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/generic_foreach"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_macrodepth_add_5_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/macrodepth_add_5"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_macrodepth_mul_4_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/macrodepth_mul_4"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_macrodepth_sub_6_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/macrodepth_sub_6"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_memcpy_fun_buffers_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/memcpy_fun_buffers"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_memmove_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/memmove"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_mutable_duplication_dag_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/mutable_duplication_dag"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_strcmp_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/strcmp"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_synthetic_strcpy_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_synthetic/strcpy"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip


@pytest.mark.slow
def test_tractor_b2_organic_qmath_app(root: Path, tmp_codebase: Path, tmp_resultsdir: Path, request: pytest.FixtureRequest, extras: list, monkeypatch: pytest.MonkeyPatch):  # fmt: skip
    case_dir = "Public-Tests/B02_organic/qmath"
    eval_tractor_ta3_corpus_app(root, tmp_codebase, tmp_resultsdir, request, monkeypatch, extras, case_dir)  # fmt: skip
