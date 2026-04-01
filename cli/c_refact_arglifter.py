"""C refactoring tool to lift struct-pointer member accesses
from call arguments into local variables.

This module implements a refactoring that finds patterns where:
1. A function call has an argument that is a struct pointer (X)
2. Another argument is a member access on that pointer (X->F)

The refactoring extracts the member access into a local variable:
    TYP newvar = X->F;
inserted before the call, and replaces X->F with newvar in the call.
"""

from dataclasses import dataclass
from pathlib import Path

from clang.cindex import (  # type: ignore
    CursorKind,
    TypeKind,
    Cursor,
)

import batching_rewriter
import compilation_database
import c_refact
from cindex_helpers import (
    render_declaration_sans_qualifiers,
    yield_matching_cursors,
    AncestorChain,
)


@dataclass
class MemberAccessInfo:
    """Information about a member access expression in a call argument."""

    cursor: Cursor
    base_expr: Cursor  # The expression being dereferenced (e.g., ptr in ptr->field)
    field_name: str
    field_type: str  # Rendered type declaration
    start_offset: int
    end_offset: int
    arg_index: int  # Position in the argument list


@dataclass
class CallSiteRewrite:
    """Information needed to rewrite a call site."""

    call_cursor: Cursor
    file_path: str
    statement_start_offset: int  # Where to insert declarations
    member_accesses: list[MemberAccessInfo]  # Member accesses to lift
    base_expr_text: str  # The text of the base expression (e.g., "ptr")


def get_source_text(cursor: Cursor, content: bytes) -> str:
    """Extract the source text for a cursor."""
    start = cursor.extent.start.offset
    end = cursor.extent.end.offset
    return content[start:end].decode("utf-8")


def find_statement_start(cursor: Cursor, content: bytes, ancestors: AncestorChain | None) -> int:
    """Find the start of the statement containing this cursor.

    Walks up the AST to find the enclosing statement, then returns its start offset.
    For statements within compound statements, we want to insert before the statement.
    """
    # Walk up to find a statement that's a direct child of a compound statement
    # print("Finding statement start for cursor at:", cursor.location)

    current = cursor
    while ancestors is not None:
        parent, ancestors = ancestors
        if not parent:
            # print("No parent found, started at", cursor.kind, "stopped at current:", current.kind)
            break

        # Check if parent is a compound statement or function body
        if parent.kind in (CursorKind.COMPOUND_STMT, CursorKind.FUNCTION_DECL):
            # Current is the statement we want to insert before
            # Find the actual start of the line (accounting for indentation)
            start_offset = current.extent.start.offset

            # Walk back to find the start of the line
            while start_offset > 0 and content[start_offset - 1 : start_offset] not in (
                b"\n",
                b"\r",
            ):
                start_offset -= 1

            # print("parent was compound stmt or fn decl, Walked back from:", cursor.extent.start)
            return start_offset

        current = parent

    # Fallback: use the cursor's own start
    return cursor.extent.start.offset


def analyze_call_arguments(call_cursor: Cursor) -> list[tuple[Cursor, int]]:
    """Analyze arguments of a call expression.

    Returns a list of (argument_cursor, arg_index) tuples.
    """
    args: list[tuple[Cursor, int]] = []
    arg_index = 0

    # Children of CALL_EXPR: first is callee, rest are arguments
    children = list(call_cursor.get_children())
    if not children:
        return args

    # Skip the first child (callee)
    for child in children[1:]:
        args.append((child, arg_index))
        arg_index += 1

    return args


def strip_unexposed(cursor: Cursor) -> Cursor:
    """Strip unexposed nodes to get to the underlying expression.
    May return an unexposed cursor if no exposed child is found."""
    while cursor.kind.is_unexposed():
        n = next(cursor.get_children(), None)
        if n is None:
            break
        cursor = n
    return cursor


def get_member_access_expr(cursor: Cursor) -> Cursor | None:
    """Check if cursor is a member access expression (-> or .)."""
    c = strip_unexposed(cursor)
    return c if c and c.kind in (CursorKind.MEMBER_REF_EXPR, CursorKind.MEMBER_REF) else None


