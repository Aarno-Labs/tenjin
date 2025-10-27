# Note: the keys in this dict are not command names, or file names,
# just arbitrary labels for the things we are tracking.
WANT = {
    "10j-llvm": "18.1.8+refold@rev-b578a0937",
    "10j-opam": "2.3.0",
    "10j-dune": "3.19.1",
    "10j-ocaml": "5.2.0",
    "10j-cmake": "3.31.7",
    "10j-bullseye-sysroot-extras": "rev-b578a0937",
    "10j-build-deps": "rev-b578a0937",
    # Note that 10j-more-deps builds against a specific version LLVM, so before
    # upgrading the major version of 10j-llvm, update 10j-more-deps first.
    "10j-more-deps": "rev-c7875c5b5",
    "10j-codehawk": "d7904d71d7743369fab9020fcaf3e8b15482152b",
    "10j-codehawk-c": "92b9bbd0220281e2931bd66b83c0f6c8ab911cde",
    # Keep in sync with the version in `xj-improve-multitool/rust-toolchain.toml`.
    "10j-xj-improve-multitool-toolchain": "nightly-2025-03-03",
    "10j-xj-default-rust-toolchain": "1.88.0",
    "10j-reference-c2rust-tag": "55e1c24d5f3cbe0f3ef29717eeed77ea4c51867a",
}

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
