import json
from pathlib import Path
from subprocess import CalledProcessError, CompletedProcess

import pytest

import ingest_tracking
import repo_root
import translation_improvement


def test_synsub_leaves_print_macros_alone(monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    rewrites: list[tuple[str, str]] = []

    monkeypatch.setattr(
        translation_improvement,
        "run_ast_grep_rewrite",
        lambda _dir, pattern, rewrite: rewrites.append((pattern, rewrite)),
    )
    monkeypatch.setattr(
        translation_improvement,
        "run_built_workspace_binary",
        lambda *_args: CompletedProcess([], 1),
    )

    cp = translation_improvement.run_improve_synsub(tmp_path, [], tmp_path)

    assert cp.returncode == 1
    print_macro_rewrites = set(translation_improvement.PRINT_MACRO_REWRITES)
    assert not any(
        (a, b) in print_macro_rewrites or (b, a) in print_macro_rewrites for a, b in rewrites
    )


def test_exceptional_lift_restores_temporarily_rewritten_print_macros(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
):
    rewrites: list[tuple[str, str]] = []

    monkeypatch.setattr(
        translation_improvement,
        "run_ast_grep_rewrite",
        lambda _dir, pattern, rewrite: rewrites.append((pattern, rewrite)),
    )

    def fail(*_args):
        raise OSError("could not launch lift-call-args")

    monkeypatch.setattr(translation_improvement, "run_built_workspace_binary", fail)

    with pytest.raises(OSError, match="could not launch"):
        translation_improvement.run_improve_lift_call_args(tmp_path, [], tmp_path)

    assert rewrites == [
        *translation_improvement.PRINT_MACRO_REWRITES,
        *((call, macro) for macro, call in translation_improvement.PRINT_MACRO_REWRITES),
    ]


@pytest.mark.parametrize("symlink_root", [False, True])
def test_lift_call_args_only_rewrites_files_inside_source_directory(
    tmp_path: Path, symlink_root: bool, capfd: pytest.CaptureFixture[str]
):
    (tmp_path / "Cargo.toml").write_text(
        '[workspace]\nmembers = ["selected", "selected-other"]\nresolver = "2"\n',
        encoding="utf-8",
    )
    original = (
        "fn consume(_: &mut i32, _: i32) {}\n"
        "pub fn needs_lift(value: &mut i32) { consume(value, *value); }\n"
    )
    for name in ("selected", "selected-other"):
        crate = tmp_path / name
        (crate / "src").mkdir(parents=True)
        (crate / "Cargo.toml").write_text(
            f'[package]\nname = "{name}"\nversion = "0.1.0"\nedition = "2021"\n',
            encoding="utf-8",
        )
        (crate / "src" / "lib.rs").write_text(original, encoding="utf-8")

    crate = tmp_path / "selected"
    source = crate / "src" / "lib.rs"
    source.write_text(original + "mod external;\n", encoding="utf-8")
    outside = tmp_path / "outside.rs"
    outside.write_text(original, encoding="utf-8")
    (crate / "src" / "external.rs").symlink_to(outside)
    if symlink_root:
        alias = tmp_path / "alias"
        alias.symlink_to(crate, target_is_directory=True)
        crate = alias

    root = repo_root.find_repo_root_dir_Path()
    cp = translation_improvement.run_built_workspace_binary(
        root, "xj-improve-lift-call-args", "xj-improve-lift-call-args", [], tmp_path
    )
    cp.check_returncode()
    output = capfd.readouterr().out
    assert "selected-other/src/lib.rs" in output
    assert "selected/src/external.rs" in output or "outside.rs" in output

    for args in ([], ["--modify-in-place"]):
        cp = translation_improvement.run_built_workspace_binary(
            root, "xj-improve-lift-call-args", "xj-improve-lift-call-args", args, crate
        )
        cp.check_returncode()
        output = capfd.readouterr().out
        if not args:
            assert "__lift_" in output
            assert "selected-other/src/lib.rs" not in output
            assert "external.rs" not in output
            assert "outside.rs" not in output
            assert source.read_text(encoding="utf-8") == original + "mod external;\n"
        assert outside.read_text(encoding="utf-8") == original
        assert (tmp_path / "selected-other" / "src" / "lib.rs").read_text(
            encoding="utf-8"
        ) == original

    assert "__lift_" in source.read_text(encoding="utf-8")


def test_failed_improvement_stage_is_not_selected_as_output(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
):
    initial = tmp_path / "00_out"
    initial.mkdir()
    (initial / "result.rs").write_text("last successful output\n", encoding="utf-8")

    def fail_synsub(_root: Path, _args: list[str], output: Path) -> CompletedProcess:
        (output / "result.rs").write_text("failed output\n", encoding="utf-8")
        return CompletedProcess([], 1)

    monkeypatch.setattr(translation_improvement, "run_improve_synsub", fail_synsub)

    selected = translation_improvement.run_improvement_passes(
        tmp_path,
        initial,
        tmp_path,
        ingest_tracking.TimingRepo(None),
    )

    assert selected == initial
    assert (tmp_path / "01_synsub" / "result.rs").read_text(encoding="utf-8") == ("failed output\n")


def test_multitool_reports_compilation_failure_before_analysis(tmp_path: Path):
    crate = tmp_path / "crate"
    source = crate / "src" / "main.rs"
    source.parent.mkdir(parents=True)
    (crate / "Cargo.toml").write_text(
        '[package]\nname = "multitool_analysis_failure"\nversion = "0.1.0"\nedition = "2021"\n',
        encoding="utf-8",
    )
    source.write_text("fn dead() {}\nfn main() { missing_symbol(); }\n", encoding="utf-8")

    root = repo_root.find_repo_root_dir_Path()
    with pytest.raises(CalledProcessError) as error:
        translation_improvement.run_improve_multitool(
            root, "TrimDeadItems", ["--modify-in-place"], crate
        )
    assert b"did not reach after_analysis" in error.value.stderr
    assert source.read_text(encoding="utf-8").startswith("fn dead()")

    source.write_text("fn dead() {}\nfn main() {}\n", encoding="utf-8")
    translation_improvement.run_improve_multitool(
        root, "TrimDeadItems", ["--modify-in-place"], crate
    )
    assert "fn dead()" not in source.read_text(encoding="utf-8")


def test_cacg_spans_use_original_offsets_after_legacy_intrinsic_rewrite(tmp_path: Path):
    crate = tmp_path / "crate"
    source = crate / "src" / "main.rs"
    source.parent.mkdir(parents=True)
    (crate / "Cargo.toml").write_text(
        '[package]\nname = "cacg_legacy_intrinsic"\nversion = "0.1.0"\nedition = "2021"\n',
        encoding="utf-8",
    )
    source.write_text(
        "#![feature(core_intrinsics)]\n"
        "unsafe fn before(ptr: *mut i32) {\n"
        "    ::core::intrinsics::atomic_cxchg_seqcst_seqcst(ptr, 0, 1);\n"
        "}\n"
        "fn after() {}\n"
        "fn main() {}\n",
        encoding="utf-8",
    )
    output = tmp_path / "cacg"
    output.mkdir()

    translation_improvement.run_improve_multitool(
        repo_root.find_repo_root_dir_Path(),
        "ExtractCACG",
        ["--cacg-json-outdir", str(output)],
        crate,
    )

    graph = json.loads(next(output.glob("*.cacg.json")).read_text(encoding="utf-8"))
    contents = source.read_bytes()
    spans = [
        contents[span["lo"] : span["hi"]]
        for span in graph["elts"]
        if Path(graph["files"][span["fileid"]]).name == "main.rs"
    ]
    assert b"fn after() {}" in spans
