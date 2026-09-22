from pathlib import Path
from subprocess import CompletedProcess

import pytest

import ingest_tracking
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
