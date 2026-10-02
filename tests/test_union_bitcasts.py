from pathlib import Path

import pytest

import compilation_database
import hermetic


def compile_and_run(source: Path) -> bytes:
    executable = source.with_suffix("")
    hermetic.run(["clang", str(source), "-o", str(executable)], check=True, capture_output=True)
    return hermetic.run([str(executable)], check=True, capture_output=True).stdout


def convert_union_bitcasts(root: Path, source: Path) -> None:
    compilation_database.write_synthetic_compile_commands_to(
        source.parent / "compile_commands.json", source, source.parent
    )
    binary = root / "_local/_build_unionbitcasts/xj-prepare-unionbitcasts"
    hermetic.run(
        [str(binary), "--inplace", "-p", str(source.parent), str(source)],
        check=True,
        capture_output=True,
    )


@pytest.mark.parametrize("initializer", ["{.t=RICON, .i=1}", "{{RICON, 1}}"])
def test_union_bitcasts_preserves_bitfields(root, tmp_path, initializer):
    source = tmp_path / "main.c"
    original = (
        "int printf(const char *, ...);\n"
        "typedef unsigned int uint;\n"
        "enum { RICON = 3 };\n"
        "typedef union { struct { unsigned t : 3; signed i : 29; }; uint bits; } Ref;\n"
        "int main(void) {\n"
        f"    static const Ref ONE = {initializer};\n"
        '    printf("%u\\n", ONE.bits);\n'
        "    return 0;\n"
        "}\n"
    )
    source.write_text(original, encoding="utf-8")
    expected = compile_and_run(source)

    convert_union_bitcasts(root, source)

    assert compile_and_run(source) == expected
    assert source.read_text(encoding="utf-8") == original


@pytest.mark.parametrize(
    ("declarations", "initializer"),
    [
        ("typedef union { unsigned t : 3; uint bits; } Ref;\n", "{.t=3}"),
        (
            "struct Packed;\n"
            "struct Packed { unsigned t : 3; signed i : 29; };\n"
            "typedef union { struct Packed value; uint bits; } Ref;\n",
            "{.value={3, 1}}",
        ),
        (
            "struct Packed { unsigned t : 3; signed i : 29; };\n"
            "typedef union { struct { struct Packed value; } outer; uint bits; } Ref;\n",
            "{.outer={.value={3, 1}}}",
        ),
        (
            "typedef union { struct { unsigned t : 3; signed i : 29; } values[1]; "
            "uint bits; } Ref;\n",
            "{.values={{3, 1}}}",
        ),
    ],
    ids=["direct", "named-record", "nested-record", "array"],
)
def test_union_bitcasts_rejects_fields_containing_bitfields(
    root, tmp_path, declarations, initializer
):
    source = tmp_path / "main.c"
    original = (
        "int printf(const char *, ...);\n"
        "typedef unsigned int uint;\n" + declarations + "int main(void) {\n"
        f"    static const Ref ONE = {initializer};\n"
        '    printf("%u\\n", ONE.bits);\n'
        "    return 0;\n"
        "}\n"
    )
    source.write_text(original, encoding="utf-8")
    expected = compile_and_run(source)

    convert_union_bitcasts(root, source)

    assert source.read_text(encoding="utf-8") == original
    assert compile_and_run(source) == expected


@pytest.mark.parametrize(
    "initializer",
    ["{.t=RICON, .i=1}", "{RICON, 1}", "{{.t=RICON, .i=1}}"],
    ids=["elided-designators", "elided-positional", "braced"],
)
def test_union_bitcasts_preserves_aggregate_initializer(root, tmp_path, initializer):
    source = tmp_path / "main.c"
    source.write_text(
        "int printf(const char *, ...);\n"
        "enum { RICON = 3 };\n"
        "int main(void) {\n"
        "    union { struct { unsigned t; int i; }; unsigned long long bits; } ONE = "
        + initializer
        + ";\n"
        + '    printf("%llu\\n", ONE.bits);\n'
        "    return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    expected = compile_and_run(source)

    convert_union_bitcasts(root, source)

    assert "__tenjin_bvm_" in source.read_text(encoding="utf-8")
    assert compile_and_run(source) == expected


@pytest.mark.parametrize("initializer", ["{3, 1}", "{{3, 1}}"], ids=["elided", "braced"])
@pytest.mark.parametrize("extra_write", [False, True], ids=["init-only", "later-write"])
def test_union_bitcasts_preserves_array_initializer(root, tmp_path, initializer, extra_write):
    source = tmp_path / "main.c"
    source.write_text(
        "int printf(const char *, ...);\n"
        "int main(void) {\n"
        "    union { unsigned values[2]; unsigned long long bits; } ONE = "
        + initializer
        + ";\n"
        + ("    ONE.values[1] = 2;\n" if extra_write else "")
        + '    printf("%llu\\n", ONE.bits);\n'
        "    return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    expected = compile_and_run(source)

    convert_union_bitcasts(root, source)

    assert "__tenjin_bvm_" in source.read_text(encoding="utf-8")
    assert compile_and_run(source) == expected
