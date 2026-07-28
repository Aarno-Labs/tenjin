"""Rewrite K&R (old-style) C syntax into ISO C prototype syntax.

This pass runs directly after preprocessor expansion, so each translation unit
is a self-contained `.i` file: every typedef, every declaration, and every call
site of a given function is visible in one place, which is what lets us fix up
unprototyped declarations from their definitions.

The constructs handled here are:

  (A)   old-style definitions with an identifier list,
            `int f(x, s) int x; char *s; { ... }`
  (A')  ... whose parameters are implicitly `int`,
            `int f(a, b) { ... }`
  (A'') ... whose *return* type is implicitly `int`,
            `static f(a) int a; { ... }`
  (B)   unprototyped definitions,  `int f() { ... }`  ->  `int f(void) { ... }`
  (C)   unprototyped declarations, `int f();`         ->  the definition's signature

Unprototyped *function pointer* types (`int (*fp)();`, `typedef int F();`,
no-prototype function types in casts and struct fields) are counted but not
rewritten; reconstructing their parameter lists needs assignment/call-site flow
analysis rather than a local syntactic fix.

Note that libclang reports an old-style definition as a `FUNCTIONPROTO`, not a
`FUNCTIONNOPROTO`, so category A cannot be detected from the type alone. What
distinguishes it is where the parameter cursors live: for an old-style
definition, a parameter's extent covers its entry in the declaration list
*after* the closing paren (or, for an implicitly-typed parameter, just the bare
identifier inside the parens).
"""

import dataclasses
from enum import Enum
from pathlib import Path
from typing import Callable

from clang.cindex import (  # type: ignore
    Cursor,
    CursorKind,
    LinkageKind,
    Token,
    TranslationUnit,
    TypeKind,
)

import batching_rewriter
import c_refact
import cindex_helpers
import compilation_database
from tenj_types import CIdentifier, FilePathStr


# Suffix given to the incoming (promoted) parameter when we must preserve the
# ABI of an old-style definition; see `_analyze_old_style_definition`.
KNR_PARAM_SUFFIX = "_xjknr"

# Types that undergo the default argument promotions when passed to a function
# that has no visible prototype. A parameter of any *other* type is passed
# identically whether or not the callee is prototyped, which is what makes the
# straightforward rewrite ABI-neutral for it.
PROMOTED_TYPE_KINDS = frozenset({
    TypeKind.BOOL,
    TypeKind.CHAR_U,
    TypeKind.UCHAR,
    TypeKind.CHAR_S,
    TypeKind.SCHAR,
    TypeKind.USHORT,
    TypeKind.SHORT,
    TypeKind.FLOAT,
})

# Diagnostics that this pass can plausibly *introduce*: making a prototype
# visible turns a previously-unchecked call into a checked one. Seeing one of
# these after a rewrite means a call site disagreed with the signature we chose,
# so the translation unit gets rolled back.
RISKY_DIAGNOSTIC_SUBSTRINGS = (
    "incompatible",
    "too many arguments",
    "too few arguments",
    "conflicting types",
    "passing argument",
)


# A function's identity for the purposes of agreeing on one signature across the
# whole project. Functions with internal linkage are keyed per-file, because
# `prep_uniquify_statics` has not run yet, so two files may each have their own
# `static int helper()` with unrelated signatures.
type FnKey = tuple[FilePathStr, CIdentifier]


def fn_key(cursor: Cursor, tu_path: FilePathStr) -> FnKey:
    if cursor.linkage == LinkageKind.INTERNAL:
        return (tu_path, cursor.spelling)
    return ("", cursor.spelling)


@dataclasses.dataclass(frozen=True)
class Signature:
    """The parameter list we intend a function to end up with, as source text."""

    params: tuple[str, ...]
    is_variadic: bool = False

    def render(self) -> str:
        if not self.params:
            return "(void)"
        suffix = ", ..." if self.is_variadic else ""
        return "(" + ", ".join(self.params) + suffix + ")"


class SiteKind(Enum):
    OLD_STYLE_DEFN = "old-style definition"
    NOPROTO_DEFN = "unprototyped definition"
    NOPROTO_DECL = "unprototyped declaration"
    IMPLICIT_INT_RESULT = "implicit int return type"


