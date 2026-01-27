"""
Type definitions can end up replicated across translation units
for two main reasons:

    - Header files included in multiple source files.
    - Manually duplicated type definitions.

We should handle both cases; regardless of origin, we must maintain
consistency across all instances of a type definition to avoid
undefined behavior:

+--------------------------------------------------------------------+
|                                                                    |
|  When we modify named type definitions in any translation unit,    |
|  we must replicate those changes across every translation unit.    |
|                                                                    |
+--------------------------------------------------------------------+

At least in theory, two #includes of the same file in different
translation units could result in different expanded text. So at least
in theory, one could have a translation unit A that gets definition D
from header H, and another translation unit B that also includes H but
gets an equivalent-but-not-textually-identical definition D elsewhere.
Even without textual identity, macro-expanded definitions can still be
equivalent, either due to int/signed int, or use of typedefs.

So we have two challenges:

    1. Ensure that changes to type definitions are replicated across all
       translation units.

    2. Avoid having type definition changes interfere with refolding.
       Naively, any change to a type definition from a header file
       would prevent refolding the header contents back to an #include.

### Challenge 1: Replication

For challenge 1, we'll require that all type definitions be textually
IDENTICAL across translation units. It would be nice to be more flexible
and allow equivalent definitions to be recognized, but that's harder than
one might think:

    To avoid trivial whitespace diffs
    from making definitions appear to differ, we could either
        - compare token sequences rather than raw text, or
        - run `clang-format` to ensure consistent formatting.
            (merely doing `" ".join(defntext.split())` does not handle
            things like spaces before semicolons.)
    If we could run clang-format on the whole .i file that would be convenient
    but doing so would interfere with refolding.  If we only cared about
    replicating entire changed definitions, we would only need to know
    the spans. HOWEVER there is no distinguished canonical definition;
    each translation unit may make separate modifications as needed.

If all definitions are byte-for-byte identical we can precisely map
sub-edits between them.

### Challenge 2: Refolding

For challenge 2, if we change the definition in the header and restore
the un-modified definition in the .nolines.i file, refolding might be
able to restore the #include. HOWEVER other modifications might prevent
the #include from being restored, in which case we want to keep the
modified definition in the .nolines.i file -- otherwise, the modification
will be lost!

    Note that because #includes can be nested and partially restored
    during refolding, it is not super straightforward to determine
    which includes were restored and which were not.

### Overall Approach

So, for a given codebase, we want to construct a mapping from
type specifiers (e.g. `struct foo`) to a list of every definition
of that type in every translation unit. If any definitions are not
textually identical, we should report an error and abort.

We can use this information to perform replication of changes
across all translation units. Specifically, given a list of edits,
in the form (X_filepath, X_offset, length, replacement_text), we can
process each edit as follows:

    1. Determine if the edit overlaps a definition for some type T.
       If not, skip it.

    2. For each other definition of T in some other translation unit U,
       compute the corresponding edit given by:
          (U_filepath, U_offset, length, replacement_text)
       where U_offset = U_def_start + (X_offset - T_def_start)

    3. Add those edits to the list of edits to be applied.

This can be done as a processing step before applying any edits.

During refolding, we'll:
    - Modify the definition in the header file, if there is one.
    - Restore the original definition in the .nolines.i files.
    - Perform refolding.
    - If the un-modified definition remains in the refolded .c file,
      replace it with the modified definition.
"""

from dataclasses import dataclass
import hashlib

from clang.cindex import CursorKind, TranslationUnit, Cursor, Token  # type: ignore

type QuasiUniformSymbolSpecifier = str


@dataclass
class TypeDefinition:
    filepath: str
    quss_for_debugging: QuasiUniformSymbolSpecifier
    tokenhash: str
    def_start: int
    def_length: int


type TranslationUnitPath = str
type OpaqueDictKey = str


