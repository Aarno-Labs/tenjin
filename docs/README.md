# Tenjin Developer Docs

## [Git Things](https://matklad.github.io/2023/12/31/git-things.html)

We [enforce](../.github/workflows/nrsr.yaml) the [not-rocket-science rule](https://graydon2.dreamwidth.org/1597.html).
The `main` branch is protected: it can only be amended by pull requests, which must have
passing tests.

Rebase merges are not permitted.
This enforces the invariant that the set of CI-tested commits is easily identifiable,
without requiring that every intermediate commit also pass tests.
Single-commit merges may be squashed. Multi-commit PRs should use a merge request.

`git log --first-parent --oneline` lists the tested ancestors of the current commit.
`git bisect --first-parent` likewise has bisection ignore untested commits.

The tests run in CI should be equivalent (for now) to `10j check-star`.
CI does not yet check whether any particular translations produce the expected results.
This ought to change in the future.

## Provisioning

Tenjin tries hard to be easy to install, in particular without requiring root/sudo access.
We do not automatically install Rust, because `rustup` transparently manages Rust toolchains,
and the details of how and where that happens should be left to the user.
We also assume `git` is available (having been used to fetch this repo).
Pretty much everything else is bootstrapped from a POSIX shell script, `cli/sh/provision.sh`.

The `10j` code itself is a Python project, managed by `uv`, which is downloaded as needed.
`10j check-py` will run Ruff lints and formatting checks. It will eventually run the `ty`
type checker, but not quite yet.

Provisioning places project prerequisites within a subdirectory called `_local`.
A file called `_local/config.10j-HAVE.json` lists the versions of each installed tool,
the idea being that the code in `10j` can dynamically declare a dependency on a particular
tool version, which will be checked against the HAVE list. Thus, as the active revision changes,
either from `git pull` or `bisect`, the support projects should be kept in sync automatically.

## How Do I…?

### disable formatting selectively

* Python: https://docs.astral.sh/ruff/formatter/#format-suppression

### Set up Visual Studio Code

TODO

### 

## CLI Notes

### `10j exec`

The tools in `_local` can be run via `10j exec`:

```
~/tenjin$ clang --version
Command 'clang' not found, but can be installed with:
sudo apt install clang
~/tenjin$ 10j exec clang --version | head -n1
Tenjin clang version 18.1.8
```

```
~/tenjin$ make --version | head -n1
GNU Make 4.3
~/tenjin$ 10j exec make --version | head -n1
GNU Make 4.4.1
```

You can also do something like `10j exec bash` which will give you a shell set up
with the PATH and other environment variables that `10j` passes to its subcommands.

### Exec aliases

A few commands (`cargo`, `opam` and `dune`) can be run as direct subcommands of `10j`, without going through `exec`.

# Project Infrastructure

Tenjin's build dependencies are maintained in the [tenjin-build-deps](https://github.com/Aarno-Labs/tenjin-build-deps) repo.
It defines container files based on [Alpine 3.21](https://github.com/Aarno-Labs/tenjin-build-deps/blob/main/containerfiles/alpine-3.21-builder) and [Debian Bullseye](https://github.com/Aarno-Labs/tenjin-build-deps/blob/main/containerfiles/debian-bullseye-builder). The corresponding container images are hosted on Docker Hub 
([debian](https://hub.docker.com/r/aarnotenjin/debian-bullseye-builder),
[alpine](https://hub.docker.com/r/aarnotenjin/alpine-3.21-builder))
and GitHub Container Registry
([debian](https://github.com/Aarno-Labs/tenjin-build-deps/pkgs/container/tenjin-debian-bullseye-builder),
[alpine](https://github.com/Aarno-Labs/tenjin-build-deps/pkgs/container/tenjin-alpine-3.21-builder)).
These containers are used to build most dependencies from source.

Debian Bullseye was chosen for glibc compatibility with most modern distros.
(`zig cc`'s ability to target glibc by version is appealing, but as of zig 0.14, `zig c++` miscompiles LLVM...)

The containers are built and pushed to both registries from [a manually-triggered GitHub workflow](https://github.com/Aarno-Labs/tenjin-build-deps/blob/main/.github/workflows/rebuildcontainer.yml).

Another [manually-triggered GitHub workflow](https://github.com/Aarno-Labs/tenjin-build-deps/blob/main/.github/workflows/makerelease.yml) uses the containers to build the non-LLVM components and creates a corresponding GitHub release
based on the current HEAD commit (example: [rev-03d4672c4](https://github.com/Aarno-Labs/tenjin-build-deps/releases/tag/rev-03d4672c4)). Tarballs are created for x86_64 and aarch64.

A [third manually-triggered GitHub workflow](https://github.com/Aarno-Labs/tenjin-build-deps/blob/main/.github/workflows/buildllvm.yml) builds LLVM for Mac and Linux (using the bullseye builder on the latter), and uploads the resulting tarballs to an existing GitHub release.