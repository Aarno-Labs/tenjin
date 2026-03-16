from dataclasses import dataclass
from enum import Enum
from pathlib import Path
import pprint

import bencodepy  # type: ignore
import click

import compilation_database
import hermetic
import repo_root
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


def compute_target_type(link_cmd: targets_from_intercept.InterceptedCommand) -> TargetType:
    if any("-shared" in arg for arg in link_cmd.entry["arguments"]):
        return TargetType.SHARED
    if (link_cmd.output or "").endswith(".a"):
        return TargetType.STATIC
    if (link_cmd.output or "").endswith(".o"):
        return TargetType.OBJECT
    return TargetType.EXECUTABLE


class LinkCommandHandling(Enum):
    INCLUDE = "include"
    EXCLUDE = "exclude"
    ADAPT_FOR_C2RUST = "adapt-for-c2rust"


@dataclass
class ExtraCompileOrLinkFlags:
    cc: list[str]
    ld: list[str]


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
        self._use_preprocessed_files: bool = False

    def __repr__(self) -> str:
        return (
            f"BuildInfo(implicit_target={self._implicit_target}, "
            f"intercepted_commands={pprint.pformat(self._intercepted_commands)})"
        )

    def for_single_file(self, c_file: Path, builddir: Path, target: BuildTarget) -> None:
        self._implicit_target = target
        self._with_parsed_compile_commands(
            compilation_database.synthetic_compile_commands_for_c_file(c_file, builddir),
            builddir,
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

        def cmd_invokes_ld(c: targets_from_intercept.InterceptedCommand) -> bool:
            return not c.compile_only and c.entry["arguments"][0].endswith("ld")

        cc_and_ld_dups: set[str] = set()

        all_target_names = [
            cmd_target_name(c) for c in self._intercepted_commands if not c.compile_only
        ]

        # When we intercept a link command done via the compiler driver, we'll also get
        # a corresponding link command done directly by the linker. This isn't a "real"
        # duplicate; we should ignore the compiler driver link command in this case, since
        # all it did was construct the appropriate linker command (which we have).
        for name in all_target_names:
            cmds_for_name = [
                c
                for c in self._intercepted_commands
                if not c.compile_only and cmd_target_name(c) == name
            ]
            if (
                len(cmds_for_name) == 2
                and len([c for c in cmds_for_name if cmd_invokes_ld(c)]) == 1
            ):
                cc_and_ld_dups.add(name)

        # Filter out redundant link steps done via compiler driver invocations.
        sans_redundant_link_commands: list[targets_from_intercept.InterceptedCommand] = []
        for c in self._intercepted_commands:
            name = cmd_target_name(c)
            if name in cc_and_ld_dups and cmd_invokes_ld(c):
                continue
            sans_redundant_link_commands.append(c)

        target_names_sans_redundants = [cmd_target_name(c) for c in sans_redundant_link_commands]

        if len(target_names_sans_redundants) != len(set(target_names_sans_redundants)):
            duplicates = set([
                name
                for name in target_names_sans_redundants
                if target_names_sans_redundants.count(name) > 1
            ])

            for c in sans_redundant_link_commands:
                if not c.compile_only and cmd_target_name(c) in duplicates:
                    click.echo(
                        click.style("ERROR:", fg="red")
                        + f" Duplicate target name '{click.style(cmd_target_name(c), fg='red')}' for command: "
                        + click.style(str(c), fg=(142, 142, 142))
                    )
            raise ValueError(f"Duplicate target names detected: {duplicates}")

        intermediates: dict[str, targets_from_intercept.InterceptedCommand] = {}
        for c in self._intercepted_commands:
            if c.compile_only:
                if (
                    c.output is None
                    and "-o" not in c.entry["arguments"]
                    and len(c.c_inputs) == 1
                    and not Path(c.c_inputs[0]).is_absolute()  # at least for now
                ):
                    # When no output is explicitly provided, the compiler will generate an object file
                    # with the same stem as the input file.
                    c.output = Path(c.c_inputs[0]).with_suffix(".o").as_posix()

                assert c.output is not None, f"Compile command missing output: {c}"
                intermediates[c.output] = c

        link_commands = list(filter(lambda c: not c.compile_only, sans_redundant_link_commands))

        targets: dict[str, BuildTarget] = {}
        for link_cmd in link_commands:
            target_name = cmd_target_name(link_cmd)
            target_type = compute_target_type(link_cmd)
            target_stem = Path(target_name).stem
            assert target_name != "unknown", f"Link command missing target name: {link_cmd}"
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
        self, current_codebase: Path, link_cmd_handling=LinkCommandHandling.EXCLUDE
    ) -> compilation_database.CompileCommands:
        """Return a compilation database for all targets combined."""
        return self._compdb_for_commands_within(
            self._intercepted_commands,
            current_codebase,
            link_cmd_handling=link_cmd_handling,
            extra_compile_or_link_flags=None,
        )

    def get_all_targets(self) -> list[BuildTarget]:
        """Return all build targets found in this BuildInfo."""
        target_map = self._process_targets()
        return [tup[0] for tup in target_map.values()]

    def compdb_for_target_within(
        self,
        target_name: BuildTargetName,
        current_codebase: Path,
        link_cmd_handling=LinkCommandHandling.EXCLUDE,
    ) -> compilation_database.CompileCommands:
        """Return a compilation database for the given target."""
        target_map = self._process_targets()
        if target_name not in target_map:
            raise ValueError(f"Target {target_name} not found in BuildInfo")

        _, cmds = target_map[target_name]
        return self._compdb_for_commands_within(
            cmds,
            current_codebase,
            link_cmd_handling=link_cmd_handling,
            extra_compile_or_link_flags=None,
        )

    def _compdb_for_commands_within(
        self,
        commands: list[targets_from_intercept.InterceptedCommand],
        current_codebase: Path,
        link_cmd_handling: LinkCommandHandling,
        extra_compile_or_link_flags: ExtraCompileOrLinkFlags | None,
    ) -> compilation_database.CompileCommands:
        cc_cmds = [
            _CompileCommand_from_intercepted_command(
                c,
                current_codebase,
                self._use_preprocessed_files,
                link_cmd_handling,
                extra_compile_or_link_flags,
            )
            for c in commands
            if c.compile_only
            or link_cmd_handling != LinkCommandHandling.EXCLUDE
            or len(c.c_inputs) == 1
        ]

        return compilation_database.CompileCommands(cc_cmds)

    def compdb_for_profiled_build(
        self, current_codebase: Path
    ) -> compilation_database.CompileCommands:
        clang_lib_path = hermetic.xj_llvm_root(repo_root.localdir()) / "lib" / "clang"
        libclang_rt_profile_a = list(clang_lib_path.glob("**/libclang_rt.profile.a"))
        assert len(libclang_rt_profile_a) == 1, (
            f"Expected exactly one libclang_rt.profile.a, found: {libclang_rt_profile_a}"
        )
        return self._compdb_for_commands_within(
            self._intercepted_commands,
            current_codebase,
            link_cmd_handling=LinkCommandHandling.INCLUDE,
            extra_compile_or_link_flags=ExtraCompileOrLinkFlags(
                cc=["-fprofile-instr-generate", "-fcoverage-mapping"],
                ld=[
                    "-u__llvm_profile_runtime",
                    libclang_rt_profile_a[0].as_posix(),
                ],
            ),
        )

    def is_empty(self) -> bool:
        """Return True if there are no intercepted commands."""
        return len(self._intercepted_commands) == 0


