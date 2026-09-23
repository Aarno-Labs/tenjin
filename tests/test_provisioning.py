from pathlib import Path

import pytest

from provisioning import (
    ProvisioningError,
    cook_pkg_config_sysroot_prefixes_within,
    pangs_release_asset_name,
)


@pytest.mark.parametrize(
    ("system", "machine", "expected"),
    [
        ("Linux", "x86_64", "pangs_linux-x86_64.tar.xz"),
        ("Linux", "aarch64", "pangs_linux-aarch64.tar.xz"),
        ("Darwin", "aarch64", "pangs_macos-aarch64.tar.xz"),
    ],
)
def test_pangs_release_asset_name(system: str, machine: str, expected: str):
    assert pangs_release_asset_name(system, machine) == expected


def test_pangs_release_asset_name_rejects_unsupported_platform():
    with pytest.raises(ProvisioningError, match="unsupported platform"):
        pangs_release_asset_name("Darwin", "x86_64")


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
