"""Tests for clang.cindex helpers."""

from clang.cindex import Cursor, TranslationUnit  # type: ignore


import cindex_helpers


def parse_single_declaration(c_source: str) -> Cursor:
    """Parse a C source string containing a single declaration and return its cursor.

    Args:
        c_source: A string of C source code assumed to contain a single declaration.

    Returns:
        The Cursor for the declaration.
    """
    index = cindex_helpers.create_xj_clang_index()
    tu = TranslationUnit.from_source(
        "decl.c",
        args=["-std=c11"],
        unsaved_files=[("decl.c", c_source)],
        index=index,
    )
    # Get the first (and assumed only) top-level declaration
    children = list(tu.cursor.get_children())
    assert len(children) == 1, f"Expected 1 declaration, got {len(children)}"
    return children[0]


def source_span_for_single_declaration(c_source: str) -> str:
    cursor = parse_single_declaration(c_source)
    return c_source[cursor.extent.start.offset : cursor.extent.end.offset]


def test_parse_single_declaration_extent_length():
    assert source_span_for_single_declaration("int x;") == "int x"


def test_parse_unwrapped_fn_args():
    assert source_span_for_single_declaration("int f();") == "int f()"


def test_parse_macro_wrapping_fn_args():
    source = """
    #define OF(args) args
    int f OF(());
    """
    assert source_span_for_single_declaration(source) == "int f "
    # Note the lack of args, and the trailing space.
