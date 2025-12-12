from pathlib import Path
import translation_preparation
import translation
import ingest_tracking
import hermetic
import repo_root
from testing import *

def run_cargo_on_final(cwd: Path, args: list[str], capture_output: bool = False):
    return hermetic.run_cargo_on_translated_code(
        args,
        cwd=cwd,
        check=True,
        capture_output=capture_output,
    )

def test_smoketest0(test_dir, test_tmp_dir, tmp_codebase, tmp_resultsdir):
  root = repo_root.find_repo_root_dir_Path()
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

  run_cargo_on_final(tmp_resultsdir/"final", ["build"])
  rs_prog_output = run_cargo_on_final(tmp_resultsdir/"final", ["run"], capture_output=True)

  assert rs_prog_output.stdout == c_prog_output.stdout

def test_smoketest1(test_dir, tmp_codebase):
  codebase = test_dir / "test_1" / "main.c"
  target = tmp_codebase / "main.c"
  translation_preparation.copy_codebase(codebase, tmp_codebase)
  hermetic.run_chkc(["c-file", "parse", str(target)], check=True)
  hermetic.run_chkc(["c-file", "analyze", str(target)], check=True)
  hermetic.run_chkc(["c-file", "report", str(target)], check=True)

def test_smoketest2(test_dir, tmp_codebase, tmp_resultsdir):
  codebase = test_dir / "test_2"
  root = repo_root.find_repo_root_dir_Path()
  translation_preparation.copy_codebase(codebase, tmp_codebase)
  # For reasons I don't understand yet, intercept-build sometimes creates an empty
  # compile_commands.json on macOS.
  hermetic.run(
      ["clang", "-c", "a.c", "b.c", "main.c", "-MJ", "compile_commands.json"],
      cwd=tmp_codebase,
      check=True,
  )
  # -MJ produces a comma-terminated list; fix it to be valid JSON
  compdb_path = tmp_codebase / "compile_commands.json"
  compdb_path.write_text("[\n" + compdb_path.read_text().rstrip(",\n") + "\n]\n")
  hermetic.run(["make"], cwd=tmp_codebase, check=True)
  assert (tmp_codebase / "compile_commands.json").exists()

  # Build and run the C program
  c_prog_output = hermetic.run(
      [str(tmp_codebase / "made_main")],
      check=True,
      capture_output=True,
  )

  translation.do_translate(
        root,
        tmp_codebase,
        tmp_resultsdir,
        cratename="smoke_test_2",
        guidance_path_or_literal="{}",
  )

  assert (tmp_resultsdir / "final" / "Cargo.toml").exists()

  run_cargo_on_final(tmp_resultsdir/"final", ["build"])
  rs_prog_output = run_cargo_on_final(tmp_resultsdir/"final", ["run"], capture_output=True)

  assert rs_prog_output.stdout == c_prog_output.stdout