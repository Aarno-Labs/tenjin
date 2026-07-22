# Slice Signature Reshaping

## Where

- [xj-prepare-slicetransform](/xj-prepare-slicetransform)

## What

C preparatory refactoring pass that reshapes `(ptr, len)` and `(lo, hi)`
parameter pairs into C struct "slices":

```c
typedef struct { int *ptr; size_t len; } RustSlice_int;
```

It runs immediately after
[pointer arithmetic reduction](pointer_arithmetic_reduction.md)
(`xj-prepare-pointertransform`) and consumes the metadata side-file that
pass wrote (`tenjin_ptr_index_metadata.json`, schema in
`xj-prepare-support/PtrIndexMetadata.h`). Every metadata fact is verified
against the AST before being applied, so stale or already-applied facts
(e.g. a shared header rewritten while processing an earlier translation
unit) are skipped.

Per reshaped function it performs:

- signature rewriting: the base/len (or lo/hi) parameters are replaced by
  a single slice parameter (`RustSlice_int arr`), with `typedef` emission
  and forward-declaration updates (including prototypes in headers);
- body touch-ups: references to the removed parameters become `arr.ptr` /
  `arr.len` forms, index-variable initializers gain lookback offsets, and
  bounds comparisons are adjusted for inclusive ends and lookahead;
- singleton ("swap-style") functions whose pointer parameters never move:
  each `T *a` parameter becomes an `int` index alongside the shared slice,
  and `*a` becomes `arr.ptr[a]`;
- call-site rewriting: callers pass the slice through, construct
  sub-slices, or wrap the original arguments in a compound literal
  (`(RustSlice_int){buf, n}`), widened by any lookback/lookahead;
- `T*` → `int` return-type collapsing (`return base + idx` → `return idx`,
  `return NULL` → `return -1`) and the corresponding caller-side fixes,
  including the "global-return" case for functions that only ever return
  `&global_array[i]`.

## Why

A C `(ptr, len)` pair carries no connection between the two arguments; a
slice struct does, and c2rust (with Tenjin guidance) can translate a
`RustSlice_<T>` parameter into a safe Rust `&[T]`/`&mut [T]` slice.

## Testing

Tool-level golden tests live in `tests/slice_transform_cases/` (and
`tests/pointer_transform_cases/` for index-rewriting-only cases), driven
by `tests/test_slice_transform.py` / `tests/test_pointer_transform.py`.
Each case checks the pointer pass's intermediate output, the metadata
side-file, and the chained final output against goldens, and verifies
compile-and-run equivalence at every stage.
