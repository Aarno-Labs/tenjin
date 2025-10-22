"""LLVM Bitcode compilation and linking functionality.

This module provides functions to compile C source files to LLVM bitcode
and link them into a single module.
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from subprocess import CalledProcessError

import click

import hermetic
from compilation_database import CompileCommands
from repo_root import localdir


def compile_and_link_bitcode(
    compile_commands_path: Path,
    target_path: Path,
) -> None:
    """Compile translation units to LLVM bitcode and link them.

    Takes a compilation database and produces a fully linked LLVM Bitcode module
    by using clang on each translation unit, then llvm-link to combine them.

    Args:
        compile_commands_path: Path to compile_commands.json
        target_path: Path where the linked bitcode module should be written

    Raises:
        CalledProcessError: If clang or llvm-link fails
        FileNotFoundError: If required tools are not found
    """
    # Load the compilation database
    compile_commands = CompileCommands.from_json_file(compile_commands_path)

    # Get paths to clang and llvm-link from hermetic installation
    llvm_root = hermetic.xj_llvm_root(localdir())
    clang_path = llvm_root / "bin" / "clang"
    llvm_link_path = llvm_root / "bin" / "llvm-link"

    # Create a temporary directory for intermediate bitcode files
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_dir_path = Path(temp_dir)
        bitcode_files: list[Path] = []

        # Compile each translation unit to bitcode
        for i, cmd in enumerate(compile_commands.commands):
            # Get the command parts
            args = cmd.get_command_parts()

            # Skip if this doesn't look like a compilation command
            if not args or len(args) == 0:
                continue

            # Create a unique output filename for this translation unit
            source_file = cmd.absolute_file_path
            bc_file = temp_dir_path / f"unit_{i}_{source_file.stem}.bc"

            # Build the clang command to emit LLVM bitcode
            # Start with clang and add the original compilation flags
            clang_args = [str(clang_path)]

            # Add all flags from the original command except the compiler name
            # and any output specification
            skip_next = False
            for j, arg in enumerate(args[1:], 1):
                if skip_next:
                    skip_next = False
                    continue

                # Skip output-related flags as we'll set our own
                if arg in ("-o", "--output"):
                    skip_next = True
                    continue
                if arg.startswith("-o") and len(arg) > 2:
                    continue

                clang_args.append(arg)

            # Add flags to emit LLVM bitcode
            clang_args.extend(["-emit-llvm", "-c", "-o", str(bc_file)])

            # Run clang to produce bitcode
            try:
                hermetic.run(
                    clang_args,
                    cwd=cmd.directory_path,
                    check=True,
                    capture_output=True,
                )
                bitcode_files.append(bc_file)
            except CalledProcessError as e:
                # If compilation fails, print diagnostic info and re-raise
                click.echo(f"Failed to compile {source_file} to bitcode")
                click.echo(f"Command: {' '.join(clang_args)}")
                click.echo(f"Stderr: {e.stderr.decode('utf-8', errors='replace')}")
                raise

        # Link all bitcode files into a single module
        if not bitcode_files:
            raise ValueError("No bitcode files were produced from compilation database")

        llvm_link_args = [
            str(llvm_link_path),
            *[str(bc) for bc in bitcode_files],
            "-o",
            str(target_path),
        ]

        try:
            hermetic.run(
                llvm_link_args,
                check=True,
                capture_output=True,
            )
        except CalledProcessError as e:
            click.echo("Failed to link bitcode files")
            click.echo(f"Command: {' '.join(llvm_link_args)}")
            click.echo(f"Stderr: {e.stderr.decode('utf-8', errors='replace')}")
            raise

        # Intermediate bitcode files are automatically cleaned up when
        # the temporary directory context exits