def replicate_type_modifications(
    rewrites: dict[TranslationUnitPath, list[tuple[int, int, str]]],
    type_definition_equivalences: dict[OpaqueDictKey, list[TypeDefinition]],
) -> dict[TranslationUnitPath, list[tuple[int, int, str]]]:
    type_definitions_by_file: dict[
        TranslationUnitPath, dict[OpaqueDictKey, list[TypeDefinition]]
    ] = {}
    for opaque_key, defs in type_definition_equivalences.items():
        for defn in defs:
            type_definitions_by_file.setdefault(defn.filepath, {}).setdefault(
                opaque_key, []
            ).append(defn)

    new_rewrites: dict[TranslationUnitPath, list[tuple[int, int, str]]] = {}

    for filepath, file_rewrites in rewrites.items():
        if filepath not in new_rewrites:
            new_rewrites[filepath] = []
        for offset, length, replacement_text in file_rewrites:
            new_rewrites[filepath].append((offset, length, replacement_text))

            # Check if this rewrite overlaps any type definitions
            for opaque_key, definitions_for_filepath_and_key in type_definitions_by_file.get(
                filepath, {}
            ).items():
                for defn in definitions_for_filepath_and_key:
                    assert defn.filepath == filepath
                    def_start = defn.def_start
                    def_end = defn.def_start + defn.def_length
                    rewrite_start = offset
                    rewrite_end = offset + length

                    no_overlap = rewrite_end <= def_start or rewrite_start >= def_end
                    if no_overlap:
                        continue

                    fully_contained = rewrite_end <= def_end and rewrite_start >= def_start
                    if not fully_contained:
                        raise ValueError(
                            f"Rewrite at {filepath}:{offset}-{offset + length} "
                            f"partially overlaps type definition "
                            f"at offsets {def_start}-{def_end}; cannot replicate."
                        )

                    # Overlap detected; replicate the edit to other definitions
                    for other_defn in type_definition_equivalences[opaque_key]:
                        if other_defn.filepath == filepath:
                            # Each type has only a single definition within a given
                            # translation unit, and we've already handled this one.
                            continue
                        # Compute corresponding offset in the other definition
                        relative_offset = rewrite_start - def_start
                        other_offset = other_defn.def_start + relative_offset
                        new_rewrites.setdefault(other_defn.filepath, []).append((
                            other_offset,
                            length,
                            replacement_text,
                        ))
    return new_rewrites


def tokenhash(tokens: list[Token]) -> str:
    tokens_sp = [t.spelling for t in tokens]
    hash_digest = hashlib.blake2b((" ".join(tokens_sp)).encode(), digest_size=8).hexdigest()
    return hash_digest


def quss(cursor: Cursor, ancestor: Cursor | None) -> QuasiUniformSymbolSpecifier:
    parts = []
    if ancestor and ancestor.kind == CursorKind.TYPEDEF_DECL:
        parts.append("typedef")

    if cursor.is_anonymous():
        parts.append("anon")

    if cursor.kind == CursorKind.STRUCT_DECL:
        parts.append("struct")
    elif cursor.kind == CursorKind.UNION_DECL:
        parts.append("union")

    parts.append(cursor.spelling)

    return "+".join(parts)


type VariableName = str


