import json
import re
from dataclasses import dataclass
from pathlib import Path

import hermetic


@dataclass
class Instrumented:
    sources: dict[str, str]
    header: str | None
    guidance: dict
    guidance_text: str

    def diagnostic_kinds(self) -> list[str]:
        return [d["kind"] for d in self.guidance.get("guidance_diagnostics", [])]


def clang(root: Path) -> Path:
    return root / "_local" / "xj-llvm" / "bin" / "clang"


def write_compdb(root: Path, codebase: Path, sources: list[Path]) -> None:
    resource_dir = (
        hermetic.run([clang(root), "-print-resource-dir"], check=True, capture_output=True)
        .stdout.decode()
        .strip()
    )
    entries = [
        {
            "directory": codebase.as_posix(),
            "file": source.as_posix(),
            "arguments": [
                clang(root).as_posix(),
                "-std=c11",
                f"-resource-dir={resource_dir}",
                "-c",
                source.as_posix(),
            ],
        }
        for source in sources
    ]
    (codebase / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")


def run_tool(root: Path, codebase: Path, sources: list[Path]) -> None:
    tool = root / "_local" / "_build_guidance" / "xj-prepare-guidance"
    hermetic.run(
        [
            tool,
            "--inplace",
            "-p",
            codebase,
            f"--guidance={codebase / 'xj-guidance.json'}",
            f"--header-out={codebase}",
            *sources,
        ],
        check=True,
        capture_output=True,
    )


def check_syntax(root: Path, codebase: Path, sources: list[Path]) -> None:
    header = codebase / "xj_guidance.h"
    include = ["-include", header.as_posix()] if header.exists() else []
    for source in sources:
        hermetic.run(
            [clang(root), "-std=c11", "-fsyntax-only", "-Wno-everything", *include, source],
            check=True,
            capture_output=True,
        )


def instrument(root: Path, codebase: Path, sources: dict[str, str], guidance: dict) -> Instrumented:
    codebase.mkdir(parents=True, exist_ok=True)
    paths = []
    for name, text in sources.items():
        path = codebase / name
        path.write_text(text, encoding="utf-8")
        paths.append(path)
    guidance_path = codebase / "xj-guidance.json"
    guidance_path.write_text(json.dumps(guidance), encoding="utf-8")
    write_compdb(root, codebase, paths)
    run_tool(root, codebase, paths)
    check_syntax(root, codebase, paths)
    header = codebase / "xj_guidance.h"
    guidance_text = guidance_path.read_text(encoding="utf-8")
    return Instrumented(
        sources={p.name: p.read_text(encoding="utf-8") for p in paths},
        header=header.read_text(encoding="utf-8") if header.exists() else None,
        guidance=json.loads(guidance_text),
        guidance_text=guidance_text,
    )


def typedef_named(result: Instrumented, rust: str) -> str:
    names = [n for n, t in result.guidance["marker_typedefs"].items() if t == rust]
    assert len(names) == 1, f"expected one typedef for {rust}, got {names}"
    return names[0]


def test_empty_guidance_is_a_no_op(root, tmp_codebase):
    source = "int f(const char *s) { return s[0]; }\n"
    result = instrument(root, tmp_codebase, {"a.c": source}, {"public_api": []})

    assert result.sources["a.c"] == source
    assert result.header is None
    assert result.guidance == {"public_api": []}


def test_retypes_every_redeclaration_and_resolves_mutability(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": """\
struct Rec { char *name; int n; };
extern const char *g_msg;
const char *g_msg = 0;
char *make(void);
int takes(const char *s);
char *make(void) { return 0; }
int takes(const char *str) {
    const char *local = 0;
    static int count = 0;
    return count;
}
"""
        },
        {
            "vars_of_type": {"&str": ["takes:str", "g_msg"], "String": "Rec:name"},
            "fn_return_type": {"make": "String"},
            "vars_mut": {"takes:local": False, "*:count": False, "g_msg": False},
        },
    )
    text = result.sources["a.c"]
    view = [n for n, t in result.guidance["marker_typedefs"].items() if t == "&str"]
    owned = [n for n, t in result.guidance["marker_typedefs"].items() if t == "String"]

    assert any(f"int takes({n} s);" in text for n in view)
    assert any(f"int takes({n} str) {{" in text for n in view)
    assert any(f"extern {n} g_msg;" in text for n in view)
    assert any(f"{n} name;" in text for n in owned)
    assert sum(text.count(f"{n} make(void)") for n in owned) == 2
    assert "const char *local = 0;" in text
    assert result.guidance["vars_mut_resolved"] == {
        "takes:local": False,
        "takes:count": False,
        "g_msg": False,
    }


def test_record_typedefs_are_placed_in_the_translation_unit(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {"a.c": "struct Node { int v; };\nint get(struct Node *n) { return n->v; }\n"},
        {"vars_of_type": {"&Node": "get:n"}},
    )
    name = typedef_named(result, "&Node")

    assert result.header is None or "struct Node" not in result.header
    assert f"typedef struct Node *{name};\nint get({name} n)" in result.sources["a.c"]


def test_nested_buffers_get_element_typedefs(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {"a.c": "int at(int **rows, int i, int j) { return rows[i][j]; }\n"},
        {"vars_of_type": {"Vec<Vec<i32>>": "at:rows"}},
    )
    outer = typedef_named(result, "Vec<Vec<i32>>")
    inner = typedef_named(result, "Vec<i32>")

    assert f"typedef int *{inner};" in result.header
    assert f"typedef {inner} *{outer};" in result.header
    assert result.header.index(f"*{inner};") < result.header.index(f"*{outer};")
    assert f"int at({outer} rows, int i, int j)" in result.sources["a.c"]
    assert re.search(
        r"return \(\*xj_index_\d+\(\(\*xj_index_\d+\(rows, i\)\), j\)\);", result.sources["a.c"]
    )
    assert re.search(rf"static inline {inner} \*xj_index_\d+\({outer} b, long i\)", result.header)


def test_arrays_sized_by_their_initializer_keep_the_completed_type(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {"a.c": 'void f(void) { char s[] = "abc"; }\n'},
        {"vars_of_type": {"String": "f:s"}},
    )
    name = typedef_named(result, "String")

    assert f"typedef char {name}[4];" in result.header
    assert f'{name} s = "abc";' in result.sources["a.c"]


def test_pointer_operators_on_guided_operands_are_normalized(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": """\
int first(const unsigned char *p) { return *p; }
int at(const unsigned char *p, int i) { return *(p + i) + *(i + p) + p[i]; }
unsigned long size(const unsigned char *p) { return sizeof *p + sizeof p[1]; }
int deref(int *q) { return q[0]; }
int *same(int *q) { return &*q; }
int array(int i) { unsigned char a[4] = {0}; return *a + a[i]; }
"""
        },
        {
            "vars_of_type": {
                "&[u8]": ["first:p", "at:p", "size:p"],
                "&i32": ["deref:q", "same:q"],
                "[u8; 4]": "array:a",
            }
        },
    )
    text = result.sources["a.c"]
    index = r"\(\*xj_index_\d+\(p, (\w+)\)\)"

    assert re.search(rf"return {index};", text)
    assert re.search(rf"return {index} \+ {index} \+ {index};", text)
    assert "return sizeof *p + sizeof p[1];" in text
    assert "return (*q);" in text
    assert re.search(r"return xj_coerce_\d+\(q\);", text)
    assert "return a[0] + a[i];" in text
    families = {(m["family"], m["from"], m["to"]) for m in result.guidance["markers"].values()}
    assert ("index", "&[u8]", "") in families


def test_pointer_motion_and_bad_offsets_are_reported(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": """\
void walk(const unsigned char *p) { p++; p += 2; }
void retreat(const unsigned char *p) { p--; }
long span(const unsigned char *p, const unsigned char *q) { return q - p; }
int second(int *r) { return r[1]; }
void back(const unsigned char *p) { const unsigned char *b = p - 1; }
"""
        },
        {
            "vars_of_type": {
                "&[u8]": ["walk:p", "retreat:p", "span:p", "span:q", "back:p"],
                "&i32": "second:r",
            }
        },
    )

    text = result.sources["a.c"]
    kinds = result.diagnostic_kinds()
    assert kinds.count("pointer-motion") == 1
    assert "pointer-difference" in kinds
    assert "subscript-of-single-object" in kinds
    assert "backward-offset" in kinds
    assert re.search(r"p = xj_slice_from_\d+\(p, 1\); p = xj_slice_from_\d+\(p, 2\);", text)
    assert "p--;" in text
    assert "return r[1];" in text


MATRIX_SOURCE = """\
struct S { unsigned char buf[8]; };
void takes_slice(const unsigned char *s);
void takes_elem(const unsigned char *e);
void takes_raw(const unsigned char *r);
void matrix(const unsigned char *gp, int i, struct S *st) {
    unsigned char arr[8] = {0};
    unsigned char garr[8] = {0};
    takes_slice(arr); takes_slice(&arr[i]); takes_slice(arr + i);
    takes_elem(arr); takes_elem(&arr[i]); takes_elem(arr + i);
    takes_raw(arr); takes_raw(&arr[i]); takes_raw(arr + i);
    takes_slice(garr); takes_slice(&garr[i]); takes_slice(garr + i);
    takes_elem(garr); takes_elem(&garr[i]); takes_elem(garr + i);
    takes_raw(garr); takes_raw(&garr[i]); takes_raw(garr + i);
    takes_slice(gp); takes_slice(&gp[i]); takes_slice(gp + i);
    takes_elem(gp); takes_elem(&gp[i]); takes_elem(gp + i);
    takes_raw(gp); takes_raw(&gp[i]); takes_raw(gp + i);
    takes_slice(st->buf); takes_slice(&st->buf[i]); takes_slice(st->buf + i);
    takes_elem(st->buf); takes_elem(&st->buf[i]); takes_elem(st->buf + i);
    takes_raw(st->buf); takes_raw(&st->buf[i]); takes_raw(st->buf + i);
}
"""


def test_place_markers_follow_source_shape_and_sink_demand(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {"a.c": MATRIX_SOURCE},
        {
            "vars_of_type": {
                "&[u8]": ["takes_slice:s", "matrix:gp"],
                "&u8": "takes_elem:e",
                "[u8; 8]": "matrix:garr",
                "Vec<u8>": "S:buf",
            }
        },
    )
    text = result.sources["a.c"]

    def expect(sink: str, marker: str, *args: str) -> None:
        call = rf"{sink}\({marker}_\d+\({re.escape(', '.join(args))}\)\);"
        assert re.search(call, text), f"missing {sink}({marker}({', '.join(args)}))"

    for base in ("arr", "garr", "gp", "st->buf"):
        expect("takes_slice", "xj_slice_from", base, "i")
        expect("takes_elem", "xj_elem_ref", base, "0")
        expect("takes_elem", "xj_elem_ref", base, "i")
    for base in ("arr", "garr", "st->buf"):
        expect("takes_slice", "xj_slice_all", base)
    for base in ("garr", "gp", "st->buf"):
        expect("takes_raw", "xj_slice_all", base)
        expect("takes_raw", "xj_elem_ref", base, "i")
        expect("takes_raw", "xj_slice_from", base, "i")

    # A guided slice into a slice sink is already the value; an unguided
    # array into a raw pointer is plain C decay.
    assert "takes_slice(gp);" in text
    assert "takes_raw(arr); takes_raw(&arr[i]); takes_raw(arr + i);" in text


def test_unguided_pointer_into_a_slice_is_reported(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": """\
void takes_slice(const unsigned char *s);
void pass(const unsigned char *raw) { takes_slice(raw); }
"""
        },
        {"vars_of_type": {"&[u8]": "takes_slice:s"}},
    )

    assert re.search(r"takes_slice\(xj_coerce_\d+\(raw\)\);", result.sources["a.c"])
    assert "unhandled-coercion" in result.diagnostic_kinds()


def test_representation_changes_are_wrapped_in_coercion_markers(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": """\
void show(const char *msg);
void f(void) { show("hi"); show(0); }
int initial(void) { char c = 65; return c; }
void view(const char *owned) { const char *borrowed = owned; }
"""
        },
        {
            "vars_of_type": {
                "String": ["show:msg", "view:owned"],
                "char": "initial:c",
                "&str": "view:borrowed",
            }
        },
    )
    text = result.sources["a.c"]

    assert re.search(r'show\(xj_coerce_\d+\("hi"\)\);', text)
    assert re.search(r"show\(xj_coerce_\d+\(0\)\);", text)
    assert re.search(r"xj_ty_\d+ c = xj_coerce_\d+\(65\);", text)
    assert re.search(r"return xj_coerce_\d+\(c\);", text)
    assert re.search(r"xj_ty_\d+ borrowed = xj_coerce_\d+\(owned\);", text)
    pairs = {(m["from"], m["to"]) for m in result.guidance["markers"].values()}
    assert ("String", "&str") in pairs
    assert ("", "String") in pairs
    assert ("char", "") in pairs


def test_equal_guidance_on_different_c_types_is_an_identity_coercion(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": """\
void takes(const unsigned char *s);
void owns(unsigned char *v);
void same(const unsigned char *s2);
void pass(unsigned char *p, const unsigned char *q) {
    unsigned char buf[8] = {0};
    takes(p);
    owns(buf);
    same(q);
}
"""
        },
        {
            "vars_of_type": {
                "&[u8]": ["takes:s", "pass:p", "same:s2", "pass:q"],
                "Vec<u8>": ["owns:v", "pass:buf"],
            }
        },
    )
    text = result.sources["a.c"]

    assert re.search(r"takes\(xj_coerce_\d+\(p\)\);", text)
    assert re.search(r"owns\(xj_coerce_\d+\(buf\)\);", text)
    assert "same(q);" in text
    pairs = {(m["from"], m["to"]) for m in result.guidance["markers"].values()}
    assert ("&[u8]", "&[u8]") in pairs
    assert ("Vec<u8>", "Vec<u8>") in pairs


def test_null_tests_of_guided_values_use_is_null_markers(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": """\
int test(const char *s) {
    if (s) return 1;
    if (!s) return 2;
    return s == 0 ? 3 : (s != (void *)0);
}
"""
        },
        {"vars_of_type": {"String": "test:s"}},
    )
    text = result.sources["a.c"]

    assert re.search(r"if \(\(!xj_is_null_\d+\(s\)\)\) return 1;", text)
    assert re.search(r"if \(xj_is_null_\d+\(s\)\) return 2;", text)
    assert re.search(r"return xj_is_null_\d+\(s\) \? 3 : \(\(!xj_is_null_\d+\(s\)\)\);", text)


def test_variadic_arguments_and_constant_initializers_are_not_wrapped(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": """\
int printf(const char *fmt, ...);
static const char *greeting = "hello";
void say(const char *s) { printf("%s\\n", s); }
"""
        },
        {"vars_of_type": {"String": ["say:s", "greeting"]}},
    )
    text = result.sources["a.c"]

    assert 'printf("%s\\n", s);' in text
    assert re.search(r'static xj_ty_\d+ greeting = "hello";', text)


def test_unmatched_specifiers_are_reported(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {"a.c": "int f(int x) { return x; }\n"},
        {"vars_of_type": {"u8": ["f:x", "g:nothing"]}},
    )

    unmatched = [
        d["subject"]
        for d in result.guidance["guidance_diagnostics"]
        if d["kind"] == "unmatched-specifier"
    ]
    assert unmatched == ["g:nothing"]


def test_a_second_run_changes_nothing(root, tmp_codebase):
    result = instrument(
        root,
        tmp_codebase,
        {"a.c": "int f(const char *s) { return *s; }\n"},
        {"vars_of_type": {"&[u8]": "f:s"}},
    )
    source = tmp_codebase / "a.c"
    run_tool(root, tmp_codebase, [source])

    assert source.read_text(encoding="utf-8") == result.sources["a.c"]
    assert (tmp_codebase / "xj-guidance.json").read_text(encoding="utf-8") == result.guidance_text


def test_names_agree_across_translation_units(root, tmp_codebase):
    shared = "int takes(const char *s);\nstruct Node { int v; };\nint get(struct Node *n);\n"
    result = instrument(
        root,
        tmp_codebase,
        {
            "a.c": shared + 'int a(void) { return takes("a"); }\n',
            "b.c": shared + 'int b(struct Node *m) { return get(m) + takes("b"); }\n',
        },
        {"vars_of_type": {"&str": "*:s", "&Node": "get:n"}},
    )

    def shared_lines(text: str) -> list[str]:
        return [line for line in text.splitlines() if line.startswith(("int takes(", "int get("))]

    assert shared_lines(result.sources["a.c"]) == shared_lines(result.sources["b.c"])
    assert len(shared_lines(result.sources["a.c"])) == 2


def run_pointer_passes(root: Path, codebase: Path, source: Path) -> None:
    local = root / "_local"
    metadata = codebase / "metadata.json"
    tools = [
        (
            local / "_build_pointertransform" / "xj-prepare-pointertransform",
            [f"--metadata-out={metadata}"],
        ),
        (
            local / "_build_baserewrite" / "xj-prepare-baserewrite",
            [f"--metadata-in={metadata}", f"--metadata-out={metadata}"],
        ),
        (
            local / "_build_slicetransform" / "xj-prepare-slicetransform",
            [f"--metadata-in={metadata}"],
        ),
    ]
    for tool, extra in tools:
        hermetic.run(
            [tool, "--inplace", *extra, "-p", codebase, source], check=True, capture_output=True
        )
    metadata.unlink(missing_ok=True)


def test_runs_on_pointer_pass_output_without_motion(root, tmp_codebase):
    tmp_codebase.mkdir()
    source = tmp_codebase / "a.c"
    source.write_text(
        """\
int sum(const int *p, int n) {
    int t = 0;
    for (int i = 0; i < n; i++) {
        t += *p;
        p++;
    }
    return t;
}
""",
        encoding="utf-8",
    )
    write_compdb(root, tmp_codebase, [source])
    run_pointer_passes(root, tmp_codebase, source)
    (tmp_codebase / "xj-guidance.json").write_text(
        json.dumps({"vars_of_type": {"&[i32]": "sum:p"}}), encoding="utf-8"
    )
    run_tool(root, tmp_codebase, [source])
    check_syntax(root, tmp_codebase, [source])
    guidance = json.loads((tmp_codebase / "xj-guidance.json").read_text(encoding="utf-8"))
    text = source.read_text(encoding="utf-8")

    kinds = [d["kind"] for d in guidance["guidance_diagnostics"]]
    assert "pointer-motion" not in kinds
    assert re.search(r"\(\*xj_index_\d+\(p, \w+\)\)", text)
