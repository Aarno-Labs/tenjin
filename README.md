# Tenjin

A tool for translating C to Rust, building upon [c2rust](https://github.com/immunant/c2rust/).

Status: embryonic

## Installation

First, [install Rust](https://rustup.rs/). On Linux, you'll also need either `curl` or `wget`.
On Mac, we assume you already have XCode tools installed.
Then:

1. Run `sh cli/sh/provision.sh`
\
\
This will locally install compatible copies of the necessary prerequisites:
LLVM/Clang, OCaml (for [CodeHawk](https://github.com/static-analysis-engineering/codehawk)),
`make`, `ninja`, `unzip`, etc. On Linux, it will also download a Debian sysroot
to enable compilation using standard system libraries.

1. Add the `cli` directory to your `$PATH` to make using Tenjin easier.

1. Run `10j check-py` to verify that the Python code in the repository does not
need to be reformatted.

## Next Steps

* [User docs](docs/USE.md)
* [Developer docs](docs/README.md)