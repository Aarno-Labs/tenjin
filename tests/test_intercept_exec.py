from pathlib import Path

import intercept_exec
import targets_from_intercept


def test_preprocess_dev_null_probe_is_executed_but_not_intercepted(tmp_path, monkeypatch):
    build_commands_dir = tmp_path / "build_commands"
    compiler = tmp_path / "clang"
    compiler.touch()
    calls = []

    monkeypatch.setenv("BUILD_COMMANDS_DIRECTORY", build_commands_dir.as_posix())
    monkeypatch.setattr(intercept_exec, "resolve_sans_intercept", lambda _name: compiler)
    monkeypatch.setattr(intercept_exec.subprocess, "call", lambda args: calls.append(args) or 0)

    result = intercept_exec.intercept_exec("cc", Path("clang"), ["-E", "-x", "c", "/dev/null"])

    assert result == 0
    assert calls == [
        [
            compiler.as_posix(),
            "-B" + Path(intercept_exec.__file__).parent.as_posix(),
            "-E",
            "-x",
            "c",
            "/dev/null",
        ]
    ]
    assert not build_commands_dir.exists()


def test_convert_json_entries_discards_preprocess_dev_null_probe():
    probe: intercept_exec.InterceptedCommandInfo = {
        "type": "cc",
        "directory": "/build",
        "arguments": ["clang", "-E", "-x", "c", "/dev/null"],
        "file": None,
        "output": None,
    }

    assert targets_from_intercept.convert_json_entries([probe]) == []


def test_preprocess_dev_null_probe_requires_all_probe_arguments():
    assert not intercept_exec.is_preprocess_dev_null_probe("cc", ["clang", "-x", "c", "/dev/null"])
    assert not intercept_exec.is_preprocess_dev_null_probe(
        "cc", ["clang", "-E", "-x", "c", "source.c"]
    )
    assert not intercept_exec.is_preprocess_dev_null_probe(
        "cc", ["clang", "-E", "-x", "c++", "/dev/null"]
    )
    assert not intercept_exec.is_preprocess_dev_null_probe(
        "ld", ["ld", "-E", "-x", "c", "/dev/null"]
    )
