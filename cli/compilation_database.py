from __future__ import annotations

import json
from pathlib import Path
import shlex
from dataclasses import dataclass

import repo_root
import hermetic
import ingest

# See https://clang.llvm.org/docs/JSONCompilationDatabase.html


@dataclass
class CompileCommand:
    """Represents a single compile command entry from compile_commands.json"""

    # Required fields
    directory: str
    file: str

    # Either command OR arguments is required (but not both)
    command: str | None = None
    arguments: list[str] | None = None

    # Optional fields
    output: str | None = None

    def __post_init__(self):
        """Validate that either command or arguments is provided"""
        if self.command is None and self.arguments is None:
            raise ValueError("Either 'command' or 'arguments' must be provided")
        if self.command is not None and self.arguments is not None:
            raise ValueError("Cannot specify both 'command' and 'arguments'")

    @property
    def directory_path(self) -> Path:
        """Get directory as a Path object"""
        return Path(self.directory)

    @property
    def file_path(self) -> Path:
        """Get file as a Path object"""
        return Path(self.file)

    @property
    def absolute_file_path(self) -> Path:
        """Get absolute path to the file"""
        if Path(self.file).is_absolute():
            return Path(self.file)
        return self.directory_path / self.file

    def get_command_parts(self) -> list[str]:
        """Get command as a list of arguments, regardless of original format"""
        if self.arguments:
            return self.arguments
        elif self.command:
            # Use shlex.split() for proper shell-like parsing of quoted arguments
            return shlex.split(self.command)
        return []

    def set_command_parts(self, parts: list[str]):
        """Set command as a list of arguments, regardless of original format"""
        if self.arguments:
            self.arguments = parts
        elif self.command:
            self.command = " ".join(parts)


@dataclass
class CompileCommands:
    """Represents the entire compile_commands.json file"""

    commands: list[CompileCommand]

    @classmethod
    def from_json_file(cls, file_path: str | Path) -> CompileCommands:
        """Load compile commands from a JSON file"""
        with open(file_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        return cls.from_dict(data)

    @classmethod
    def from_dict(cls, data: list[dict]) -> CompileCommands:
        """Create from a list of dictionaries"""
        commands = [CompileCommand(**entry) for entry in data]
        return cls(commands=commands)

    def to_dict(self) -> list[dict]:
        """Convert to a list of dictionaries"""
        result = []
        for cmd in self.commands:
            entry: dict[str, str | list[str]] = {"directory": cmd.directory, "file": cmd.file}
            if cmd.command:
                entry["command"] = cmd.command
            if cmd.arguments:
                entry["arguments"] = cmd.arguments
            if cmd.output:
                entry["output"] = cmd.output
            result.append(entry)
        return result

    def to_json_file(self, file_path: str | Path, indent: int = 2):
        """Save compile commands to a JSON file"""
        with open(file_path, "w", encoding="utf-8") as f:
            json.dump(self.to_dict(), f, indent=indent)

    def get_source_files(self) -> list[Path]:
        """Get all unique source files"""
        return list(set(cmd.absolute_file_path for cmd in self.commands))

    def get_directories(self) -> list[Path]:
        """Get all unique directories"""
        return list(set(cmd.directory_path for cmd in self.commands))


def write_synthetic_compile_commands_to(compdb_path: Path, c_file: Path, builddir: Path):
    """Write a synthetic compile_commands.json file for a single C file."""
    assert compdb_path.parent.is_dir()
    outname = c_file.with_suffix(".o").name
    cc = hermetic.xj_llvm_root(repo_root.localdir()) / "bin" / "clang"
    c_file_full_q = shlex.quote(c_file.resolve().as_posix())
    CompileCommands([
        CompileCommand(
            directory=builddir.as_posix(),
            file=c_file.resolve().as_posix(),
            command=f"{cc} -c {c_file_full_q} -o {shlex.quote(outname)}",
            output=outname,
        )
    ]).to_json_file(compdb_path)


def extract_preprocessor_definitions_from_compile_commands(
    compile_commands_path: Path,
    codebase: Path,
) -> ingest.PerFilePreprocessorDefinitions:
    """Extract preprocessor definitions from `compile_commands.json`"""
    definitions = {}
    ccs = CompileCommands.from_json_file(compile_commands_path)
    for cc in ccs.commands:
        args = cc.get_command_parts()
        # command_info["directory"] is build directory, which can be
        # located anywhere; it has no relation to the source file path.
        if cc.file_path.resolve() == codebase.resolve():
            relative_path = cc.file_path
        else:
            relative_path = Path(cc.file).relative_to(codebase)
        defs: list[ingest.PreprocessorDefinition] = []
        i = 0
        while i < len(args):
            arg = args[i]
            i += 1
            if arg == "-D" and i + 1 < len(args):
                # If we find a bare -D, the next argument is a definition.
                key, _, value = args[i + 1].partition("=")
                i += 1  # Skip the value
                defs.append((key, value))
            elif arg.startswith("-D") and "=" in arg:
                # Handle -Dkey=value style definitions.
                key, _, value = arg[2:].partition("=")
                defs.append((key, value))
            elif arg.startswith("-D"):
                # Handle -Dkey style definitions.
                defs.append((arg[2:], None))  # Add the definition without the -D prefix
        if defs:
            definitions[relative_path.as_posix()] = defs
    return definitions


def munge_compile_commands_for_hermetic_translation(compile_commands_path: Path):
    """Modify compile_commands.json to explicitly provide Tenjin's sysroot.
    The clang driver automatically gets this via .cfg files, but c2rust doesn't."""
    ccs = CompileCommands.from_json_file(compile_commands_path)
    for cc in ccs.commands:
        args = cc.get_command_parts()
        if not args:
            continue
        if Path(args[0]).name in ("clang", "cc") and "-c" in args:
            args.append("--sysroot")
            args.append(str(hermetic.xj_llvm_root(repo_root.localdir()) / "sysroot"))

        cc.set_command_parts(args)

    ccs.to_json_file(compile_commands_path)


def munge_compile_commands_for_tenjin_translation(compile_commands_path: Path):
    """Modify compile_commands.json to include Tenjin-specific declarations
    and block expansion of macros that Tenjin gives special treatment."""
    ccs = CompileCommands.from_json_file(compile_commands_path)
    for cc in ccs.commands:
        args = cc.get_command_parts()
        if not args:
            continue
        if Path(args[0]).name in ("clang", "cc") and "-c" in args:
            args.append("-include")
            args.append(
                str(repo_root.find_repo_root_dir_Path() / "cli" / "autoincluded_tenjin_decls.h")
            )
            args.append("--block-macro")
            args.append("assert")
        cc.set_command_parts(args)

    ccs.to_json_file(compile_commands_path)
