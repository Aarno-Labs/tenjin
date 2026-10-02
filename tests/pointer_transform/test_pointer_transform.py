import json
from pathlib import Path

import pytest

import hermetic
from pointer_transform_utils import compile_and_run, run_case

_CASES_DIR = Path(__file__).parent / "pointer_transform_cases"
_CASES = sorted(p.name for p in _CASES_DIR.iterdir() if p.is_dir())


@pytest.mark.parametrize("case", _CASES)
def test_pointer_transform_case(root, test_tmp_dir, case):
    # `root` is requested for its side effect of building the tool.
    run_case(test_tmp_dir, _CASES_DIR / case)


def test_postincrement_arrow_remains_valid_c(root, tmp_codebase):
    tmp_codebase.mkdir()
    source = tmp_codebase / "postincrement_arrow.c"
    source.write_text(
        (Path(__file__).parent / "repros" / source.name).read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    clang = root / "_local" / "xj-llvm" / "bin" / "clang"
    hermetic.run([clang, "-std=c11", "-fsyntax-only", source], check=True, capture_output=True)

    pointer_transform = root / "_local" / "_build_pointertransform" / "xj-prepare-pointertransform"
    result = hermetic.run(
        [pointer_transform, source, "--", "-std=c11"], check=True, capture_output=True
    )
    transformed = tmp_codebase / "transformed.c"
    transformed.write_text(result.stdout.decode("utf-8"), encoding="utf-8")
    hermetic.run([clang, "-std=c11", "-fsyntax-only", transformed], check=True, capture_output=True)


@pytest.mark.parametrize("storage", ["parameter", "local", "global"])
@pytest.mark.parametrize(
    "operation, accessed_index, final_index",
    [("p++", 1, 2), ("++p", 2, 2), ("p--", 1, 0), ("--p", 0, 0)],
)
def test_increment_decrement_arrow_behavior(
    root, tmp_codebase, storage, operation, accessed_index, final_index
):
    tmp_codebase.mkdir()
    source = tmp_codebase / "arrow.c"
    declaration = "Item *p = items + 1;"
    source.write_text(
        "typedef struct { int u; } Item;\n"
        "static Item items[] = {{3}, {5}, {7}};\n"
        + (f"static {declaration}\n" if storage == "global" else "")
        + f"static int update({'Item *p' if storage == 'parameter' else 'void'}) {{\n"
        + (f"    {declaration}\n" if storage == "local" else "")
        + f"    ({operation})->u += 10;\n"
        + "    return p->u;\n"
        + "}\n"
        + "int main(void) {\n"
        + f"    int value = update({'items + 1' if storage == 'parameter' else ''});\n"
        + f"    return value != items[{final_index}].u\n"
        + "        || "
        + " || ".join(
            f"items[{i}].u != {value + (10 if i == accessed_index else 0)}"
            for i, value in enumerate([3, 5, 7])
        )
        + ";\n}\n",
        encoding="utf-8",
    )
    baseline = compile_and_run(tmp_codebase, [source], "original")
    assert baseline == (0, "")

    pointer_transform = root / "_local" / "_build_pointertransform" / "xj-prepare-pointertransform"
    result = hermetic.run(
        [pointer_transform, source, "--", "-std=c11"], check=True, capture_output=True
    )
    assert "[REPLACED]" in result.stderr.decode("utf-8")
    transformed = tmp_codebase / "transformed.c"
    transformed.write_text(result.stdout.decode("utf-8"), encoding="utf-8")
    assert compile_and_run(tmp_codebase, [transformed], "transformed") == baseline


def test_rewritten_pointer_return_type_is_separated_from_function_name(root, tmp_codebase):
    tmp_codebase.mkdir()
    source = tmp_codebase / "return_global.c"
    source.write_text(
        "typedef struct { int value; } Item;\n"
        "static Item items[1];\n"
        "static Item* find_item(int index);\n"
        "static Item *find_item(int index) {\n"
        "    if (index == 0) return &items[index];\n"
        "    return (void *)0;\n"
        "}\n"
        "static int use_item(Item *item) { return item->value; }\n"
        "static int item_exists(int index) {\n"
        "    return find_item(index) != (void *)0;\n"
        "}\n"
        "static int get_item_value(int index) {\n"
        "    Item *item = find_item(index);\n"
        "    if (item) return use_item(item);\n"
        "    return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    clang = root / "_local" / "xj-llvm" / "bin" / "clang"
    (tmp_codebase / "compile_commands.json").write_text(
        json.dumps([
            {
                "directory": tmp_codebase.as_posix(),
                "file": source.as_posix(),
                "arguments": [clang.as_posix(), "-std=c11", "-c", source.as_posix()],
            }
        ]),
        encoding="utf-8",
    )

    # The T*-to-int rewrite under test is performed by the slice tool,
    # so run the full pointer -> slice pipeline (in-place: the slice
    # tool re-parses the pointer tool's output from disk).
    pointer_transform = root / "_local" / "_build_pointertransform" / "xj-prepare-pointertransform"
    slice_transform = root / "_local" / "_build_slicetransform" / "xj-prepare-slicetransform"
    metadata = tmp_codebase / "metadata.json"
    hermetic.run(
        [pointer_transform, "--inplace", f"--metadata-out={metadata}", "-p", tmp_codebase, source],
        check=True,
        capture_output=True,
    )
    hermetic.run(
        [slice_transform, "--inplace", f"--metadata-in={metadata}", "-p", tmp_codebase, source],
        check=True,
        capture_output=True,
    )
    transformed = source.read_text(encoding="utf-8")

    assert transformed.count("static int find_item(int index)") == 2
    assert "intfind_item" not in transformed
    assert "find_item(index) != -1" in transformed
    assert "return use_item(&items[item]);" in transformed

    hermetic.run(
        [clang, "-std=c11", "-fsyntax-only", source],
        check=True,
        capture_output=True,
    )