def get_member_access_base(cursor: Cursor) -> Cursor | None:
    """Get the base expression of a member access (the part before -> or .)."""
    mae = get_member_access_expr(cursor)
    if not mae:
        return None

    # The first child of MEMBER_REF_EXPR is the base expression
    children = list(mae.get_children())
    if children:
        return children[0]
    return None


def is_pointer_dereference(cursor: Cursor) -> bool:
    """Check if this member access uses -> (pointer dereference)."""
    base = get_member_access_base(cursor)
    if base:
        return base.type.kind == TypeKind.POINTER
    return False


def cursors_match(c1: Cursor, c2: Cursor, content: bytes) -> bool:
    """Check if two cursors refer to the same expression textually."""
    text1 = get_source_text(c1, content)
    text2 = get_source_text(c2, content)
    return text1 == text2


def find_matching_base_pointer(
    member_access: Cursor, all_args: list[tuple[Cursor, int]], content: bytes
) -> tuple[Cursor, int] | None:
    """Find an argument that matches the base of the member access.

    Returns (matching_arg_cursor, arg_index) or None.
    """
    base = get_member_access_base(member_access)
    if not base:
        return None

    base_text = get_source_text(base, content)

    # Look for an argument whose text matches the base expression
    for arg_cursor, arg_idx in all_args:
        # Skip the member access itself
        if arg_cursor == member_access:
            continue

        arg_text = get_source_text(arg_cursor, content)
        if arg_text == base_text:
            # Found a matching argument
            return (arg_cursor, arg_idx)

    return None


def analyze_call_site(
    call_cursor: Cursor, content: bytes, file_path: str, ancestors: AncestorChain
) -> CallSiteRewrite | None:
    """Analyze a call site to find member accesses that can be lifted.

    Returns CallSiteRewrite if the pattern is found, None otherwise.
    """
    args = analyze_call_arguments(call_cursor)
    if len(args) < 2:
        # Need at least 2 arguments for the pattern
        return None

    member_accesses_to_lift: list[MemberAccessInfo] = []
    base_expr_text_set: set[str] = set()

    for arg_cursor, arg_idx in args:
        # Check if this argument is a member access using ->
        if get_member_access_expr(arg_cursor) and is_pointer_dereference(arg_cursor):
            # Check if the base pointer appears as another argument
            match = find_matching_base_pointer(arg_cursor, args, content)
            if match:
                base_cursor, _ = match
                base_text = get_source_text(base_cursor, content)

                # Get field information
                field_name = arg_cursor.spelling
                base = get_member_access_base(arg_cursor)
                assert base is not None

                # Get the field type
                # The type of the member access expression is the field type
                field_type_obj = arg_cursor.type
                field_type_str = render_declaration_sans_qualifiers(field_type_obj, "")

                member_accesses_to_lift.append(
                    MemberAccessInfo(
                        cursor=arg_cursor,
                        base_expr=base,
                        field_name=field_name,
                        field_type=field_type_str,
                        start_offset=arg_cursor.extent.start.offset,
                        end_offset=arg_cursor.extent.end.offset,
                        arg_index=arg_idx,
                    )
                )
                base_expr_text_set.add(base_text)

    if not member_accesses_to_lift:
        return None

    # Use the first base expression text (they should all be the same in our pattern)
    base_expr_text = next(iter(base_expr_text_set)) if base_expr_text_set else ""

    statement_start = find_statement_start(call_cursor, content, ancestors)

    return CallSiteRewrite(
        call_cursor=call_cursor,
        file_path=file_path,
        statement_start_offset=statement_start,
        member_accesses=member_accesses_to_lift,
        base_expr_text=base_expr_text,
    )


def generate_lifted_var_name(field_name: str, counter: int) -> str:
    """Generate a unique name for a lifted variable."""
    return f"_xj_lifted_{field_name}_{counter}"


def get_indentation(content: bytes, offset: int) -> str:
    """Get the indentation at the given offset (start of line)."""
    # Look ahead to find indentation
    indent_end = offset
    while indent_end < len(content) and content[indent_end : indent_end + 1] in (b" ", b"\t"):
        indent_end += 1

    return content[offset:indent_end].decode("utf-8")