@dataclasses.dataclass
class ParamInfo:
    """One parameter of an old-style definition.

    `declared_text` is the declarator as it should appear in a prototype, e.g.
    `char *s`; `promoted_type` is the type the caller actually passes when no
    prototype is visible, and differs from the declared type only for the types
    in `PROMOTED_TYPE_KINDS`.
    """

    name: CIdentifier
    declared_text: str
    promoted_type: str
    is_promoted: bool


@dataclasses.dataclass
class RewriteSite:
    """One place in one `.i` file that this pass may edit."""

    kind: SiteKind
    key: FnKey
    # Half-open byte range to replace, and the text to replace it with. For
    # `IMPLICIT_INT_RESULT` the range is empty (a pure insertion).
    start: int
    end: int
    # Populated during the decide phase, since a declaration's replacement text
    # depends on a definition that may live in another translation unit.
    replacement: str | None = None
    # For old-style definitions only: where to insert the shadow copies, and
    # what they are.
    body_insert_offset: int | None = None
    body_insert_text: str = ""


@dataclasses.dataclass
class TuAnalysis:
    """What one translation unit contributes to the project-wide decision."""

    tu_path: FilePathStr
    sites: list[RewriteSite] = dataclasses.field(default_factory=list)
    # The signature a function will have once its definition is rewritten. This
    # is authoritative: at most one translation unit defines a given function.
    definition_signatures: dict[FnKey, Signature] = dataclasses.field(default_factory=dict)
    # Signatures taken from already-prototyped *declarations*, used only when no
    # definition is available. Two of these may legitimately disagree by
    # parameter name alone, so a conflict here just makes them unusable rather
    # than poisoning the function.
    declaration_signatures: dict[FnKey, Signature] = dataclasses.field(default_factory=dict)
    # Functions that are referenced at all (called, or address taken).
    referenced: set[FnKey] = dataclasses.field(default_factory=set)
    # Functions passed a nonzero number of arguments at some call site.
    called_with_args: set[FnKey] = dataclasses.field(default_factory=set)
    # Functions this translation unit says must not be touched anywhere.
    unhandled: dict[FnKey, str] = dataclasses.field(default_factory=dict)
    noproto_type_mentions: int = 0


@dataclasses.dataclass
class KnrPassSummary:
    rewritten: dict[SiteKind, int] = dataclasses.field(default_factory=dict)
    skipped: dict[str, int] = dataclasses.field(default_factory=dict)
    noproto_type_mentions: int = 0
    rolled_back_tus: list[FilePathStr] = dataclasses.field(default_factory=list)

    def note_rewrite(self, kind: SiteKind) -> None:
        self.rewritten[kind] = self.rewritten.get(kind, 0) + 1

    def note_skip(self, reason: str) -> None:
        self.skipped[reason] = self.skipped.get(reason, 0) + 1

    def describe(self) -> str:
        lines = []
        total = sum(self.rewritten.values())
        lines.append(f"K&R elimination: rewrote {total} site(s)")
        for kind in SiteKind:
            n = self.rewritten.get(kind, 0)
            if n:
                lines.append(f"    {n:5d}  {kind.value}")
        if self.skipped:
            lines.append("  skipped:")
            for reason, n in sorted(self.skipped.items(), key=lambda kv: -kv[1]):
                lines.append(f"    {n:5d}  {reason}")
        if self.noproto_type_mentions:
            lines.append(
                f"  {self.noproto_type_mentions} unprototyped function-pointer type(s) left alone"
            )
        if self.rolled_back_tus:
            lines.append(f"  rolled back {len(self.rolled_back_tus)} TU(s):")
            for p in self.rolled_back_tus:
                lines.append(f"      {p}")
        return "\n".join(lines)


def _decode(src: bytes, start: int, end: int) -> str | None:
    try:
        return src[start:end].decode("utf-8")
    except UnicodeDecodeError:
        return None


def _find_param_parens(cursor: Cursor, tokens: list[Token]) -> tuple[int, int, int] | None:
    """Locate the declarator's parameter list within `tokens`.

    Returns `(name_index, lparen_index, rparen_index)`, or None if the shape is
    not one we recognize (in which case the function is left alone).
    """
    for i, tok in enumerate(tokens):
        if tok.spelling != cursor.spelling:
            continue
        if i + 1 >= len(tokens) or tokens[i + 1].spelling != "(":
            continue
        depth = 0
        for j in range(i + 1, len(tokens)):
            spelling = tokens[j].spelling
            if spelling == "(":
                depth += 1
            elif spelling == ")":
                depth -= 1
                if depth == 0:
                    return (i, i + 1, j)
        return None
    return None


