import json
from pathlib import Path
from subprocess import CalledProcessError, CompletedProcess

import pytest

import ingest_tracking
import repo_root
import translation_improvement


def test_failed_synsub_restores_temporarily_rewritten_print_macros(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
):
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
    assert rewrites[-4:] == [
        (call, macro) for macro, call in translation_improvement.PRINT_MACRO_REWRITES
    ]


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
        (call, macro) for macro, call in translation_improvement.PRINT_MACRO_REWRITES
    ]


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
