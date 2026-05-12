from typing import Generator
import platform

from pycparser import c_lexer  # type: ignore
from clang.cindex import (  # type: ignore
    TypeKind,
    Cursor,
    CursorKind,
    TranslationUnit,
)
import clang.cindex as cindex  # type: ignore

import repo_root
import hermetic


def create_xj_clang_index() -> cindex.Index:
    """Create a clang Index configured to use the hermetic xj-llvm installation."""

    if not cindex.Config.loaded:
        xj_llvm = hermetic.xj_llvm_root(repo_root.localdir())

        if platform.system() == "Darwin":
            libclang_path = xj_llvm / "lib" / "libclang.dylib"
        else:
            # Keep this version in sync with `constants.py`, which must be compatible
            # with the one embedded in `pyproject.toml`.
            libclang_path = xj_llvm / "lib" / "libclang.so.21.1.8"

        cindex.Config.set_library_file(libclang_path)

    return cindex.Index.create()


type AncestorChain = tuple[Cursor, AncestorChain | None]


def yield_matching_cursors(
    root_cursor: Cursor, cursor_kinds_of_interest: list[CursorKind]
) -> Generator[tuple[Cursor, AncestorChain], None, None]:
    """Yield all call expression cursors in the translation unit."""

    worklist: list[AncestorChain] = [(root_cursor, None)]
    while worklist:
        current, ancestors = worklist.pop()
        if current.kind in cursor_kinds_of_interest:
            assert ancestors is not None
            yield (current, ancestors)

        for child in current.get_children():
            worklist.append((child, (current, ancestors)))  # type: ignore


def render_declaration_sans_qualifiers(type_obj, var_name) -> str:
    """Render a variable declaration for the given type and name."""

    def render_inner(ty, inner_text):
        """Recursively build the declaration string."""
        kind = ty.kind

        if kind == TypeKind.POINTER:
            pointee = ty.get_pointee()
            # Need parentheses if inner_text contains array or function syntax
            if pointee.kind in (
                TypeKind.CONSTANTARRAY,
                TypeKind.FUNCTIONPROTO,
                TypeKind.FUNCTIONNOPROTO,
            ):
                return render_inner(pointee, f"(*{inner_text})")
            else:
                return render_inner(pointee, f"*{inner_text}")

        elif kind == TypeKind.CONSTANTARRAY:
            size = ty.get_array_size()
            element_type = ty.get_array_element_type()
            return render_inner(element_type, f"{inner_text}[{size}]")

        elif kind == TypeKind.INCOMPLETEARRAY:
            element_type = ty.get_array_element_type()
            return render_inner(element_type, f"{inner_text}[]")

        elif kind == TypeKind.FUNCTIONPROTO:
            result_type = ty.get_result()
            args_str = ", ".join(arg.spelling for arg in ty.argument_types())
            return render_inner(result_type, f"{inner_text}({args_str})")

        elif kind == TypeKind.FUNCTIONNOPROTO:
            result_type = ty.get_result()
            return render_inner(result_type, f"{inner_text}()")

        elif kind == TypeKind.ELABORATED:
            # Unwrap elaborated types
            named_type = ty.get_named_type()
            return render_inner(named_type, inner_text)

        else:
            # Base case: simple type
            return f"{ty.spelling} {inner_text}"

    return render_inner(type_obj, var_name).strip()


