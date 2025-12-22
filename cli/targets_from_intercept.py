from dataclasses import dataclass, replace
from typing import Callable
from pathlib import Path
import os.path

import bencodepy  # type: ignore

import intercept_exec


# Path.resolve() resolves symlinks, but we want to preserve them
def combine(path1: str, path2: str) -> str:
    if os.path.isabs(path2):
        return os.path.normpath(path2)
    return os.path.normpath(os.path.join(path1, path2))


@dataclass
class InterceptedCommand:
    entry: intercept_exec.InterceptedCommandInfo
    new_args: list[str]
    c_inputs: list[str]
    rest_inputs: list[str]
    libs: list[str]
    lib_dirs: list[str]
    compile_only: bool
    shared_lib: bool
    output: str | None = None

    def abs_path(self, p: Path) -> Path:
        if p.is_absolute():
            return p
        return Path(self.entry["directory"]) / p

    def fmap_input_paths(self, updater: Callable[[str], str]) -> "InterceptedCommand":
        return replace(
            self,
            entry={
                **self.entry,
                "file": updater(self.entry["file"]) if self.entry["file"] else None,
                "arguments": [updater(arg) for arg in self.entry["arguments"]],
            },
            c_inputs=[updater(arg) for arg in self.c_inputs],
        )


def convert_json_entries(
    entries: list[intercept_exec.InterceptedCommandInfo],
) -> list[InterceptedCommand]:
    entry_infos = []
    for entry in entries:
        entry_infos.append(convert_intercepted_entry(entry))
    return entry_infos


def convert_intercepted_entry(entry: intercept_exec.InterceptedCommandInfo) -> InterceptedCommand:
    # old_args, entry["arguments"] = entry["arguments"], []
    old_args = list(entry["arguments"])
    arg_iter = iter(old_args)

    ei = InterceptedCommand(
        entry=entry,
        new_args=[],
        c_inputs=[],
        rest_inputs=[],
        libs=[],
        lib_dirs=[],
        compile_only=False,
        shared_lib=False,
        output=None,
    )

    ei.new_args.append(next(arg_iter))  # Copy over old_args[0]
    for arg in arg_iter:
        if arg in {"-D", "-U", "-I", "-include"}:
            # TODO: use the full list of `Separate` options from gcc
            ei.new_args.append(arg)
            ei.new_args.append(next(arg_iter))

        elif arg == "-c":
            ei.compile_only = True

        elif arg == "-o":
            ei.output = next(arg_iter)
            ei.new_args.append(arg)
            assert ei.output is not None
            ei.new_args.append(ei.output)

        elif arg[:2] == "-o":
            ei.output = arg[2:]
            ei.new_args.append(arg)

        elif arg == "-l":
            ei.libs.append(next(arg_iter))

        elif arg[:2] == "-l":
            ei.libs.append(arg[2:])

        # -pthread implicitly adds -lpthread
        elif arg == "-pthread":
            ei.libs.append("pthread")
            ei.new_args.append(arg)

        elif arg == "-L":
            ei.lib_dirs.append(next(arg_iter))

        elif arg[:2] == "-L":
            ei.lib_dirs.append(arg[2:])

        elif arg == "-shared":
            ei.shared_lib = True

        elif arg[-2:] == ".c":
            ei.c_inputs.append(arg)

        elif arg[-2:] == ".o":
            ei.rest_inputs.append(arg)

        elif arg[-3:] == ".so" and not arg.startswith("-Wl,"):
            ei.rest_inputs.append(arg)

        else:
            ei.new_args.append(arg)

    return ei