def _find_body_lbrace(tokens: list[Token], after: int) -> int | None:
    for j in range(after + 1, len(tokens)):
        if tokens[j].spelling == "{":
            return j
    return None


def _find_decl_list_end(tokens: list[Token], rparen_index: int, brace_index: int) -> int:
    """Index of the last token belonging to the old-style declaration list.

    We stop at the last `;` rather than at the token before the body so that a
    trailing attribute (`f(a) int a; __attribute__((x)) { ... }`) survives.
    """
    for j in range(brace_index - 1, rparen_index, -1):
        if tokens[j].spelling == ";":
            return j
    return rparen_index


def _has_top_level_comma(param: Cursor) -> bool:
    """True when the parameter's extent spans a joined declarator.

    For `int a, b;` in a declaration list, clang reports `b`'s extent as the
    whole `int a, b`, whose text cannot be reused as a single declarator.
    """
    depth = 0
    for tok in param.get_tokens():
        spelling = tok.spelling
        if spelling in ("(", "["):
            depth += 1
        elif spelling in (")", "]"):
            depth -= 1
        elif spelling == "," and depth == 0:
            return True
    return False


def _render_param(param: Cursor, src: bytes, paren_span: tuple[int, int]) -> str | None:
    """Render one parameter of an old-style definition as a prototype declarator."""
    start, end = param.extent.start.offset, param.extent.end.offset
    text = _decode(src, start, end)
    if text is None:
        return None
    text = text.strip()

    inside_parens = paren_span[0] <= start and end <= paren_span[1]
    if inside_parens and text == param.spelling:
        # An identifier-list entry with no matching declaration: implicitly int.
        return f"int {param.spelling}"

    if text and not _has_top_level_comma(param):
        return text

    # Joined declarators lose their type in the extent text, so fall back to
    # rendering from the type. This drops top-level qualifiers, which is inert
    # for `const`/`register` but not for `volatile`.
    if param.type.is_volatile_qualified():
        return None
    return cindex_helpers.render_declaration_sans_qualifiers(param.type, param.spelling)


def _is_promoted(param: Cursor) -> bool:
    return param.type.get_canonical().kind in PROMOTED_TYPE_KINDS


def _unique_incoming_name(base: CIdentifier, taken: set[str]) -> CIdentifier:
    candidate = f"{base}{KNR_PARAM_SUFFIX}"
    n = 0
    while candidate in taken:
        candidate = f"{base}{KNR_PARAM_SUFFIX}{n}"
        n += 1
    return candidate


def _type_mentions_noproto(ty, depth: int = 0) -> bool:
    """True if `ty` is, or contains, a function type written without a prototype."""
    if depth > 8:
        return False
    kind = ty.kind
    if kind == TypeKind.FUNCTIONNOPROTO:
        return True
    if kind == TypeKind.POINTER:
        return _type_mentions_noproto(ty.get_pointee(), depth + 1)
    if kind in (
        TypeKind.CONSTANTARRAY,
        TypeKind.INCOMPLETEARRAY,
        TypeKind.VARIABLEARRAY,
    ):
        return _type_mentions_noproto(ty.get_array_element_type(), depth + 1)
    if kind == TypeKind.ELABORATED:
        return _type_mentions_noproto(ty.get_named_type(), depth + 1)
    if kind == TypeKind.TYPEDEF:
        decl = ty.get_declaration()
        if decl is not None and decl.kind == CursorKind.TYPEDEF_DECL:
            return _type_mentions_noproto(decl.underlying_typedef_type, depth + 1)
    if kind == TypeKind.FUNCTIONPROTO:
        return _type_mentions_noproto(ty.get_result(), depth + 1)
    return False


def _declspec_tokens_lack_a_type(tokens: list[Token], name_index: int) -> bool:
    """True when the tokens before the function name contain no type specifier.

    Only storage-class and function specifiers may precede the name of a
    function whose return type is implicitly `int`.
    """
    non_type_specifiers = {
        "static",
        "extern",
        "auto",
        "register",
        "inline",
        "__inline",
        "__inline__",
        "_Noreturn",
        "__attribute__",
        "(",
        ")",
        ",",
    }
    saw_attribute = False
    for tok in tokens[:name_index]:
        spelling = tok.spelling
        if spelling == "__attribute__":
            saw_attribute = True
        if saw_attribute:
            # Everything inside `__attribute__((...))` is opaque to us; the
            # closing paren returns us to declaration-specifier territory.
            if spelling == ")":
                saw_attribute = False
            continue
        if spelling not in non_type_specifiers:
            return False
    return True


