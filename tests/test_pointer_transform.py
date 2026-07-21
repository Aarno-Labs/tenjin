import json

import hermetic


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