def collect_type_definitions(
    translation_units: list[TranslationUnit],
    var_decl_fn_ptr_arg_lparen_locs: dict[str, dict[str, int]],
) -> dict[QuasiUniformSymbolSpecifier | VariableName, list[TypeDefinition]]:
    """
    Collect type definitions from all translation units.

    Returns:
        A mapping from specifiers to lists of TypeDefinition instances.

    A specifier is, from the perspective of a client of this function,
    an arbitrary (opaque) key that distinguishes equivalence classes
    of TypeDefinitions that should be kept in sync.
    """

    # Suppose we have two translation units, A & B, that each contain
    #    extern struct { int x; } g1, g3;
    #    extern struct { int x; } g2;
    # (with definitions in a third translation unit C).
    # Then:
    #   * A.g1 and A.g2 are distinct types, and edits to one should not
    #     affect the other.
    #   * Likewise, B.g2 and B.g1 are distinct types.
    #   * A.g1 and A.g3 are the same type, so edits should be deduplicated.
    #   * But A.g1 and B.g1 are the same type, and edits to one should
    #     be replicated to the other.
    #
    # That is to say: type matching is not purely by hash/contents,
    # since g1 and g2 have different types. Type matching is also not
    # purely by name associated with decl, since A.g1 and A.g3 have the
    # same type. (Nor is it by sets of names: the type associated with
    # the definition of g1 could have arbitrarily many names associated,
    # including or not including g3.)
    #
    # The way we handle this is to have a map for anonymous types (and
    # direct function pointer types) associated with program variables,
    # keyed by
    #    TU -> variable name -> TypeDefinition
    #
    # In the example above, g1 and g3 will map to the same TypeDefinition,
    # and g2 will map to a different one.  Then we ensure that all type
    # definitions associated with the same variable name across TUs are
    # grouped together. (Note we assume alpha-renaming has already been
    # done on static variables to ensure uniqueness within each TU.)
    #
    # A separate issue is that function prototypes are permitted to vary
    # in the presence or absence of parameter names, so we cannot assume
    # the full prototype text is identical across TUs. To handle this in
    # the case of variables with function pointer types, we note that at
    # least for now, we can get by with only tracking the region of text
    # from the return type to parameter list start.

    type_definitions_by_tu_and_varname: dict[str, dict[VariableName, TypeDefinition]] = {}
    unique_type_definitions: dict[
        QuasiUniformSymbolSpecifier | VariableName, tuple[Cursor, Cursor | None]
    ] = {}
    type_definitions_by_type: dict[QuasiUniformSymbolSpecifier, list[TypeDefinition]] = {}

    current_tu_path = ""

    def mk_TD(node: Cursor, tokhash: str, quss: str) -> TypeDefinition:
        file = node.location.file
        assert file is not None
        filepath = file.name
        extent = node.extent
        def_start = extent.start.offset
        def_end = extent.end.offset
        def_length = def_end - def_start
        return TypeDefinition(
            filepath=filepath,
            quss_for_debugging=quss,
            tokenhash=tokhash,
            def_start=def_start,
            def_length=def_length,
        )

    def visit(node: Cursor, ancestors: list[Cursor]) -> None:
        if node.kind in (CursorKind.STRUCT_DECL, CursorKind.UNION_DECL):
            if node.is_definition():
                if node.is_anonymous():
                    # print(f"Found anonymous type definition: {node.spelling} at {node.location}")
                    # print(list(str(a.kind) + ":" + a.spelling for a in ancestors))
                    if ancestors[-1].kind != CursorKind.VAR_DECL:
                        return
                    # Anonymous types defined within functions are guaranteed
                    # to be unique to that function, i.e. their equivalence class is trivial,
                    # so we can skip them.
                    if any(a.kind == CursorKind.FUNCTION_DECL for a in ancestors):
                        return

                    # print(">>>>>> " + str(list(str(a.kind) for a in ancestors)))
                    tokhash = tokenhash(list(node.get_tokens()))
                    type_definitions_by_tu_and_varname.setdefault(current_tu_path, {})[
                        ancestors[-1].spelling
                    ] = mk_TD(node, tokhash, quss(node, ancestors[-1]))
                else:
                    # print(f"Found named type definition: {node.spelling} at {node.location}")
                    # print(list(str(a.kind) + ":" + a.spelling for a in ancestors))
                    unique_type_definitions[str(node.location)] = (
                        node,
                        ancestors[-1] if ancestors else None,
                    )
        for child in node.get_children():
            visit(child, [*ancestors, node])

    for tu in translation_units:
        current_tu_path = tu.spelling
        visit(tu.cursor, [])

    for node, ancestor in unique_type_definitions.values():
        tokhash = tokenhash(list(node.get_tokens()))
        type_name = quss(node, ancestor)
        file = node.location.file
        if file is not None:
            type_definitions_by_type.setdefault(type_name, []).append(
                mk_TD(node, tokhash, type_name)
            )

    for tu_path, d in var_decl_fn_ptr_arg_lparen_locs.items():
        for varname, lparen_offset in d.items():
            tu_varname_map = type_definitions_by_tu_and_varname.setdefault(tu_path, {})
            if varname in tu_varname_map:
                raise ValueError("already recorded via AST visit: " + varname)
            q = f"var+{varname}"
            tu_varname_map[q] = TypeDefinition(
                filepath=tu_path,
                quss_for_debugging=q,
                tokenhash="<ignored>",
                def_start=lparen_offset,
                def_length=2,
            )

    classes_by_varname: dict[VariableName, list[TypeDefinition]] = {}
    tokenhashes_by_varname: dict[VariableName, str] = {}

    for varname_to_td in type_definitions_by_tu_and_varname.values():
        for varname, td in varname_to_td.items():
            classes_by_varname.setdefault(varname, []).append(td)
            if varname not in tokenhashes_by_varname:
                tokenhashes_by_varname[varname] = td.tokenhash
            else:
                assert tokenhashes_by_varname[varname] == td.tokenhash, (
                    f"Hash mismatch for anonymous type associated with variable {varname}; "
                    f"\nold hash: {tokenhashes_by_varname[varname]}\nnew hash: {td.tokenhash}"
                )

    return {**type_definitions_by_type, **classes_by_varname}