def _CompileCommand_from_intercepted_command(
    icmd: targets_from_intercept.InterceptedCommand,
    current_codebase: Path,
    use_preprocessed_files: bool,
    link_cmd_handling: LinkCommandHandling,
    extra_compile_or_link_flags: ExtraCompileOrLinkFlags | None = None,
) -> compilation_database.CompileCommand:
    """Convert an InterceptedCommand to a CompileCommand."""

    assert current_codebase.is_absolute()

    cc_res = current_codebase.resolve()

    def tweak_suffix(p: Path) -> str:
        if use_preprocessed_files:
            if p.suffix == ".c":
                return p.with_suffix(".nolines.i").as_posix()
        return p.as_posix()

    def update(p: str, must_exist=True) -> str:
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
            return tweak_suffix(pp)

        # Paths within siblings of the current_codebase are redirected
        # to current_codebase when possible.
        if pp.is_relative_to(cc_res.parent):
            index = len(cc_res.parent.parts)
            redirected_parts = list(pp.parts)
            redirected_parts[index] = cc_res.name
            newp = Path(*redirected_parts)
            if not must_exist or newp.exists():
                return tweak_suffix(newp)
            return tweak_suffix(pp)

        # Absolute paths outside both codebase and builddir remain as-is.
        return p

    def update_arg(p: str) -> str:
        # Applies update to an include (-Ipath) argument
        if p.startswith("-I"):
            return f"-I{update(p[2:])}"
        return update(p)

    filename = icmd.entry["file"]
    if not filename:
        if len(icmd.c_inputs) == 1:
            filename = icmd.c_inputs[0]
        elif len(icmd.c_inputs) == 0 and not icmd.compile_only and len(icmd.rest_inputs) == 1:
            filename = icmd.rest_inputs[0]

    raw_arguments = list(icmd.entry["arguments"])  # make a mutable copy
    if extra_compile_or_link_flags:
        if not icmd.compile_only and raw_arguments[0].endswith("ld"):
            # We only use ld-specific flags when linking is being done directly by ld;
            # we use cc flags for both compile and link steps.
            raw_arguments += extra_compile_or_link_flags.ld
        elif icmd.static_lib:
            pass
        else:
            raw_arguments += extra_compile_or_link_flags.cc

    if link_cmd_handling == LinkCommandHandling.ADAPT_FOR_C2RUST and not icmd.compile_only:
        # For link commands, we need to adapt the arguments
        # to be suitable for c2rust.
        assert icmd.output is not None, "Link command must have an output"
        assert not icmd.c_inputs, f"Link command should not have c_inputs: {icmd}"

        # XREF:c2rust_target_link_type
        if icmd.shared_lib:
            link_type = "shared"
        elif icmd.static_lib:
            link_type = "static"
        else:
            link_type = "exe"
        link_info = {
            "inputs": icmd.rest_inputs,  # FIXME: wrong order???
            "c_files": [],
            "libs": icmd.libs,
            "lib_dirs": icmd.lib_dirs,
            "type": link_type,
            # TODO: parse and add in other linker flags
            # for now, we don't do this because rustc doesn't use them
        }
        filename = "/c2rust/link/" + bencodepy.encode(link_info).decode("utf-8")
        print("@@@@@@@@@@@@@@@@@@@@ targets.py Link info for", icmd.output, ":", link_info)

    if not filename and not icmd.compile_only and not icmd.c_inputs and len(icmd.rest_inputs) > 0:
        # At this point, if we don't have a filename, it's most likely
        # because we have a link command for multiple object files, in
        # which case there is no distinguished input file. But it also
        # doesn't matter so long as we're just using the command to do
        # linking, since the filename doesn't get used. We'll just pick
        # the first input file.
        filename = icmd.rest_inputs[0]

    assert filename, f"InterceptedCommand has no identified input file, {icmd}"

    return compilation_database.CompileCommand(
        directory=icmd.entry["directory"],
        file=update(filename),
        arguments=[update_arg(arg) for arg in raw_arguments],
        output=icmd.output,
    )
