from dataclasses import dataclass
from enum import Enum
from pathlib import Path
import pprint

import compilation_database
import targets_from_intercept
from intercept_exec import InterceptedCommandInfo

"""
Tenjin must be able to handle codebases that build multiple targets.
In particular, we must be able to handle codebases that build shared
libraries plus an executable that links against those shared libraries.

If we only use `compile_commands.json`, we lose the information about
which object files go into which final targets, which means we cannot
safely perform whole-program analyses. If we try to link every object
file into a single target, we may end up with duplicate symbol errors.

Furthermore, we wish to have the generated Cargo project match the
structure of the C build as closely as possible.

Thus, Tenjin needs to represent build targets in a structured way.

CMake provides a "file API" that can be used to query targets
and their source files, but (1) this is CMake-specific, and (2)
it is not quite granular enough for our purposes. For example,
a given source file might be compiled with different flags
for different targets (e.g., different preprocessor definitions),
but the CMake file API would only list the dependency on the source
file.
"""


class TargetType(Enum):
    SHARED = "shared-library"
    EXECUTABLE = "executable"
    STATIC = "static-library"
    OBJECT = "object-file"

    def extension(self) -> str:
        match self:
            case TargetType.SHARED:
                return ".so"
            case TargetType.EXECUTABLE:
                return ""
            case TargetType.STATIC:
                return ".a"
            case TargetType.OBJECT:
                return ".o"
            case _:
                raise ValueError(f"Unknown TargetType: {self}")


type BuildTargetName = str


@dataclass
class BuildTarget:
    name: BuildTargetName
    type: TargetType
    stem: str


class BuildInfo:
    def __init__(self) -> None:
        self._intercepted_commands: list[targets_from_intercept.InterceptedCommand] = []
        self._implicit_target: BuildTarget | None = None

    def __repr__(self) -> str:
        return (
            f"BuildInfo(implicit_target={self._implicit_target}, "
            f"intercepted_commands={pprint.pformat(self._intercepted_commands)})"
        )

    def for_single_file(self, c_file: Path, target: BuildTarget) -> None:
        self._implicit_target = target
        self._with_parsed_compile_commands(
            compilation_database.synthetic_compile_commands_for_c_file(c_file, c_file.parent),
            c_file.parent,
        )

    def _with_parsed_compile_commands(
        self,
        ccmds: compilation_database.CompileCommands,
        directory: Path,
    ) -> None:
        def to_intercepted(
            cmd: compilation_database.CompileCommand,
        ) -> targets_from_intercept.InterceptedCommand:
            assert cmd.directory == directory.as_posix()
            entry: InterceptedCommandInfo = {
                "type": "cc",
                "directory": cmd.directory,
                "arguments": cmd.get_command_parts(),
                "file": cmd.file,
                "output": cmd.output,
            }
            return targets_from_intercept.convert_intercepted_entry(entry)

        cmd_infos = [to_intercepted(c) for c in ccmds.commands]
        self.set_intercepted_commands(cmd_infos)

    def set_intercepted_commands(
        self, intercepted_commands: list[targets_from_intercept.InterceptedCommand]
    ):
        self._intercepted_commands = intercepted_commands

    def _process_targets(
        self,
    ) -> dict[BuildTargetName, tuple[BuildTarget, list[targets_from_intercept.InterceptedCommand]]]:
        """Returns a mapping from target names to (BuildTarget, list of commands).

        The list of commands includes both compilation and linking commands."""
        if self._implicit_target is not None:
            # When an implicit target is given, we assume all commands
            # belong to that target.
            return {self._implicit_target.name: (self._implicit_target, self._intercepted_commands)}

        # Without an implicit target, we must try to reconstruct
        # the targets from the intercepted commands.

        # Sanity check: ensure no duplicate object file outputs
        c_outputs_list = [
            c.output for c in self._intercepted_commands if c.compile_only and c.output is not None
        ]
        assert len(c_outputs_list) == len(set(c_outputs_list)), (
            "Duplicate object file outputs detected"
        )

        # Sanity check: ensure no duplicate target names
        def cmd_target_name(link_cmd: targets_from_intercept.InterceptedCommand) -> str:
            return Path(link_cmd.output).name if link_cmd.output else "unknown"

        target_names = [
            cmd_target_name(c) for c in self._intercepted_commands if not c.compile_only
        ]
        assert len(target_names) == len(set(target_names)), "Duplicate target names detected"

        intermediates: dict[str, targets_from_intercept.InterceptedCommand] = {}
        for c in self._intercepted_commands:
            if c.compile_only:
                assert c.output is not None
                intermediates[c.output] = c

        link_commands = list(filter(lambda c: not c.compile_only, self._intercepted_commands))

        targets: dict[str, BuildTarget] = {}
        for link_cmd in link_commands:
            target_name = cmd_target_name(link_cmd)
            target_type = (
                TargetType.SHARED
                if any("-shared" in arg for arg in link_cmd.entry["arguments"])
                else TargetType.EXECUTABLE
            )
            target_stem = Path(target_name).stem
            target = BuildTarget(name=target_name, type=target_type, stem=target_stem)
            targets[target_name] = target

        target_to_cmds = {}
        for link_cmd in link_commands:
            target = targets[cmd_target_name(link_cmd)]
            target_cmds = [link_cmd]
            for intermediate in link_cmd.rest_inputs:
                if intermediate in intermediates:
                    c_cmd = intermediates[intermediate]
                    target_cmds.append(c_cmd)
            target_to_cmds[target.name] = (target, target_cmds)

        return target_to_cmds

    def compdb_for_all_targets_within(
        self, current_codebase: Path, include_link_cmds=False
    ) -> compilation_database.CompileCommands:
        """Return a compilation database for all targets combined."""
        cc_cmds = [
            _CompileCommand_from_intercepted_command(c, current_codebase)
            for c in self._intercepted_commands
            if c.compile_only or include_link_cmds
        ]

        return compilation_database.CompileCommands(cc_cmds)

    def get_all_targets(self) -> list[BuildTarget]:
        """Return all build targets found in this BuildInfo."""
        target_map = self._process_targets()
        return [tup[0] for tup in target_map.values()]

    def compdb_for_target_within(
        self, target_name: BuildTargetName, current_codebase: Path, include_link_cmds=False
    ) -> compilation_database.CompileCommands:
        """Return a compilation database for the given target."""
        target_map = self._process_targets()
        if target_name not in target_map:
            raise ValueError(f"Target {target_name} not found in BuildInfo")

        _, cmds = target_map[target_name]
        cc_cmds = [
            _CompileCommand_from_intercepted_command(c, current_codebase)
            for c in cmds
            if c.compile_only or include_link_cmds
        ]

        return compilation_database.CompileCommands(cc_cmds)

    def is_empty(self) -> bool:
        """Return True if there are no intercepted commands."""
        return len(self._intercepted_commands) == 0