def lift_subfield_args(
    compdb: compilation_database.CompileCommands,
):
    """Lift struct member accesses from function call arguments into local variables.

    This refactoring finds call sites where:
    1. One argument is a struct pointer X
    2. Another argument is a member access X->F

    It creates a local variable before the call:
        TYP newvar = X->F;

    And replaces X->F in the call with newvar.

    Args:
        json_path: Path to JSON file (reserved for future analysis data)
        compdb: Compilation database for the project
        prev: Path to previous codebase (unused, for interface compatibility)
        current_codebase: Path to current codebase directory
    """
    print("\n" + "=" * 80)
    print("LIFTING STRUCT MEMBER ACCESSES FROM CALL ARGUMENTS")
    print("=" * 80)

    # Parse the project
    index = c_refact.create_xj_clang_index()
    tus = c_refact.parse_project(index, compdb)

    # Collect all call expressions by location
    call_cursors_by_loc = c_refact.collect_cursors_by_loc(tus, [CursorKind.CALL_EXPR])

    print(f"\nFound {sum(len(v) for v in call_cursors_by_loc.values())} call expressions")

    # Analyze call sites to find rewrite opportunities
    rewrites_by_file: dict[str, list[CallSiteRewrite]] = {}

    for tu_path, tu in tus.items():
        if Path(tu_path).suffix != ".i":
            print(f"TENJIN: NOTE: Subfield arg lifting skipping non-preprocessed file: {tu_path}")
            # If we ended up not expanding source, we cannot reliably apply rewrites, so skip this file.
            continue

        content = Path(tu_path).read_bytes()
        for cursor, ancestors in yield_matching_cursors(tu.cursor, [CursorKind.CALL_EXPR]):
            rewrite = analyze_call_site(cursor, content, tu_path, ancestors)
            if rewrite:
                rewrites_by_file.setdefault(tu_path, []).append(rewrite)

    print(f"\nFound {sum(len(v) for v in rewrites_by_file.values())} call sites to rewrite")

    # Apply rewrites
    with batching_rewriter.BatchingRewriter() as rewriter:
        for file_path, rewrites in rewrites_by_file.items():
            content = rewriter.get_content(file_path)
            var_counter = 0

            # Sort by statement start offset (descending) to avoid offset shifts
            rewrites_sorted = sorted(rewrites, key=lambda r: r.statement_start_offset, reverse=True)

            for rewrite in rewrites_sorted:
                # Get indentation for the statement
                indent = get_indentation(content, rewrite.statement_start_offset)

                # Generate declarations for each member access
                declarations = []
                replacements = []

                for member_access in rewrite.member_accesses:
                    var_name = generate_lifted_var_name(member_access.field_name, var_counter)
                    var_counter += 1

                    # Construct the declaration
                    # field_type already includes the type, we just need to add the variable name
                    field_type_clean = member_access.field_type.strip()

                    # Get the member access text (e.g., "ptr->field")
                    member_text = content[
                        member_access.start_offset : member_access.end_offset
                    ].decode("utf-8")

                    decl = f"{indent}{field_type_clean} {var_name} = {member_text};\n"
                    declarations.append(decl)

                    # Record the replacement (member access -> var_name)
                    replacements.append((
                        member_access.start_offset,
                        member_access.end_offset - member_access.start_offset,
                        var_name,
                    ))

                # Insert declarations before the statement
                combined_decls = "".join(declarations)
                rewriter.add_rewrite(
                    file_path,
                    rewrite.statement_start_offset,
                    0,  # Insert, don't replace
                    combined_decls,
                )

                # Replace member accesses in the call arguments
                for start_offset, length, new_text in replacements:
                    rewriter.add_rewrite(file_path, start_offset, length, new_text)

                # print(f"  Rewriting call at {file_path}:{rewrite.call_cursor.location.line}")
                # print(f"    Lifting {len(rewrite.member_accesses)} member access(es)")

    print("\n" + "=" * 80)
    print("STRUCT MEMBER LIFTING COMPLETE")
    print("=" * 80)
