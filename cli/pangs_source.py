"""Prepare C source for moving global variables into a context passed between functions.

Check PANGS's rewrite plan, apply its signature, callback-adapter, call-site,
and pruning edits, and validate the rewritten C before Rust translation.
"""

from __future__ import annotations

import re
import tempfile
from subprocess import CalledProcessError
from pathlib import Path

import batching_rewriter
import compilation_database
import hermetic
import repo_root


class ContractViolation(ValueError):
    """A selected PANGS rewrite plan could not be applied to produce valid code."""


def relocate_compdb(
    compdb: compilation_database.CompileCommands, original: Path, staged: Path
) -> compilation_database.CompileCommands:
    """Copy compiler commands with paths redirected to the staged source tree."""

    def remap(arg: str) -> str:
        return re.sub(
            rf"(^|[=,:]|^-[^/=,:]*){re.escape(str(original))}(?=/|$)",
            lambda m: m.group(1) + str(staged),
            arg,
        )

    return compilation_database.CompileCommands([
        compilation_database.CompileCommand(
            directory=remap(c.directory),
            file=remap(c.file),
            arguments=[remap(a) for a in c.get_command_parts()],
        )
        for c in compdb.commands
    ])


def snapshot_path(root: Path, relative: str) -> Path:
    """Return a snapshot-relative path, rejecting absolute paths and escapes from root."""
    path = root / relative
    if Path(relative).is_absolute() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"PANGS source path escapes snapshot: {relative}")
    return path


def validate_plan(manifest: dict, root: Path) -> dict:
    """Check finalized plan metadata and the structure of its source edits.

    Return the validated source metadata from the manifest.
    """
    source = manifest.get("context_rewrite", {}).get("source", {})
    if source.get("version") != 3 or source.get("complete") is not True:
        raise ValueError("Tenjin requires PANGS source-complete plan version 3; rerun analysis")
    if source.get("retention") != {
        "policy": "c2rust-declaration-dependencies",
        "version": 1,
        "preserve_unused_functions": False,
    }:
        raise ValueError("PANGS source retention differs from Tenjin's C2Rust configuration")
    if source.get("emitter") != "tenjin-c2rust-default-v1":
        raise ValueError("PANGS source emitter differs from Tenjin's C2Rust configuration")
    if source.get("construction") != "automatic-context-in-main":
        raise ValueError("Unsupported PANGS context construction recipe")
    files = set()
    for entry in source["files"]:
        path = snapshot_path(root, entry["path"])
        if path in files:
            raise ValueError(f"Duplicate PANGS source identity: {path}")
        files.add(path)
    selected = manifest["context_rewrite"]["selected"]
    chosen = {
        g["key"]
        for g in manifest["globals"]
        if (g.get("disposition") or {}).get("chosen") == "localize"
    }
    if {f["global"] for f in selected["fields"]} != chosen:
        raise ValueError("PANGS selected fields differ from final dispositions")
    candidates = {f["global"]: f for f in manifest["context_rewrite"]["fields"]}
    for field in selected["fields"]:
        if field != candidates.get(field["global"]) or field["blockers"]:
            raise ValueError("PANGS selected recipe is blocked or differs from analysis")
    functions = {name for f in selected["fields"] for name in f["functions"]}
    if functions != set(selected["functions"]):
        raise ValueError("PANGS selected functions differ from recipe union")
    previous = None
    for edit in sorted(selected["source_edits"], key=lambda e: (e["file"], e["start"], e["end"])):
        path = snapshot_path(root, edit["file"])
        if path not in files or edit["kind"] not in {
            "signature",
            "call",
            "typedef-clone",
            "typedef-use",
            "prune-declaration",
            "prune-expression",
            "wrapper-declaration",
            "wrapper-definition",
            "wrapper-use",
        }:
            raise ValueError("Unsupported PANGS source edit")
        start, end = edit["start"], edit["end"]
        if start < 0 or end < start:
            raise ValueError(f"Invalid PANGS source edit range: {path}:{start}:{end}")
        if (
            previous
            and previous["file"] == edit["file"]
            and (start < previous["end"] or start == previous["start"])
        ):
            raise ValueError(f"Conflicting PANGS source edits: {path}:{start}")
        previous = edit
    return source


