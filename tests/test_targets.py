from pathlib import Path

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
                "clang",
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
                "clang",
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
                "clang",
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


def test_shared_lib_renamed_to_avoid_collision_with_executable(tmp_path):
    # A shared library `libdriver.so` and an executable `driver` both reduce to
    # crate stem `driver`. `drop_lib_prefix` must rename the library so the two
    # don't collapse into one workspace member -- even when the executable
    # output carries a directory prefix (so a bare-stem comparison is required).
    builddir = tmp_path / "build"
    builddir.mkdir()

    build_info = targets.BuildInfo()
    build_info.set_intercepted_commands([
        targets_from_intercept.convert_intercepted_entry({
            "type": "cc",
            "directory": builddir.as_posix(),
            "arguments": ["clang", "-shared", "-o", "../../libdriver.so", "driver.o"],
            "file": None,
            "output": None,
        }),
        targets_from_intercept.convert_intercepted_entry({
            "type": "cc",
            "directory": builddir.as_posix(),
            "arguments": ["clang", "-o", "../../driver", "main.o"],
            "file": None,
            "output": None,
        }),
    ])

    compdb = build_info.compdb_for_all_targets_within(
        builddir, link_cmd_handling=targets.LinkCommandHandling.ADAPT_FOR_C2RUST
    )
    link_outputs = [cmd.output for cmd in compdb.commands if cmd.output]
    crate_stems = [Path(out).stem for out in link_outputs]

    assert "../../xjlib_driver.so" in link_outputs
    assert "../../driver" in link_outputs
    # The disambiguation must survive the crate name being derived from the stem.
    assert len(crate_stems) == len(set(crate_stems)), link_outputs
