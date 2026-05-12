from clang.cindex import (  # type: ignore
    CursorKind,
    TranslationUnit,
)

import c_refact
import cindex_helpers
import compilation_database


def translation_unit_has_main(tu: TranslationUnit) -> bool:
    """Check if a translation unit defines a function named `main`."""
    for cursor in tu.cursor.get_children():
        if cursor.kind == CursorKind.FUNCTION_DECL and cursor.spelling == "main":
            # Check that it's a definition, not just a declaration
            if cursor.is_definition():
                return True
    return False


def find_main_translation_units(
    compdb: compilation_database.CompileCommands,
) -> list[compilation_database.CompileCommand]:
    index = cindex_helpers.create_xj_clang_index()
    result: list[compilation_database.CompileCommand] = []

    for cmd in compdb.commands:
        if cmd.is_fake_link_thingy:
            continue
        srcfile = cmd.absolute_file_path
        parts = cmd.get_command_parts()[1:]  # Skip compiler executable

        try:
            tu = c_refact.parse_translation_unit_with_args(
                index,
                srcfile.as_posix(),
                parts,
                in_dir=cmd.directory_path.as_posix(),
            )

            if translation_unit_has_main(tu):
                result.append(cmd)

        except Exception as e:
            # Skip files that fail to parse
            print(f"Warning: Failed to parse {srcfile}: {e}")
            continue

    return result