def render_normalized_declaration_with_type_names(
    decl_sans_semicolon: str, type_names: set[str]
) -> str:
    """Render a normalized declaration string for comparison purposes."""
    # Without knowing the typedef names, clang may misinterpret
    # a declaration like `void foo(size_t)` as giving an untyped parameter
    # named `size_t`, which is then given the implicit default type `int`.
    # To avoid this, the caller can pass in an overapproximation of the
    # needed type names.
    prefix_lines = [f"typedef struct {{}} {tn};" for tn in type_names]
    parsed_text = "\n".join(prefix_lines) + "\n" + decl_sans_semicolon + ";"

    tu = TranslationUnit.from_source(
        "example.c",
        args=["-std=c11"],
        unsaved_files=[("example.c", parsed_text)],
    )

    # Get the last declaration in the TU.
    cursor = list(tu.cursor.get_children())[-1]

    # print(cursor.kind, cursor.spelling)
    if cursor.kind == CursorKind.FUNCTION_DECL:
        return render_declaration_sans_qualifiers(cursor.type, cursor.spelling)

    elif cursor.kind == CursorKind.VAR_DECL:
        ty = cursor.type
        if ty.kind == TypeKind.POINTER:
            pointee = ty.get_pointee()
            # Need parentheses if inner_text contains array or function syntax
            if pointee.kind in (
                TypeKind.CONSTANTARRAY,
                TypeKind.FUNCTIONPROTO,
                TypeKind.FUNCTIONNOPROTO,
            ):
                return render_declaration_sans_qualifiers(cursor.type, cursor.spelling)

    else:
        decl_str = render_declaration_sans_qualifiers(
            cursor.underlying_typedef_type, cursor.spelling
        )
        return "typedef " + decl_str
    raise ValueError(f"Unexpected cursor kind: {cursor.kind}")


def lex_preprocessed_c_and_estimate_typenames(sourcetext: str) -> tuple[list[str], set[str]]:
    """Lex preprocessed C source text into tokens using pycparser."""
    dummy_lexer = c_lexer.CLexer(None, None, None, None)
    possible_typenames = set()

    def type_lookup_func(name: str) -> bool:
        if not name:
            return False
        if name.isidentifier():
            if (
                name.upper() not in dummy_lexer.keywords
                and name.upper() not in dummy_lexer.keywords_new
            ):
                possible_typenames.add(name)
                return True
        return False

    lexer = c_lexer.CLexer(
        error_func=lambda msg, line, column: print(f"Lexer error at {line}:{column}: {msg}"),
        on_lbrace_func=lambda: None,
        on_rbrace_func=lambda: None,
        type_lookup_func=type_lookup_func,
    )
    lexer.build()
    lexer.input(sourcetext)

    tokens = []
    while True:
        tok = lexer.token()
        if not tok:
            break
        tokens.append(tok.value)
    return tokens, possible_typenames


def render_normalized_declaration(decl_sans_semicolon: str) -> str:
    """Render a normalized declaration string for comparison purposes."""
    # Step 1: lex all tokens to collect an overapproximation of needed typedef names.
    _, type_names = lex_preprocessed_c_and_estimate_typenames(decl_sans_semicolon)
    # Step 2: use clang to parse the declaration in a suitably extended type context.
    return render_normalized_declaration_with_type_names(decl_sans_semicolon, type_names)


if __name__ == "__main__":
    sourcetexts = [
        (
            "int mtimesort(struct _info **a, struct _info **b)",
            "int mtimesort(struct _info **, struct _info **)",
        ),
        (
            "extern int mtimesort(struct _info **a, struct _info **b)",
            "int mtimesort(struct _info **, struct _info **)",
        ),
        (
            "extern int mtimesort(struct _info **a,   struct   _info * * b)",
            "int mtimesort(struct _info **, struct _info **)",
        ),
        ("char const *prot(mode_t m)", "const char *prot(mode_t)"),
        (
            "extern int (*topsort)(const struct _info **, const struct _info **)",
            "int (*topsort)(const struct _info **, const struct _info **)",
        ),
        ("void *xrealloc (void *ptr, size_t size)", "void *xrealloc(void *, size_t)"),
        ("void *xrealloc (void *   , size_t     )", "void *xrealloc(void *, size_t)"),
        ("void printit(char const *s)", "void printit(const char *)"),
        (
            "typedef int (*compfunc_t)(const void *, const void *)",
            "typedef int (*compfunc_t)(const void *, const void *)",
        ),
    ]
    for sourcetext in sourcetexts:
        result = render_normalized_declaration(sourcetext[0])
        expected = sourcetext[1]
        status = "✓" if result == expected else "✗"
        print(f"{status} {result}")
        if result != expected:
            print(f"  Expected: {expected}")