def _CompileCommand_from_intercepted_command(
    icmd: targets_from_intercept.InterceptedCommand,
    current_codebase: Path,
) -> compilation_database.CompileCommand:
    """Convert an InterceptedCommand to a CompileCommand."""

    assert current_codebase.is_absolute()

    cc_res = current_codebase.resolve()

    def update(p: str) -> str:
        # `p` is assumed to either be a relative path (relative to builddir)
        # or an absolute path, or a non-path argument.
        #
        # We want to convert all paths to be absolute, pointing within
        # current_codebase when possible.

        if Path(p).is_absolute():
            pp = Path(p).resolve()
            # Note: we do not return early for absolute paths
            # that do not exist; we first try rewriting them.
        else:
            try:
                pp = icmd.abs_path(Path(p)).resolve()
                if not pp.exists():
                    return p  # Non-path argument
            except OSError:
                return p  # Non-path argument

        # pp is absolute and resolved here

        # Paths within the current codebase remain as-is.
        if pp.is_relative_to(cc_res):
            return pp.as_posix()

        # Paths within siblings of the current_codebase are redirected
        # to current_codebase when possible.
        if pp.is_relative_to(cc_res.parent):
            index = len(cc_res.parent.parts)
            redirected_parts = list(pp.parts)
            redirected_parts[index] = cc_res.name
            newp = Path(*redirected_parts)
            if newp.exists():
                return newp.as_posix()
            return pp.as_posix()

        # Absolute paths outside both codebase and builddir remain as-is.
        return p

    output = icmd.output
    assert output is not None, "InterceptedCommand has no output"
    if not Path(output).is_absolute():
        output = (current_codebase / output).as_posix()

    filename = icmd.entry["file"]
    if not filename and icmd.compile_only and len(icmd.c_inputs) == 1:
        filename = icmd.c_inputs[0]

    assert filename, f"InterceptedCommand has no identified input file, {icmd}"
    assert output, "InterceptedCommand has no identified output"

    return compilation_database.CompileCommand(
        directory=icmd.entry["directory"],
        file=update(filename),
        arguments=[update(arg) for arg in icmd.entry["arguments"]],
        output=update(output),
    )