def analyze_translation_unit(tu_path: FilePathStr, tu: TranslationUnit, src: bytes) -> TuAnalysis:
    """Find every site in one preprocessed translation unit that this pass may edit."""
    analysis = TuAnalysis(tu_path=tu_path)

    for cursor in tu.cursor.walk_preorder():
        if cursor.kind in (
            CursorKind.CALL_EXPR,
            CursorKind.DECL_REF_EXPR,
        ):
            _record_reference(cursor, tu_path, analysis)
            continue
        if cursor.kind in (
            CursorKind.VAR_DECL,
            CursorKind.FIELD_DECL,
            CursorKind.TYPEDEF_DECL,
            CursorKind.PARM_DECL,
        ):
            ty = (
                cursor.underlying_typedef_type
                if cursor.kind == CursorKind.TYPEDEF_DECL
                else cursor.type
            )
            if _type_mentions_noproto(ty):
                analysis.noproto_type_mentions += 1

    for cursor in tu.cursor.get_children():
        if cursor.kind != CursorKind.FUNCTION_DECL:
            continue
        if cursor.location.file is None or cursor.location.file.name != tu_path:
            continue
        _analyze_function(cursor, tu_path, src, analysis)

    return analysis


def _record_reference(cursor: Cursor, tu_path: FilePathStr, analysis: TuAnalysis) -> None:
    if cursor.kind == CursorKind.DECL_REF_EXPR:
        referenced = cursor.referenced
        if referenced is not None and referenced.kind == CursorKind.FUNCTION_DECL:
            analysis.referenced.add(fn_key(referenced, tu_path))
        return

    callee = cursor.referenced
    if callee is None or callee.kind != CursorKind.FUNCTION_DECL:
        return
    key = fn_key(callee, tu_path)
    analysis.referenced.add(key)
    # `get_arguments` on a call expression yields the argument expressions.
    if any(True for _ in cursor.get_arguments()):
        analysis.called_with_args.add(key)


def _analyze_function(
    cursor: Cursor,
    tu_path: FilePathStr,
    src: bytes,
    analysis: TuAnalysis,
) -> None:
    key = fn_key(cursor, tu_path)
    tokens = list(cursor.get_tokens())
    if not tokens:
        return
    parens = _find_param_parens(cursor, tokens)
    if parens is None:
        # Without the parameter list we cannot describe this function's
        # signature, so nothing anywhere may be rewritten on its behalf --
        # including an unprototyped declaration of it in another TU.
        analysis.unhandled[key] = "could not locate parameter list"
        return
    name_index, lparen_index, rparen_index = parens
    paren_span = (tokens[lparen_index].extent.start.offset, tokens[rparen_index].extent.end.offset)

    params = list(cursor.get_arguments())
    is_definition = cursor.is_definition()
    brace_index = _find_body_lbrace(tokens, rparen_index) if is_definition else None

    if params:
        any_outside = any(
            param.extent.start.offset >= paren_span[1] or param.extent.end.offset <= paren_span[0]
            for param in params
        )
        any_bare = any(
            _decode(src, param.extent.start.offset, param.extent.end.offset) == param.spelling
            for param in params
        )
        if any_outside or any_bare:
            _analyze_old_style_definition(
                cursor,
                key,
                src,
                tokens,
                lparen_index,
                rparen_index,
                brace_index,
                paren_span,
                params,
                analysis,
            )
        else:
            _record_signature(analysis, key, _signature_of_prototype(cursor, src), is_definition)
    elif cursor.type.kind == TypeKind.FUNCTIONNOPROTO:
        analysis.sites.append(
            RewriteSite(
                kind=SiteKind.NOPROTO_DEFN if is_definition else SiteKind.NOPROTO_DECL,
                key=key,
                start=paren_span[0],
                end=paren_span[1],
            )
        )
        if is_definition:
            _record_signature(analysis, key, Signature(params=()), is_definition=True)
    else:
        _record_signature(analysis, key, _signature_of_prototype(cursor, src), is_definition)

    if _declspec_tokens_lack_a_type(tokens, name_index):
        analysis.sites.append(
            RewriteSite(
                kind=SiteKind.IMPLICIT_INT_RESULT,
                key=key,
                start=tokens[name_index].extent.start.offset,
                end=tokens[name_index].extent.start.offset,
                replacement="int ",
            )
        )


