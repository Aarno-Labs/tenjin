#!/usr/bin/env bash
#
# Smoke-test the alloc-motion pass over the fixtures in this directory.
#
# For each `positive_*.c` we expect the tool to change the file and for the
# rewritten C to still compile; for each `negative_*.c` we expect the output
# to be byte-for-byte identical to the input. Run from the repo root or here:
#
#     ./cli/10j exec bash xj-prepare-allocmotion/tests/run_tests.sh
#
# (Run via `10j exec` so clang/cmake resolve against the hermetic toolchain.)

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
builddir="$repo/_local/_build_allocmotion"
bin="$builddir/xj-prepare-allocmotion"

# Build the tool. Configure once, then always do an incremental build so a
# stale binary never silently masks source changes (ninja no-ops if nothing
# changed).
if [[ ! -d "$builddir" ]]; then
    cmake -GNinja -S "$repo/xj-prepare-allocmotion" -B "$builddir" >/dev/null || exit 1
fi
cmake --build "$builddir" >/dev/null || exit 1

resource_dir="$(clang -print-resource-dir)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

run_one() {
    local src="$1"
    "$bin" "$src" -- -resource-dir="$resource_dir" 2>/dev/null
}

fail=0
shopt -s nullglob
for src in "$here"/positive_*.c "$here"/negative_*.c; do
    name="$(basename "$src")"
    out="$work/$name"
    run_one "$src" >"$out"

    if [[ "$name" == positive_* ]]; then
        if cmp -s "$src" "$out"; then
            echo "FAIL  $name: expected a rewrite, but output is unchanged"
            fail=1
        elif ! clang -std=c11 -Wall -Wextra -c "$out" -o /dev/null 2>"$work/$name.err"; then
            echo "FAIL  $name: rewritten C did not compile"
            sed 's/^/        /' "$work/$name.err"
            fail=1
        else
            echo "ok    $name: rewritten and compiles"
        fi
    else # negative_*
        if cmp -s "$src" "$out"; then
            echo "ok    $name: left untouched"
        else
            echo "FAIL  $name: expected no change, but the file was rewritten"
            diff -u "$src" "$out" | sed 's/^/        /'
            fail=1
        fi
    fi
done

exit "$fail"
