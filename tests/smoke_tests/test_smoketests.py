import json
from pathlib import Path

import hermetic
import translation
import translation_preparation


def run_cargo_on_final(cwd: Path, args: list[str], capture_output: bool = False):
    return hermetic.run_cargo_on_translated_code(
        args,
        cwd=cwd,
        check=True,
        capture_output=capture_output,
    )


def test_smoketest0(root, test_dir, test_tmp_dir, tmp_codebase, tmp_resultsdir):
    codebase = test_dir / "test_0" / "main.c"

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Ensure it compiles and runs as expected
    hermetic.run(
        ["clang", "-o", str(test_tmp_dir / "main"), str(tmp_codebase / "main.c")], check=True
    )
    c_prog_output = hermetic.run([str(test_tmp_dir / "main")], check=True, capture_output=True)
    assert c_prog_output.stdout == b"Hello, Tenjin!\n"

    # Run translation
    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="smoke_test_0",
        guidance_path_or_literal="{}",
    )

    assert (tmp_resultsdir / "final" / "Cargo.toml").exists()

    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(tmp_resultsdir / "final", ["run"], capture_output=True)

    assert rs_prog_output.stdout == c_prog_output.stdout


def test_smoketest1(test_dir, tmp_codebase):
    codebase = test_dir / "test_1" / "main.c"
    target = tmp_codebase / "main.c"
    translation_preparation.copy_codebase(codebase, tmp_codebase)
    hermetic.run_chkc(["c-file", "parse", str(target)], check=True)
    hermetic.run_chkc(["c-file", "analyze", str(target)], check=True)
    hermetic.run_chkc(["c-file", "report", str(target)], check=True)


def test_smoketest3(root, test_dir, test_tmp_dir, tmp_codebase, tmp_resultsdir):
    codebase = test_dir / "test_03"
    build_dir = test_tmp_dir / "build"

    translation_preparation.copy_codebase(codebase, tmp_codebase)

    # Ensure it compiles and runs as expected
    hermetic.run(["cmake", "-B", str(build_dir), "-S", str(tmp_codebase)], check=True)
    hermetic.run(["cmake", "--build", str(build_dir)], check=True, capture_output=True)
    c_prog_output = hermetic.run(
        [str(build_dir / "tenjin_smoke_test_4")], check=True, capture_output=True
    )
    assert c_prog_output.stdout == b"Hello, Tenjin!\n", f"Got: {c_prog_output.stdout!r}"

    # Run translation
    translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="smoke_test_3",
        guidance_path_or_literal="{}",
    )
    run_cargo_on_final(tmp_resultsdir / "final", ["build"])
    rs_prog_output = run_cargo_on_final(tmp_resultsdir / "final", ["run"], capture_output=True)

    assert rs_prog_output.stdout == c_prog_output.stdout


def test_triplicated_compilation(root, test_dir, tmp_codebase, tmp_resultsdir):
    codebase = test_dir / "triplicated_exeonly"
    # Run translation
    translation.do_translate(
        root,
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
    compdb_dedup_path = tmp_resultsdir / "c_02_uniquify_built" / "compile_commands.json"
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