def _record_signature(
    analysis: TuAnalysis, key: FnKey, signature: Signature, is_definition: bool
) -> None:
    target = analysis.definition_signatures if is_definition else analysis.declaration_signatures
    target.setdefault(key, signature)


def _signature_of_prototype(cursor: Cursor, src: bytes) -> Signature:
    """The parameter list of an already-prototyped declaration, as source text."""
    params = []
    for param in cursor.get_arguments():
        text = _decode(src, param.extent.start.offset, param.extent.end.offset)
        if text is None:
            text = cindex_helpers.render_declaration_sans_qualifiers(param.type, param.spelling)
        params.append(text.strip())
    return Signature(params=tuple(params), is_variadic=cursor.type.is_function_variadic())


def _analyze_old_style_definition(
    cursor: Cursor,
    key: FnKey,
    src: bytes,
    tokens: list[Token],
    lparen_index: int,
    rparen_index: int,
    brace_index: int | None,
    paren_span: tuple[int, int],
    params: list[Cursor],
    analysis: TuAnalysis,
) -> None:
    if brace_index is None:
        analysis.unhandled[key] = "old-style parameter list outside a definition"
        return

    infos: list[ParamInfo] = []
    promoted_types = list(cursor.type.argument_types())
    for index, param in enumerate(params):
        declared_text = _render_param(param, src, paren_span)
        if declared_text is None:
            analysis.unhandled[key] = "unrenderable parameter"
            return
        promoted_type = (
            promoted_types[index].spelling if index < len(promoted_types) else param.type.spelling
        )
        infos.append(
            ParamInfo(
                name=param.spelling,
                declared_text=declared_text,
                promoted_type=promoted_type,
                is_promoted=_is_promoted(param),
            )
        )

    # `int f(c) char c;` is called with `c` promoted to `int`. Emitting
    # `int f(char c)` would move that conversion to the call site, which is fine
    # for every call that can see the new prototype but *not* for a call made
    # through an unprototyped function pointer -- and those still exist after
    # this pass, since no-prototype function pointer types are out of scope.
    # Nothing would diagnose the mismatch, so instead we keep the promoted
    # signature and reintroduce the declared parameter as a body-local, which is
    # exactly the semantics C ascribes to an old-style definition:
    #
    #     int f(int c_xjknr) { char c = c_xjknr; ... }
    #
    # Parameters of every other type are passed identically either way, so they
    # are rewritten directly and need no shadow copy.
    if any(info.is_promoted for info in infos):
        taken = {tok.spelling for tok in tokens}
        prototype_params = []
        shadow_decls = []
        for info in infos:
            if not info.is_promoted:
                prototype_params.append(info.declared_text)
                continue
            incoming = _unique_incoming_name(info.name, taken)
            taken.add(incoming)
            prototype_params.append(f"{info.promoted_type} {incoming}")
            shadow_decls.append(f"{info.declared_text} = {incoming};")
        signature = Signature(params=tuple(prototype_params))
        body_insert_text = " " + " ".join(shadow_decls)
        body_insert_offset = tokens[brace_index].extent.start.offset + 1
    else:
        signature = Signature(params=tuple(info.declared_text for info in infos))
        body_insert_text = ""
        body_insert_offset = None

    decl_list_end_index = _find_decl_list_end(tokens, rparen_index, brace_index)
    analysis.sites.append(
        RewriteSite(
            kind=SiteKind.OLD_STYLE_DEFN,
            key=key,
            start=paren_span[0],
            end=tokens[decl_list_end_index].extent.end.offset,
            replacement=signature.render(),
            body_insert_offset=body_insert_offset,
            body_insert_text=body_insert_text,
        )
    )
    analysis.definition_signatures[key] = signature


@dataclasses.dataclass
class ProjectDecision:
    """The one signature per function that every translation unit must use."""

    signatures: dict[FnKey, Signature] = dataclasses.field(default_factory=dict)
    unhandled: dict[FnKey, str] = dataclasses.field(default_factory=dict)
    referenced: set[FnKey] = dataclasses.field(default_factory=set)
    called_with_args: set[FnKey] = dataclasses.field(default_factory=set)