def apply_source_edits(manifest: dict, root: Path) -> None:
    """Apply planned context passing, callback adapters, and discarded-code removal."""
    selected = manifest["context_rewrite"]["selected"]
    with batching_rewriter.BatchingRewriter() as rewriter:
        for entry in manifest["context_rewrite"]["source"]["files"]:
            rewriter.add_rewrite(
                str(snapshot_path(root, entry["path"])), 0, 0, "struct XjGlobals;\n"
            )
        for edit in selected["source_edits"]:
            path = snapshot_path(root, edit["file"])
            rewriter.add_rewrite(
                str(path), edit["start"], edit["end"] - edit["start"], edit["replacement"]
            )


def validation_commands(
    compdb: compilation_database.CompileCommands,
) -> compilation_database.CompileCommands:
    """Build syntax-check commands for rewritten, already-preprocessed C source."""
    commands = []
    for command in compdb.commands:
        args = command.get_command_parts()
        filtered = []
        skip = False
        for arg in args[1:]:
            if skip:
                skip = False
            elif arg in {"-o", "--output", "-x", "-D", "-U", "-include", "-imacros"}:
                skip = True
            elif arg not in {"-c", "-emit-llvm"} and not arg.startswith(("-o", "-D", "-U")):
                filtered.append(arg)
        # Materialization introduces generated #includes. Preprocess these, but
        # do not reapply macros/forced includes to already-preprocessed user code.
        commands.append(
            compilation_database.CompileCommand(
                directory=command.directory,
                file=command.file,
                arguments=[
                    args[0],
                    "-x",
                    "c",
                    *filtered,
                    "-fsyntax-only",
                    "-Werror=incompatible-function-pointer-types",
                ],
            )
        )
    return compilation_database.CompileCommands(commands)


def validate_c(compdb: compilation_database.CompileCommands) -> None:
    """Check every rewritten C file's syntax and types with Clang 14 and 21."""
    for llvm14 in (True, False):
        for command in validation_commands(compdb).commands:
            try:
                hermetic.run(
                    ["clang", *command.get_command_parts()[1:]],
                    cwd=command.directory_path,
                    check=True,
                    capture_output=True,
                    env_ext={"XJ_USE_LLVM14": "1"} if llvm14 else None,
                )
            except CalledProcessError as exc:
                diagnostics = (exc.stderr or b"").decode("utf-8", errors="replace")
                raise ValueError(
                    f"Clang {14 if llvm14 else 21} rejected rewritten C:\n{diagnostics}"
                ) from exc


def validate_cross_tu(manifest: dict, original: Path, staged: Path) -> None:
    """Check declarations agree across C files and moved globals are no longer referenced."""
    commands = compilation_database.CompileCommands.from_dict([
        entry["command"] for entry in manifest["context_rewrite"]["source"]["files"]
    ])
    commands = validation_commands(relocate_compdb(commands, original, staged))
    with tempfile.TemporaryDirectory(prefix="pangs-validation-") as temp:
        database = Path(temp) / "commands.json"
        commands.to_json_file(database)
        hermetic.run(
            [
                hermetic.xj_pangs_exe(repo_root.localdir()),
                "validate-source",
                "--source-compdb",
                database,
                *[
                    arg
                    for field in manifest["context_rewrite"]["selected"]["fields"]
                    for arg in ("--removed-global", field["llvm_name"].rsplit(".", 1)[-1])
                ],
            ],
            check=True,
            capture_output=True,
            env_ext={"XJ_USE_LLVM14": "1"},
        )
