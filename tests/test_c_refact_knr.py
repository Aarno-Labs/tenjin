from pathlib import Path

import pytest

import c_refact_knr
from c_refact_knr import SiteKind


def knr_rewrite(tmp_path: Path, source: str, *, name: str = "sample.c") -> str:
    """Run the pass over a single-file project and return the rewritten source."""
    path = tmp_path / name
    path.write_text(source, encoding="utf-8")
    c_refact_knr.eliminate_knr_syntax_in_files([path], ["-std=gnu89"])
    return path.read_text(encoding="utf-8")


def knr_rewrite_project(tmp_path: Path, sources: dict[str, str]) -> dict[str, str]:
    """Run the pass over a multi-file project, as the real pipeline does."""
    paths = []
    for name, source in sources.items():
        path = tmp_path / name
        path.write_text(source, encoding="utf-8")
        paths.append(path)
    c_refact_knr.eliminate_knr_syntax_in_files(paths, ["-std=gnu89"])
    return {p.name: p.read_text(encoding="utf-8") for p in paths}


# --- Category A: old-style definitions -------------------------------------


def test_old_style_definition_becomes_a_prototype(tmp_path):
    result = knr_rewrite(
        tmp_path,
        "int f(x, s)\nint x;\nchar *s;\n{\n    return s[x];\n}\n",
    )
    assert result == "int f(int x, char *s)\n{\n    return s[x];\n}\n"


def test_parameters_follow_identifier_list_order_not_declaration_order(tmp_path):
    result = knr_rewrite(
        tmp_path,
        "int f(a, b)\n    char *b;\n    int a;\n{ return a; }\n",
    )
    assert result.startswith("int f(int a, char *b)\n")


def test_implicitly_typed_parameters_get_an_explicit_int(tmp_path):
    result = knr_rewrite(tmp_path, "int f(a, b)\n{ return a + b; }\n")
    assert result == "int f(int a, int b)\n{ return a + b; }\n"


def test_partially_declared_parameter_list(tmp_path):
    result = knr_rewrite(tmp_path, "int f(a, b)\n    int a;\n{ return a + b; }\n")
    assert result == "int f(int a, int b)\n{ return a + b; }\n"


def test_joined_declarators_are_split_into_separate_parameters(tmp_path):
    # clang reports `b`'s extent as the whole `int a, b`, so the declarator text
    # cannot be reused and the type has to be rendered instead.
    result = knr_rewrite(
        tmp_path,
        "int f(a, b, c)\n    int a, b;\n    char *c;\n{ return a + b + c[0]; }\n",
    )
    assert result.startswith("int f(int a, int b, char *c)\n")


def test_array_parameter_keeps_its_declared_form(tmp_path):
    result = knr_rewrite(
        tmp_path,
        "int f(s, n)\n    char s[];\n    register int n;\n{ return s[n]; }\n",
    )
    assert result.startswith("int f(char s[], register int n)\n")


def test_implicit_int_return_type_is_made_explicit(tmp_path):
    result = knr_rewrite(tmp_path, "static f(a)\n    int a;\n{ return a; }\n")
    assert result == "static int f(int a)\n{ return a; }\n"


def test_attribute_before_the_function_name_is_preserved(tmp_path):
    result = knr_rewrite(
        tmp_path,
        "int __attribute__((noinline)) f(a)\n  int a;\n{ return a; }\n",
    )
    assert result == "int __attribute__((noinline)) f(int a)\n{ return a; }\n"


def test_prototyped_definition_is_left_alone(tmp_path):
    source = "int f(int x, char *s)\n{ return s[x]; }\n"
    assert knr_rewrite(tmp_path, source) == source


def test_main_with_an_identifier_list(tmp_path):
    result = knr_rewrite(
        tmp_path,
        "int main(argc, argv)\nint argc;\nchar **argv;\n{ return argc; }\n",
    )
    assert result == "int main(int argc, char **argv)\n{ return argc; }\n"


# --- Category A, ABI preservation ------------------------------------------