def decide_signatures(analyses: list[TuAnalysis]) -> ProjectDecision:
    """Agree on one signature per function across the whole project.

    Two translation units that disagree about a function -- which can happen for
    an external name declared unprototyped in one place and defined in another
    with a shape we could not render -- leave it alone everywhere, rather than
    emitting a link-level ABI mismatch that nothing downstream would catch.
    """
    decision = ProjectDecision()

    for analysis in analyses:
        decision.unhandled.update(analysis.unhandled)
        decision.referenced |= analysis.referenced
        decision.called_with_args |= analysis.called_with_args

    # A valid program defines each function once, so two definitions that
    # disagree mean we misread one of them; leave the function alone.
    for analysis in analyses:
        for key, signature in analysis.definition_signatures.items():
            existing = decision.signatures.get(key)
            if existing is None:
                decision.signatures[key] = signature
            elif existing != signature:
                decision.unhandled[key] = "translation units disagree on the definition"

    # Declarations are only a fallback for functions defined outside the
    # project. Two prototyped declarations of the same function may differ by
    # parameter name alone, which is no reason to give up on the function --
    # it just means neither is usable as the canonical text.
    fallbacks: dict[FnKey, Signature | None] = {}
    for analysis in analyses:
        for key, signature in analysis.declaration_signatures.items():
            if key in decision.signatures:
                continue
            if key in fallbacks and fallbacks[key] != signature:
                fallbacks[key] = None
            else:
                fallbacks[key] = signature
    for key, fallback in fallbacks.items():
        if fallback is not None:
            decision.signatures[key] = fallback

    for key in decision.unhandled:
        decision.signatures.pop(key, None)

    return decision


def resolve_site_replacements(
    analysis: TuAnalysis,
    decision: ProjectDecision,
    summary: KnrPassSummary,
) -> list[RewriteSite]:
    """Fill in each site's replacement text, dropping the ones we cannot settle."""
    resolved: list[RewriteSite] = []
    for site in analysis.sites:
        if site.key in decision.unhandled:
            summary.note_skip(decision.unhandled[site.key])
            continue

        if site.kind in (SiteKind.OLD_STYLE_DEFN, SiteKind.IMPLICIT_INT_RESULT):
            assert site.replacement is not None
            resolved.append(site)
            continue

        if site.kind == SiteKind.NOPROTO_DEFN:
            if site.key in decision.called_with_args:
                # Passing arguments to a function defined with an empty
                # parameter list is already undefined; leave the pair alone
                # rather than turning it into a compile error.
                summary.note_skip("unprototyped definition is called with arguments")
                continue
            site.replacement = "(void)"
            resolved.append(site)
            continue

        assert site.kind == SiteKind.NOPROTO_DECL
        signature = decision.signatures.get(site.key)
        if signature is not None:
            site.replacement = signature.render()
            resolved.append(site)
            continue
        if site.key not in decision.referenced:
            site.replacement = "(void)"
            resolved.append(site)
            continue
        # The function is used but has no definition or prototype anywhere in
        # the project. Its parameter types could be guessed from the call sites,
        # but a wrong guess is a silent ABI error that still compiles, so this
        # is left for a human (or for category E, which needs the same analysis).
        summary.note_skip("unprototyped declaration of an external function")

    return resolved


def _diagnostic_regression(before: list[str], after: list[str]) -> str | None:
    """Describe the first sign that a rewrite broke a translation unit."""
    before_counts: dict[str, int] = {}
    for spelling in before:
        before_counts[spelling] = before_counts.get(spelling, 0) + 1
    for spelling in after:
        if before_counts.get(spelling, 0) > 0:
            before_counts[spelling] -= 1
            continue
        lowered = spelling.lower()
        if any(substring in lowered for substring in RISKY_DIAGNOSTIC_SUBSTRINGS):
            return spelling
    return None


def _diagnostics(tu: TranslationUnit) -> tuple[int, list[str]]:
    """Error count and the spellings of all warnings-and-worse."""
    errors = 0
    spellings = []
    for diagnostic in tu.diagnostics:
        if diagnostic.severity >= 3:  # Error or Fatal
            errors += 1
        if diagnostic.severity >= 2:  # Warning or worse
            spellings.append(diagnostic.spelling)
    return errors, spellings


type ReparseFn = Callable[[], TranslationUnit]


