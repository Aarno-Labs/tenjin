# Note: the keys in this dict are not command names, or file names,
# just arbitrary labels for the things we are tracking.
WANT = {
    "10j-llvm": "21.1.8+refold@rev-ad108ddde",
    "10j-llvm14": "14.0.6@llvmorg-14.0.6",
    "10j-opam": "2.3.0",
    "10j-dune": "3.19.1",
    "10j-ocaml": "5.2.0",
    "10j-cmake": "3.31.7",
    "10j-cargo-nextest": "0.9.136",
    "10j-bullseye-sysroot-extras": "rev-b578a0937",
    "10j-build-deps": "rev-da7a64964",
    # Note that 10j-more-deps builds against a specific version of LLVM, so before
    # upgrading the major version of 10j-llvm, update 10j-more-deps first.
    "10j-more-deps": "rev-5a807b90f",
    "10j-codehawk": "7e01d90290c01f40e572ffee1f79a9d663f3ab7a",
    "10j-codehawk-c": "76556e4d12a6f01f6da2bbf9a8c6f02624e70f04",
    # Keep in sync with the version in `xj-improve-multitool/rust-toolchain.toml`.
    "10j-xj-improve-multitool-toolchain": "nightly-2026-05-01",
    "10j-xj-default-rust-toolchain": "1.88.0",
    # Keep in sync with the version in `c2rust/c2rust-rust-tools/src/lib.rs`.
    "10j-edition2021-rust-toolchain": "nightly-2025-05-18",
    # Keep in sync with the version in `c2rust/c2rust-rust-tools/src/lib.rs`.
    "10j-edition2024-rust-toolchain": "nightly-2026-03-03",
    "10j-reference-c2rust-tag": "3558805d0c94ce42b3efbce59e8cef784dcc78f4",
    "10j-ast-grep": "0.40.5",
    "10j-crat": "crat-f598249c3",
}

XJ_GUIDANCE_FILENAME = "xj-guidance.json"

# Subdirectory of hermetic.xj_llvm_root()
SYSROOT_NAME = "sysroot"

if __name__ == "__main__":
    # This is a separate script from provisioning.py so that it can be run
    # with a stock Python interpreter, without any third-party modules.
    import sys
    import platform

    def piece(k):
        return f"{k}-{WANT['10j-' + k]}"

    match sys.argv:
        case [_, "ocaml-cache-key"]:
            ocamlparts = ";".join(piece(k) for k in "ocaml opam dune".split())
            print(";".join([platform.system(), platform.machine(), ocamlparts]))
        case [_, "codehawk-cache-key"]:
            codehawkparts = piece("codehawk")
            print(";".join([platform.system(), platform.machine(), codehawkparts]))
        case [_, "upstream-c2rust-cache-key"]:
            upstream_c2rust_tag = piece("reference-c2rust-tag")
            print(";".join([platform.system(), platform.machine(), upstream_c2rust_tag]))
        case _:
            pass