def test_promoted_parameters_keep_the_promoted_signature_by_default(tmp_path):
    # A caller with no visible prototype passes `c` as int and `f` as double.
    # Preserving that signature and converting in the body is exactly what the
    # old-style definition meant, so calls through an unprototyped function
    # pointer keep working.
    result = knr_rewrite(
        tmp_path,
        "int g(c, f)\n  char c;\n  float f;\n{ return c + (int)f; }\n",
    )
    assert result == (
        "int g(int c_xjknr, double f_xjknr)\n"
        "{ char c = c_xjknr; float f = f_xjknr; return c + (int)f; }\n"
    )


def test_non_promoted_parameters_need_no_shadow_copy(tmp_path):
    result = knr_rewrite(
        tmp_path,
        "int g(p, n)\n  long n;\n  double *p;\n{ return (int)(*p + n); }\n",
    )
    assert result == "int g(double *p, long n)\n{ return (int)(*p + n); }\n"


def test_shadow_parameter_name_avoids_a_collision(tmp_path):
    result = knr_rewrite(
        tmp_path,
        "int g(c)\n  char c;\n{ int c_xjknr = 1; return c + c_xjknr; }\n",
    )
    assert "int c_xjknr0" in result
    assert result.startswith("int g(int c_xjknr0)\n{ char c = c_xjknr0;")


# --- Categories B and C: unprototyped declarations and definitions ----------


def test_unprototyped_definition_becomes_void(tmp_path):
    result = knr_rewrite(tmp_path, "int f() { return 7; }\n")
    assert result == "int f(void) { return 7; }\n"


def test_unreferenced_unprototyped_declaration_becomes_void(tmp_path):
    result = knr_rewrite(tmp_path, "int f();\n")
    assert result == "int f(void);\n"


def test_unprototyped_declaration_takes_the_definitions_signature(tmp_path):
    result = knr_rewrite(
        tmp_path,
        "extern int f();\nint f(x, s)\nint x;\nchar *s;\n{ return s[x]; }\n",
    )
    assert result.startswith("extern int f(int x, char *s);\n")


def test_declaration_and_definition_agree_across_translation_units(tmp_path):
    results = knr_rewrite_project(
        tmp_path,
        {
            "defn.c": "int f(x, s)\nint x;\nchar *s;\n{ return s[x]; }\n",
            "use.c": 'extern int f();\nint use(void) { return f(1, "x"); }\n',
        },
    )
    assert results["defn.c"].startswith("int f(int x, char *s)\n")
    assert results["use.c"].startswith("extern int f(int x, char *s);\n")


def test_statics_with_the_same_name_are_kept_apart(tmp_path):
    # `prep_uniquify_statics` has not run yet, so two files can each define a
    # different `static int helper`. Keying on the name alone would make them
    # look like one function with two conflicting signatures.
    results = knr_rewrite_project(
        tmp_path,
        {
            "one.c": "static int helper(a)\n int a;\n{ return a; }\nint u1(void){return helper(1);}\n",
            "two.c": "static int helper(p)\n char *p;\n{ return p[0]; }\nint u2(void){return helper(0);}\n",
        },
    )
    assert results["one.c"].startswith("static int helper(int a)\n")
    assert results["two.c"].startswith("static int helper(char *p)\n")


def test_a_prototyped_declaration_elsewhere_does_not_block_the_definition(tmp_path):
    # The header prototype names its parameters `a`/`b` while the definition
    # calls them `x`/`s`. Only the definition is authoritative, so the differing
    # names must not read as a conflict.
    results = knr_rewrite_project(
        tmp_path,
        {
            "defn.c": "extern int f(int a, char *b);\nint f(x, s)\nint x;\nchar *s;\n{ return s[x]; }\n",
            "other.c": "extern int f(int a, char *b);\nint use(void) { return f(1, 0); }\n",
        },
    )
    assert "int f(int x, char *s)\n" in results["defn.c"]


def test_conflicting_declarations_are_unusable_but_not_fatal(tmp_path):
    # Neither prototyped declaration can serve as the canonical text for `ext`,
    # so the unprototyped declaration is left alone -- but `g` is still fixed.
    results = knr_rewrite_project(
        tmp_path,
        {
            "one.c": "extern int ext(int a);\nint u1(void){return ext(1);}\n",
            "two.c": "extern int ext(int b);\nint u2(void){return ext(2);}\n",
            "three.c": "extern int ext();\nint g(n)\n int n;\n{ return ext(n); }\n",
        },
    )
    assert results["three.c"].startswith("extern int ext();\n")
    assert "int g(int n)\n" in results["three.c"]


