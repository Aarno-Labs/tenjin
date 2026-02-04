# Note: the keys in this dict are not command names, or file names,
# just arbitrary labels for the things we are tracking.
WANT = {
    "10j-llvm": "18.1.8+refold@rev-be692ea09",
    "10j-llvm14": "14.0.6@llvmorg-14.0.6",
    "10j-opam": "2.3.0",
    "10j-dune": "3.19.1",
    "10j-ocaml": "5.2.0",
    "10j-cmake": "3.31.7",
    "10j-bullseye-sysroot-extras": "rev-b578a0937",
    "10j-build-deps": "rev-d915905d0",
    # Note that 10j-more-deps builds against a specific version of LLVM, so before
    # upgrading the major version of 10j-llvm, update 10j-more-deps first.
    "10j-more-deps": "rev-d915905d0",
    "10j-codehawk": "5414e06ea6461c759fe565db14e5c8ff64047725",
    "10j-codehawk-c": "db010d1bab5381f9986dbf269ead9c036b55df07",
    # Keep in sync with the version in `xj-improve-multitool/rust-toolchain.toml`.
    "10j-xj-improve-multitool-toolchain": "nightly-2025-08-20",
    "10j-xj-default-rust-toolchain": "1.88.0",
    "10j-reference-c2rust-tag": "613d3f5a7ef568aecb1681ef5960f956c1ed7344",
    "10j-ast-grep": "0.40.5",
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
