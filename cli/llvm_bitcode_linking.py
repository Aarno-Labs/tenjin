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


def compile_and_link_bitcode(
    compile_commands: CompileCommands,
    destination_path: Path,
    use_llvm14: bool = False,
) -> None:
    """Compile translation units to LLVM bitcode and link them.

    Takes a compilation database and produces a fully linked LLVM Bitcode module
    by using clang on each translation unit, then llvm-link to combine them.

    Args:
        compile_commands: Parsed compilation database
        destination_path: Path where the linked bitcode module should be written

    Raises:
        CalledProcessError: If clang or llvm-link fails
        FileNotFoundError: If required tools are not found
    """

    # Create a temporary directory for intermediate bitcode files
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_dir_path = Path(temp_dir)
        bitcode_files: list[Path] = []

        # Compile each translation unit to bitcode
        sorted_commands = sorted(compile_commands.commands, key=lambda c: c.absolute_file_path)
        for i, cmd in enumerate(sorted_commands):
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
            clang_args = ["clang"]

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

                # Skip optimization flags; we'll specify -O0 ourselves.
                if arg.startswith("-O"):
                    continue

                clang_args.append(arg)

            # Add flags to emit LLVM bitcode that is easy to analyze; we're not
            # going to run this code, or even use it for subsequent translation.
            clang_args.extend(["-emit-llvm", "-g", "-O0", "-c", "-o", str(bc_file)])

            # For reasons I don't yet understand, while Clang 18 picks up our
            # default config file automatically, Clang 14 needs it spelled out.
            clang_args.extend(["--config", "clang.cfg"])

            # Run clang to produce bitcode
            try:
                hermetic.run(
                    clang_args,
                    cwd=cmd.directory_path,
                    check=True,
                    capture_output=True,
                    env_ext={"XJ_USE_LLVM14": "1"} if use_llvm14 else None,
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
            click.echo(compile_commands.to_dict(), err=True)
            raise ValueError("No bitcode files were produced from compilation database")

        if len(bitcode_files) == 1:
            # If there's only one bitcode file, just move it to the target path
            bitcode_files[0].replace(destination_path)
        else:
            llvm_link_args = [
                "llvm-link",
                *[str(bc) for bc in bitcode_files],
                "-o",
                str(destination_path),
            ]

            try:
                hermetic.run(
                    llvm_link_args,
                    check=True,
                    capture_output=True,
                    env_ext={"XJ_USE_LLVM14": "1"} if use_llvm14 else None,
                )
            except CalledProcessError as e:
                click.echo("Failed to link bitcode files")
                click.echo(f"Command: {' '.join(llvm_link_args)}")
                click.echo(f"Stderr: {e.stderr.decode('utf-8', errors='replace')}")
                raise

        # Intermediate bitcode files are automatically cleaned up when
        # the temporary directory context exits
