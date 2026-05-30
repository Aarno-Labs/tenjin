import targets
import targets_from_intercept


def test_build_info_surfaces_compile_only_object_targets(tmp_path):
    build_info = targets.BuildInfo()
    builddir = tmp_path / "build"
    builddir.mkdir()
    source = tmp_path / "uses.c"
    source.write_text("int use(void) { return 0; }\n", encoding="utf-8")

    build_info.set_intercepted_commands([
        targets_from_intercept.convert_intercepted_entry({
            "type": "cc",
            "directory": builddir.as_posix(),
            "arguments": [
                "/home/brk/tenjin/_local/xj-llvm/bin/clang",
                "-c",
                "-o",
                "uses.o",
                source.as_posix(),
            ],
            "file": None,
            "output": None,
        })
    ])

    assert build_info.get_all_targets() == [
        targets.BuildTarget(
            key="uses.o",
            type=targets.TargetType.OBJECT,
            stem_not_unique="uses",
        )
    ]


def test_build_info_does_not_surface_intermediate_objects_when_link_target_exists(tmp_path):
    build_info = targets.BuildInfo()
    builddir = tmp_path / "build"
    builddir.mkdir()
    source = tmp_path / "main.c"
    source.write_text("int main(void) { return 0; }\n", encoding="utf-8")

    build_info.set_intercepted_commands([
        targets_from_intercept.convert_intercepted_entry({
            "type": "cc",
            "directory": builddir.as_posix(),
            "arguments": [
                "/home/brk/tenjin/_local/xj-llvm/bin/clang",
                "-c",
                "-o",
                "main.o",
                source.as_posix(),
            ],
            "file": None,
            "output": None,
        }),
        targets_from_intercept.convert_intercepted_entry({
            "type": "cc",
            "directory": builddir.as_posix(),
            "arguments": [
                "/home/brk/tenjin/_local/xj-llvm/bin/clang",
                "-o",
                "main.exe",
                "main.o",
            ],
            "file": None,
            "output": None,
        }),
    ])

    assert build_info.get_all_targets() == [
        targets.BuildTarget(
            key="main.exe",
            type=targets.TargetType.EXECUTABLE,
            stem_not_unique="main",
        )
    ]