def extract_MD_format_dependencies(depfile_path: Path) -> list[str]:
    """Extract the list of dependencies from a Makefile-style .d file."""
    with open(depfile_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Remove line continuations
    content = content.replace("\\\n", " ")

    # Split at the first colon
    parts = content.split(":", 1)
    if len(parts) != 2:
        return []

    deps_part = parts[1]
    deps = deps_part.split()
    return deps


# Extracted from c2rust/scripts/convert_build_commands.py
def extract_link_compile_commands(
    entry_infos: list[InterceptedCommand], codebase: Path, builddir: Path
) -> list[dict]:
    fake_ctr = -1

    def get_fake() -> int:
        nonlocal fake_ctr
        fake_ctr += 1
        return fake_ctr

    object_map = {}
    new_cc_entries = []
    new_link_entries = []
    for ei in entry_infos:
        for inp in ei.c_inputs:
            inp_path = Path(inp)
            if not inp_path.is_absolute():
                abs = codebase / inp_path
                if abs.exists():
                    inp_path = abs
                else:
                    d = Path(ei.entry["directory"])
                    abs = codebase / d / inp_path
                    if abs.exists():
                        inp_path = abs

            assert inp_path.exists(), f"Input file {inp_path} does not exist"
            assert inp_path.is_absolute()
            assert inp_path.is_relative_to(codebase)

            # TODO: handle duplicates
            c_object_unresolved = ei.output or "%s___%d.o" % (Path(inp).stem, get_fake())
            c_object = resolve_in_ei_directory(ei, c_object_unresolved, builddir)
            object_map[inp] = c_object

            new_entry: dict[str, str | list[str]] = ei.entry.copy()  # type: ignore
            new_entry["arguments"] = [*ei.new_args, "-c", inp]
            new_entry["file"] = inp_path.as_posix()  # .relative_to(codebase).as_posix()
            new_entry["output"] = c_object
            del new_entry["type"]  # this field is not allowed in compile_commands.json
            new_cc_entries.append(new_entry)

    for ei in filter(lambda ei: not ei.compile_only, entry_infos):
        new_entry: dict[str, str | list[str]] = ei.entry.copy()  # type: ignore
        # mapped_c_objects = list(set([object_map[inp] for inp in ei.c_inputs]))
        # c_files = [inp for inp in ei.c_inputs if inp in object_map]
        # print("c_inputs:", ei.c_inputs)
        # print("mapped_c_objects:", mapped_c_objects)
        # print("c_files:", c_files)
        # print()
        assert not ei.c_inputs

        new_entry["arguments"] = ei.new_args
        # Hacky solution: c2rust-tranpile needs an absolute path here,
        # so we add a path-like prefix so that the transpiler can both
        # parse it correctly and recognize it as a bencoded link command
        inputs = [resolve_in_ei_directory(ei, x, builddir) for x in ei.rest_inputs]
        link_info = {
            "inputs": inputs,  # FIXME: wrong order???
            "c_files": [],
            "libs": ei.libs,
            "lib_dirs": ei.lib_dirs,
            "type": "shared" if ei.shared_lib else "exe",
            # TODO: parse and add in other linker flags
            # for now, we don't do this because rustc doesn't use them
        }
        print("Link info:", link_info)
        # new_entry["_c2rust_link"] = link_info  # delay bencoding, to allow editing of paths
        new_entry["file"] = "/c2rust/link/" + bencodepy.encode(link_info).decode("utf-8")
        # new_entry["file"] = "/c2rust/link"
        output = ei.output or "a.out"
        # CMake's compile_commands.json output paths are relative to the build directory,
        # even when the "directory" is a subdirectory of the build directory.
        new_entry["output"] = resolve_in_ei_directory(ei, output, builddir)
        del new_entry["type"]  # this field is not allowed in compile_commands.json
        new_link_entries.append(new_entry)

    # With one link command, we won't want to create a workspace.
    if len(new_link_entries) == 1:
        return new_cc_entries

    return sorted(new_cc_entries + new_link_entries, key=lambda e: e["file"])


def resolve_in_ei_directory(ei: InterceptedCommand, pathbuf: str, builddir: Path) -> str:
    if Path(pathbuf).is_absolute():
        return pathbuf

    p = Path(ei.entry["directory"], pathbuf)
    if not p.exists():
        return pathbuf
    p = p.resolve()
    builddir = builddir.resolve()
    assert p.is_relative_to(builddir)
    return p.relative_to(builddir).as_posix()