def test_external_function_without_a_definition_is_left_alone(tmp_path):
    # Guessing parameter types from call sites would be a silent ABI error when
    # wrong, and it would still compile, so the verify gate could not catch it.
    source = 'extern int ext();\nint use(void) { return ext(1, "x"); }\n'
    assert knr_rewrite(tmp_path, source) == source


def test_unprototyped_definition_called_with_arguments_is_left_alone(tmp_path):
    source = "int f() { return 7; }\nint use(void) { return f(1); }\n"
    assert knr_rewrite(tmp_path, source) == source


# --- Category E: counted, not rewritten ------------------------------------


def test_unprototyped_function_pointer_types_are_counted_but_untouched(tmp_path):
    source = "typedef int knr();\nint (*fp)();\nstruct S { int (*m)(); };\n"
    path = tmp_path / "ptrs.c"
    path.write_text(source, encoding="utf-8")

    summary = c_refact_knr.eliminate_knr_syntax_in_files([path], ["-std=gnu89"])

    assert path.read_text(encoding="utf-8") == source
    assert summary.noproto_type_mentions >= 3
    assert not summary.rewritten


# --- The verify gate --------------------------------------------------------


def test_a_call_that_contradicts_the_new_prototype_rolls_the_tu_back(tmp_path):
    # The call precedes the definition and is checked only against the
    # unprototyped `extern int f()`, so passing an int for `char *s` goes
    # unnoticed. Filling in the prototype makes it a diagnostic, and the whole
    # TU is restored rather than left broken.
    source = (
        "extern int f();\n"
        "int use(void) { return f(1, 2); }\n"
        "int f(x, s)\n"
        "int x;\n"
        "char *s;\n"
        "{ return s[x]; }\n"
    )
    path = tmp_path / "bad.c"
    path.write_text(source, encoding="utf-8")

    summary = c_refact_knr.eliminate_knr_syntax_in_files([path], ["-std=gnu89"])

    assert path.read_text(encoding="utf-8") == source
    assert summary.rolled_back_tus == [path.as_posix()]
    assert not summary.rewritten


def test_a_mismatched_call_after_the_definition_is_already_checked(tmp_path):
    # The same mismatch, but with the definition first: clang was already
    # checking the call against it, so the rewrite changes nothing about what
    # is diagnosed and there is nothing to roll back.
    source = "int f(x, s)\nint x;\nchar *s;\n{ return s[x]; }\nint use(void) { return f(1, 2); }\n"
    path = tmp_path / "already-checked.c"
    path.write_text(source, encoding="utf-8")

    summary = c_refact_knr.eliminate_knr_syntax_in_files([path], ["-std=gnu89"])

    assert path.read_text(encoding="utf-8").startswith("int f(int x, char *s)\n")
    assert not summary.rolled_back_tus


@pytest.mark.parametrize(
    "before,after,expected",
    [
        ([], ["incompatible pointer types passing 'int'"], True),
        (["unused variable 'x'"], ["unused variable 'x'"], False),
        ([], ["unused variable 'x'"], False),
        (
            ["incompatible integer to pointer conversion"],
            ["incompatible integer to pointer conversion"],
            False,
        ),
    ],
)
def test_diagnostic_regression_detection(before, after, expected):
    found = c_refact_knr._diagnostic_regression(before, after)
    assert (found is not None) == expected


# --- Signature rendering ----------------------------------------------------


def test_signature_render():
    assert c_refact_knr.Signature(params=()).render() == "(void)"
    assert c_refact_knr.Signature(params=("int a",)).render() == "(int a)"
    assert (
        c_refact_knr.Signature(params=("const char *fmt",), is_variadic=True).render()
        == "(const char *fmt, ...)"
    )


def test_summary_describes_each_site_kind():
    summary = c_refact_knr.KnrPassSummary()
    summary.note_rewrite(SiteKind.OLD_STYLE_DEFN)
    summary.note_rewrite(SiteKind.OLD_STYLE_DEFN)
    summary.note_skip("unrenderable parameter")

    described = summary.describe()
    assert "rewrote 2 site(s)" in described
    assert "2  old-style definition" in described
    assert "1  unrenderable parameter" in described
