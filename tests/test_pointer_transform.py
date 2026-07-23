import json
from pathlib import Path

import pytest

import hermetic
from ptr_slice_fixture_harness import run_case

_CASES_DIR = Path(__file__).parent / "pointer_transform_cases"
_CASES = sorted(p.name for p in _CASES_DIR.iterdir() if p.is_dir())


@pytest.mark.parametrize("case", _CASES)
def test_pointer_transform_case(root, test_tmp_dir, case):
    run_case(root, test_tmp_dir, _CASES_DIR / case)


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

    pointer_transform = root / "_local" / "_build_pointertransform" / "xj-prepare-pointertransform"
    result = hermetic.run(
        [pointer_transform, "-p", tmp_codebase, source],
        check=True,
        capture_output=True,
    )
    transformed = result.stdout.decode("utf-8")

    assert transformed.count("static int find_item(int index)") == 2
    assert "intfind_item" not in transformed
    assert "find_item(index) != -1" in transformed
    assert "return use_item(&items[item]);" in transformed

    hermetic.run(
        [clang, "-std=c11", "-x", "c", "-fsyntax-only", "-"],
        input=result.stdout,
        check=True,
        capture_output=True,
    )