def eliminate_knr_syntax(
    commands: list[compilation_database.CompileCommand],
) -> KnrPassSummary:
    """Rewrite K&R syntax out of each of `commands`' translation units, in place.

    Every command must name a preprocessed (`.i`) file; see `prep_eliminate_knr`
    for why the pass is restricted to those.
    """
    index = cindex_helpers.create_xj_clang_index()

    units: list[tuple[FilePathStr, ReparseFn]] = []
    for cmd in commands:

        def reparse(cmd=cmd) -> TranslationUnit:
            return c_refact.parse_translation_unit_with_args(
                index,
                cmd.absolute_file_path.as_posix(),
                cmd.get_command_parts()[1:],  # Skip compiler executable
                in_dir=cmd.directory_path.as_posix(),
            )

        units.append((cmd.absolute_file_path.as_posix(), reparse))

    return eliminate_knr_syntax_in_units(units)


def eliminate_knr_syntax_in_files(
    paths: list[Path], args: list[str] | None = None
) -> KnrPassSummary:
    """`eliminate_knr_syntax` for plain files, without a compilation database."""
    index = cindex_helpers.create_xj_clang_index()
    parse_args = args if args is not None else ["-std=c11"]

    units: list[tuple[FilePathStr, ReparseFn]] = []
    for path in paths:

        def reparse(path=path) -> TranslationUnit:
            return c_refact.parse_translation_unit_with_args(
                index, path.as_posix(), [*parse_args, path.as_posix()]
            )

        units.append((path.as_posix(), reparse))

    return eliminate_knr_syntax_in_units(units)


def eliminate_knr_syntax_in_units(units: list[tuple[FilePathStr, ReparseFn]]) -> KnrPassSummary:
    """Analyze every translation unit, agree on signatures, then rewrite each one.

    The two phases cannot be fused: an unprototyped declaration in one unit is
    resolved from a definition that may live in another, and every unit must
    settle on the same answer.
    """
    summary = KnrPassSummary()

    parsed: list[tuple[FilePathStr, ReparseFn, TranslationUnit]] = []
    analyses: list[TuAnalysis] = []
    for tu_path, reparse in units:
        try:
            tu = reparse()
        except Exception as e:
            print(f"TENJIN: WARNING: K&R elimination could not parse {tu_path}: {e}")
            continue
        parsed.append((tu_path, reparse, tu))
        analyses.append(analyze_translation_unit(tu_path, tu, Path(tu_path).read_bytes()))

    decision = decide_signatures(analyses)

    for (tu_path, reparse, tu), analysis in zip(parsed, analyses):
        summary.noproto_type_mentions += analysis.noproto_type_mentions
        sites = resolve_site_replacements(analysis, decision, summary)
        if not sites:
            continue

        baseline_errors, baseline_spellings = _diagnostics(tu)

        rewriter = batching_rewriter.BatchingRewriter()
        for site in sites:
            assert site.replacement is not None
            rewriter.add_rewrite(tu_path, site.start, site.end - site.start, site.replacement)
            if site.body_insert_offset is not None and site.body_insert_text:
                rewriter.add_rewrite(tu_path, site.body_insert_offset, 0, site.body_insert_text)
        snapshot = rewriter.capture_snapshot()
        try:
            rewriter.apply_rewrites()
        except ValueError as e:
            print(f"TENJIN: WARNING: K&R rewrites conflicted in {tu_path}: {e}")
            rewriter.restore_snapshot(snapshot)
            summary.rolled_back_tus.append(tu_path)
            continue

        regression = _verify(reparse, baseline_errors, baseline_spellings)
        if regression is not None:
            print(f"TENJIN: WARNING: K&R rewrites regressed {tu_path}: {regression}")
            rewriter.restore_snapshot(snapshot)
            summary.rolled_back_tus.append(tu_path)
            continue

        for site in sites:
            summary.note_rewrite(site.kind)

    print(summary.describe())
    return summary


def _verify(
    reparse: ReparseFn,
    baseline_errors: int,
    baseline_spellings: list[str],
) -> str | None:
    """Re-parse a rewritten TU; return a description of any regression."""
    try:
        tu = reparse()
    except Exception as e:
        return f"reparse failed: {e}"
    errors, spellings = _diagnostics(tu)
    if errors > baseline_errors:
        for diagnostic in tu.diagnostics:
            if diagnostic.severity >= 3:
                return f"new error: {diagnostic.spelling}"
        return f"error count rose from {baseline_errors} to {errors}"
    return _diagnostic_regression(baseline_spellings, spellings)
