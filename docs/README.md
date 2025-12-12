# Tenjin Developer Docs

## [Git Things](https://matklad.github.io/2023/12/31/git-things.html)

We [enforce](../.github/workflows/nrsr.yaml) the [not-rocket-science rule](https://graydon2.dreamwidth.org/1597.html).
The `main` branch is protected: it can only be amended by pull requests, which must have
passing tests.

Rebase merges are not permitted.
This enforces the invariant that the set of CI-tested commits is easily identifiable,
[without requiring](https://matklad.github.io/2023/12/31/git-things.html)
that every intermediate commit also pass tests.
Single-commit merges may be squashed. Multi-commit PRs should use a merge request.

`git log --first-parent --oneline` lists the tested ancestors of the current commit.
`git bisect --first-parent` likewise has bisection ignore untested commits.

The tests run in CI should be equivalent (for now) to `10j check-star && 10j check-unit-rs && 10j runtests tests`.

Some commits should have their message prefixed with a tag:
- `NFC: ` -- No Functional Change, for behavior-preserving refactorings
- `TF: ` -- Tests Failing, when the commit intentionally contains a known regression (e.g. for demonstration purposes)

## Provisioning

Tenjin tries hard to be easy to install, in particular without requiring root/sudo access.
We do not automatically install Rust, because `rustup` transparently manages Rust toolchains,
and the details of how and where that happens should be left to the user -- in particular,
how to integrate with the user's shell, and where toolchains should be installed.
Tenjin does require specific Rust toolchains, both stable and nightly. These toolchains are
selected and updated on demand.

We also assume `git` is available (having been used to fetch this repo).
A POSIX shell plus `curl` or `wget` is all that's needed to bootstrap.

The `10j` code itself is a Python project, managed by `uv`, which is downloaded as needed.
`10j check-py` will run Ruff lints and formatting checks. It will eventually run the `ty`
type checker, but not quite yet; for now we use `mypy`.

### Transparent Dependency Updates

Provisioning places project prerequisites within a subdirectory called `_local`.
A file called `_local/config.10j-HAVE.json` lists the versions of each installed tool,
the idea being that the code in `10j` can dynamically declare a dependency on a particular
tool version, which will be checked against the HAVE list. Thus, as the active revision changes,
either from `git pull` or `bisect`, the support projects should be kept in sync automatically.

## How Do I…?

### disable formatting selectively

* Python: https://docs.astral.sh/ruff/formatter/#format-suppression

### Set up Visual Studio Code

- Extensions: Python/Pylance, rust-analyzer, Ruff
  - Disable the Black formatter extension, if you already have it, as it will produce style mismatches.
  - Depending on what parts of the codebase are being analyzed, `rust-analyzer` may need access to Tenjin's LLVM library directory. From Tenjin's root directory, invoke `10j exec code .` (see below regarding `10j exec`).
- In the Problems panel, filter irrelevant warnings with `!c2rust/dynamic_instrumentation,!c2rust/pdg`

### Update snapshot tests

- Either:
  - `10j test-unit-rs` then `(cd c2rust ; 10j cargo insta review)`, or
  - `INSTA_UPDATE=always 10j test-unit-rs` then review the `*.rs` diffs

### synchronize with upstream `c2rust`

See the section below about `git-subrepo`.

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
To see what that environment is, try `10j exec env`.

### Exec aliases

A few commands (`cargo`, `clang`, `opam` and `dune`) can be run as direct subcommands of `10j`, without going through `exec`.

```
~/tenjin$ 10j clang --version | head -n1
Tenjin clang version 18.1.8
```

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
based on the current HEAD commit (example: [rev-03d4672c4](https://github.com/Aarno-Labs/tenjin-build-deps/releases/tag/rev-03d4672c4)).
Tarballs for core build tools and libraries needed on Linux are created for x86\_64 and aarch64. Another set of tarballs ("more-deps") are built for Mac aarch64 as well as Linux.

A [third manually-triggered GitHub workflow](https://github.com/Aarno-Labs/tenjin-build-deps/blob/main/.github/workflows/buildllvm.yml) builds LLVM for Mac and Linux (using the bullseye builder on the latter), and uploads the resulting tarballs to an existing GitHub release.

## `git-subrepo`

The `c2rust/` directory is managed with [git-subrepo](https://github.com/ingydotnet/git-subrepo).
This helps automate synchronization with upstream, without requiring extra software from most Tenjin users.
Only folks who want to pull in new changes from upstream need to have `git-subrepo` installed.

The `Aarno-Labs/tenjin` repository brings in the `tenjin` branch of
[`Aarno-Labs/c2rust`](https://github.com/Aarno-Labs/c2rust/) as a subrepo.

To sync with the latest changes from upstream (that is, `immunant/c2rust`):

1. Sync `Aarno-Labs/c2rust`'s `master` branch with `immunant/c2rust`
2. Merge `master` into the `tenjin` branch of `Aarno-Labs/c2rust` (and push)
3. Check out a new branch in (your fork of) `Aarno-Labs/tenjin`, then do `git subrepo pull c2rust`. (You do NOT need to do `git subrepo init` first.)
4. If there are merge conflicts, follow the listed instructions to address them
5. Your new branch should eventually wind up with a commit like [this one](https://github.com/Aarno-Labs/tenjin/commit/8b6a2b94f73453f20f03e3aa58a6332a71a9638f ) for a conflict-free merge or [this one](https://github.com/Aarno-Labs/tenjin/commit/7be732b3260a9d2b08d36d53b2f262487af83c25) for a hand-resolved merge
6. Make sure to run `10j check-rs` and `10j test-unit-rs` and `10j check-e2e-smoke-tests` and address any issues raised. Also run at least one non-trivial translation, to validate that our invocations of `c2rust` are still kosher. Do these as separate commits, to minimize changes in the merge commit.
7. Push the branch!
