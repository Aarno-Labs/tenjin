# alloc-motion test fixtures

Small, self-contained C inputs for the `xj-prepare-allocmotion` pass.

- `positive_*.c` — patterns the pass should rewrite (malloc/calloc then full
  field-by-field init before first use). The runner checks that the output
  differs from the input and that the rewritten C still compiles.
- `negative_*.c` — patterns the pass must leave untouched (partial init,
  aliasing / address-taken, use-before-init). The runner checks the output is
  byte-for-byte identical to the input.

Run them all (from the repo root):

```sh
./cli/10j exec bash xj-prepare-allocmotion/tests/run_tests.sh
```

The script builds the tool into `_local/_build_allocmotion` if needed. To see a
single transform, run the tool directly and read its stdout:

```sh
_local/_build_allocmotion/xj-prepare-allocmotion --verbose \
    xj-prepare-allocmotion/tests/positive_decl_at_site.c \
    -- -resource-dir="$(clang -print-resource-dir)"
```

To list which allocations *would* be rewritten without changing anything, use
`--report` (dry run; prints one line per rewritable allocation to stdout):

```sh
_local/_build_allocmotion/xj-prepare-allocmotion --report \
    xj-prepare-allocmotion/tests/positive_two_allocs.c \
    -- -resource-dir="$(clang -print-resource-dir)"
# positive_two_allocs.c:14:5: two_allocs: 'p' -> box (struct P, 2 fields, null-check removed, decl-at-site)
# positive_two_allocs.c:20:5: two_allocs: 'q' -> box (struct P, 2 fields, decl-at-site)
```
