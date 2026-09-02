from pathlib import Path

from provisioning import cook_pkg_config_sysroot_prefixes_within


def test_cook_pkg_config_sysroot_prefixes_within(tmp_path: Path):
    sysroot = tmp_path / "llvm" / "sysroot"
    pkgconfig = sysroot / "usr" / "lib" / "pkgconfig"
    pkgconfig.mkdir(parents=True)

    affected = pkgconfig / "affected.pc"
    affected.write_text(
        "prefix=/usr\nexec_prefix=${prefix}\nlibdir=${prefix}/lib\n",
        encoding="utf-8",
    )
    unaffected = pkgconfig / "unaffected.pc"
    unaffected_data = "prefix=/usr/local\n# prefix=/usr\n"
    unaffected.write_text(unaffected_data, encoding="utf-8")

    cook_pkg_config_sysroot_prefixes_within(sysroot)

    assert affected.read_text(encoding="utf-8") == (
        f"prefix={(sysroot / 'usr').resolve().as_posix()}\n"
        "exec_prefix=${prefix}\n"
        "libdir=${prefix}/lib\n"
    )
    assert unaffected.read_text(encoding="utf-8") == unaffected_data
