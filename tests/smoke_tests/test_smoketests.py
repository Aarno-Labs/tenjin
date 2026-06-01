import json
import platform
import pytest
import sys

from tenjin_pytest_helpers import (
    annotate_pytest_request_with_translation_notes,
    assert_final_had_no_unsafe_fns,
    run_cargo_on_final,
)
import covset
import hermetic
import translation
import translation_preparation
import translation_multi_config


def test_single_c_file(test_dir, test_tmp_dir, tenjin_fixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = test_dir / "single_c_file" / "main.c"

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Ensure it compiles and runs as expected
    hermetic.run(
        ["clang", "-o", str(test_tmp_dir / "main"), str(tmp_codebase / "main.c")], check=True
    )
    c_prog_output = hermetic.run([str(test_tmp_dir / "main")], check=True, capture_output=True)
    assert c_prog_output.stdout == b"Hello, Tenjin!\n"

    guidance = """{
        "vars_of_type": {"&[u8]": ["get_bits:p"]}
    }"""

    translation.do_translate(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="single_c_file",
        guidance_path_or_literal=guidance,
    )

    assert (tmp_resultsdir / "final" / "Cargo.toml").exists()

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(tmp_resultsdir / "final", ["run"], capture_output=True)

    assert rs_prog_output.stdout == c_prog_output.stdout

    # Check that code coverage is collected as expected
    cp = covset.generate_via(
        None,
        tmp_codebase,
        tmp_resultsdir,
        test_tmp_dir / "x.0.cov.json",
        html=False,
        rust=False,
        rest=[],
    )
    cp.check_returncode()

    cp_ce = hermetic.run(
        [
            (tenjin_fixtures.root / "cli" / "10j").as_posix(),
            "covset-eval",
            "-o",
            "/dev/null",
            f"(show {str(test_tmp_dir / 'x.0.cov.json')})",
        ],
        check=True,
        capture_output=True,
    )
    # strip file header line, which has a path that varies per run
    output_lines = cp_ce.stdout.decode("utf-8").splitlines()
    output_relevant = "\n".join(output_lines[2:]) + "\n"
    assert (
        output_relevant
        == """----------------------------------------
  #include <stdio.h>
  #include <assert.h>
  
- int get_bits(const short *p, int n) {
-     int next, cache = 0, s = n & 7;
-     int shl = n + s;
-     next = *p++ & (255 >> s);  // <- pointer modified
-     while ((shl-= 8) > 0) {
-         cache |= next << shl;
-         next = *p++;           // <- pointer modified
-     }
-     return cache | (next >> -shl);
- }
  
  int main(int argc, char** argv)
+ {
+ 	printf("Hello, Tenjin!\\n");
+ 	assert(1);
  
+ 	if (argc > 2) {
-           printf("  (more than two args provided)\\n");
- 	}
+ 	return 0;
+ }

========================================
Total covered lines: 6 / 18 = 33.33%
"""  # noqa: W293
    )

    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def test_chkc_trivial(test_dir, tmp_codebase):
    """Test that chkc can parse, analyze, and report on a trivial C file."""
    codebase = test_dir / "chkc_trivial" / "main.c"
    target = tmp_codebase / "main.c"
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    hermetic.run_chkc(["c-file", "parse", str(target)], check=True)
    hermetic.run_chkc(["c-file", "analyze", str(target)], check=True)
    hermetic.run_chkc(["c-file", "report", str(target)], check=True)


def test_cmake_lone_exe(test_dir, test_tmp_dir, tenjin_fixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir

    codebase = test_dir / "cmake_lone_exe"
    build_dir = test_tmp_dir / "build"

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Ensure it compiles and runs as expected
    hermetic.run(["cmake", "-B", str(build_dir), "-S", str(tmp_codebase)], check=True)
    hermetic.run(["cmake", "--build", str(build_dir)], check=True)
    c_prog_output = hermetic.run(
        [str(build_dir / "tenjin_smoke_test_lone_exe")], capture_output=True
    )
    assert c_prog_output.returncode == 0, (
        f"Program exited with code {c_prog_output.returncode}, stderr: {c_prog_output.stderr!r}"
    )
    assert c_prog_output.stdout == b"Hello, Tenjin! 43\n", f"Got: {c_prog_output.stdout!r}"

    # Run translation
    translation.do_translate(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        cratename=codebase.name,
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(tmp_resultsdir / "final", ["run"], capture_output=True)

    assert rs_prog_output.stdout == c_prog_output.stdout

    main_rses = list((tmp_resultsdir / "final").glob("**/src/main*.rs"))
    assert len(main_rses) == 1, "No main.rs file found"

    # float_to_bits should be converted from unsafe union to safe Rust method.
    assert ".to_bits()" in main_rses[0].read_text(encoding="utf-8")

    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def test_exe_dylibs(test_dir, test_tmp_dir, tenjin_fixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir

    codebase = test_dir / "exe_dylibs_make"
    c_build_dir = test_tmp_dir / "build"

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Ensure it compiles and runs as expected
    translation_preparation.copy_codebase(codebase, c_build_dir)
    hermetic.run(["make", "-C", str(c_build_dir)], check=True)
    c_prog_output = hermetic.run(["./foo"], check=True, capture_output=True, cwd=c_build_dir)
    assert c_prog_output.stdout == b"Hello, Tenjin! 42 99\n", f"Got: {c_prog_output.stdout!r}"

    # Run translation
    translation.do_translate(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        cratename=codebase.name,
        buildcmd="make",
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(tmp_resultsdir / "final", ["run"], capture_output=True)

    assert rs_prog_output.stdout == c_prog_output.stdout

    libname = "distinct"

    r_build_dir = tmp_resultsdir / "final" / "target" / "debug"
    so_suffix = ".dylib" if platform.system() == "Darwin" else ".so"
    assert (c_build_dir / f"lib{libname}.so").exists()
    assert (r_build_dir / f"lib{libname}{so_suffix}").exists()

    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def test_triplicated_compilation(test_dir, tenjin_fixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    codebase = test_dir / "triplicated_exeonly"
    # Run translation
    translation.do_translate(
        tenjin_fixtures.root,
        codebase,
        tmp_resultsdir,
        cratename="triplicated_exeonly",
        guidance_path_or_literal="{}",
        buildcmd="make",
    )

    # Build the C program
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    hermetic.run(["make"], cwd=tmp_codebase, check=True)

    # (A) Check that the compiled executable has a return code of 40
    c_prog_result = hermetic.run([str(tmp_codebase / "program")], check=False, capture_output=True)
    assert c_prog_result.returncode == 40, (
        f"Expected return code 40, got {c_prog_result.returncode}"
    )

    # (B) Deduplicated compilation database should have three differently-named files
    compdb_dedup_path = tmp_resultsdir / "c_03_uniquify_built" / "compile_commands.json"
    assert compdb_dedup_path.exists(), f"Deduplicated compdb not found at {compdb_dedup_path}"

    with open(compdb_dedup_path, "r", encoding="utf-8") as f:
        compdb_dedup = json.load(f)

    dedup_files = [entry.get("file", "") for entry in compdb_dedup]
    assert len(dedup_files) == 3, (
        f"Expected 3 files in deduplicated compdb, found {len(dedup_files)}"
    )
    assert len(set(dedup_files)) == 3, (
        f"Expected 3 different files, found duplicates: {dedup_files}"
    )

    # Verify they're not all named source.c
    assert not all("source.c" in f for f in dedup_files), (
        "Deduplicated files should have different names"
    )

    # (C) Check that the translated Rust code also returns 40
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_result = hermetic.run_cargo_on_translated_code(
        ["run"],
        cwd=tmp_resultsdir / "final",
        check=False,
        capture_output=True,
    )
    assert rs_prog_result.returncode == 40, (
        f"Expected Rust return code 40, got {rs_prog_result.returncode}"
    )

    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def test_trivial_un_unsafe(test_dir, tenjin_fixtures):
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir

    codebase = test_dir / "trivial_un_unsafe"
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    translation.do_translate(
        tenjin_fixtures.root,
        codebase,
        tmp_resultsdir,
        cratename="safe",
        guidance_path_or_literal='{ "public_api" : [] }',
    )

    assert_final_had_no_unsafe_fns(tmp_resultsdir)
    annotate_pytest_request_with_translation_notes(tenjin_fixtures)


def test_example_P02_configuration_single(test_dir, test_tmp_dir, tenjin_fixtures):
    codebase = test_dir / "example_P02_configuration" / "config_notests"
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    default_build_dir = test_tmp_dir / "build_default"
    hermetic.run(
        [
            "cmake",
            "-DAPP_MODE=fast",
            "-DBACKEND=alpha",
            "-DWORD_SIZE=32",
            "-B",
            str(default_build_dir),
            "-S",
            str(tmp_codebase),
        ],
        check=True,
    )
    hermetic.run(["cmake", "--build", str(default_build_dir)], check=True)
    c_default_output = hermetic.run(
        [str(default_build_dir / "test_case" / "skeleton")], capture_output=True
    )

    translation.do_translate(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        do_not_refactor_headers_within=[],
        cratename="triplicated_exeonly",
        guidance_path_or_literal="{}",
        buildcmd="make",
        cmake_defines=["APP_MODE=fast", "BACKEND=alpha", "WORD_SIZE=32"],
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    default_output = run_cargo_on_final(tmp_resultsdir / "final", ["run"], capture_output=True)

    assert c_default_output.stdout == default_output.stdout


# Disabled because crat-merge is only built for linux
@pytest.mark.skipif(sys.platform == "darwin", reason="Skipping on macOS.")
def test_example_P02_configuration(test_dir, test_tmp_dir, tenjin_fixtures):
    codebase = test_dir / "example_P02_configuration" / "config_notests"
    tmp_codebase, tmp_resultsdir = tenjin_fixtures.tmp_codebase, tenjin_fixtures.tmp_resultsdir
    translation_preparation.copy_codebase(codebase, tmp_codebase)

    config_path = tmp_codebase / "test_case" / "configuration.json"
    # Run translation
    translation_multi_config.do_translate_multi_config(
        tenjin_fixtures.root,
        tmp_codebase,
        tmp_resultsdir,
        config_path,
        do_not_refactor=[],
        cratename="triplicated_exeonly",
        guidance_str="{}",
        buildcmd="make",
        cmake_defines=[],
        jobs=8,
        cmake_presets_path=None,
    )

    # Build C/Rust with "default" configuration, compare stdout
    default_build_dir = test_tmp_dir / "build_default"
    hermetic.run(
        [
            "cmake",
            "-DAPP_MODE=fast",
            "-DBACKEND=alpha",
            "-DWORD_SIZE=32",
            "-B",
            str(default_build_dir),
            "-S",
            str(tmp_codebase),
        ],
        check=True,
    )
    hermetic.run(["cmake", "--build", str(default_build_dir)], check=True)
    c_default_output = hermetic.run(
        [str(default_build_dir / "test_case" / "skeleton")], capture_output=True
    )
    full_features = "APP_MODE_fast,BACKEND_alpha,WORD_SIZE_32"
    run_cargo_on_final(tmp_resultsdir / "merged", ["build", "--features", full_features])
    default_output = run_cargo_on_final(
        tmp_resultsdir / "merged", ["run", "--features", full_features], capture_output=True
    )
    assert default_output.stdout == c_default_output.stdout

    # Build C/Rust with "full" configuration, compare output
    full_build_dir = test_tmp_dir / "full_build_dir"
    hermetic.run(
        [
            "cmake",
            "-DAPP_MODE=safe",
            "-DBACKEND=beta",
            "-DWORD_SIZE=64",
            "-DENABLE_EXTRA=On",
            "-B",
            str(full_build_dir),
            "-S",
            str(tmp_codebase),
        ],
        check=True,
    )
    hermetic.run(["cmake", "--build", str(full_build_dir)], check=True)
    c_full_output = hermetic.run(
        [str(full_build_dir / "test_case" / "skeleton")], capture_output=True
    )
    full_features = "APP_MODE_safe,BACKEND_beta,WORD_SIZE_64,ENABLE_EXTRA"
    run_cargo_on_final(tmp_resultsdir / "merged", ["build", "--features", full_features])
    full_output = run_cargo_on_final(
        tmp_resultsdir / "merged", ["run", "--features", full_features], capture_output=True
    )

    assert full_output.stdout == c_full_output.stdout
