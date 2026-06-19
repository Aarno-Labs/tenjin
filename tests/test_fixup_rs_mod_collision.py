from pathlib import Path

from fixup_rs_mod_collision import CrateRootNotFound, inject_tu_includes


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def test_injects_sibling_tu_as_first_inline_module_item(tmp_path: Path):
    write(
        tmp_path / "lib.rs",
        """pub mod src {
    pub mod src {
        pub mod util {
            pub mod hash {
                pub mod builtin;
            }
        }
    }
}
""",
    )
    write(tmp_path / "src" / "src" / "util" / "hash.rs", "pub fn loaded() {}\n")

    first = inject_tu_includes(tmp_path)
    second = inject_tu_includes(tmp_path)

    assert [(i.module_path, i.include_path, i.applied) for i in first] == [
        ("src::src::util::hash", "src/src/util/hash.rs", True)
    ]
    assert second == []
    assert 'include!("src/src/util/hash.rs");' in (tmp_path / "lib.rs").read_text(encoding="utf-8")


def test_ignores_bare_mod_and_inline_modules_without_sibling_file(tmp_path: Path):
    write(
        tmp_path / "lib.rs",
        """pub mod top {
    pub mod bare;
}
pub mod grouped {
    pub mod child;
}
""",
    )
    write(tmp_path / "top" / "bare.rs", "pub fn already_loaded() {}\n")

    assert inject_tu_includes(tmp_path) == []


def test_dry_run_does_not_write(tmp_path: Path):
    original = "pub mod src {}\n"
    write(tmp_path / "lib.rs", original)
    write(tmp_path / "src.rs", "pub fn loaded() {}\n")

    planned = inject_tu_includes(tmp_path, dry_run=True)

    assert [(i.module_path, i.include_path, i.applied) for i in planned] == [
        ("src", "src.rs", False)
    ]
    assert (tmp_path / "lib.rs").read_text(encoding="utf-8") == original


def test_skips_path_attribute_subtree(tmp_path: Path):
    write(
        tmp_path / "lib.rs",
        """#[path = "elsewhere.rs"]
pub mod src {
    pub mod nested {}
}
""",
    )
    write(tmp_path / "src.rs", "pub fn loaded() {}\n")
    write(tmp_path / "src" / "nested.rs", "pub fn nested_loaded() {}\n")

    assert inject_tu_includes(tmp_path) == []


def test_missing_crate_root_raises(tmp_path: Path):
    try:
        inject_tu_includes(tmp_path)
    except CrateRootNotFound:
        pass
    else:
        raise AssertionError("expected CrateRootNotFound")
