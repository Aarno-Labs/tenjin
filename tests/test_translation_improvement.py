from pathlib import Path
from subprocess import CompletedProcess

import pytest

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
