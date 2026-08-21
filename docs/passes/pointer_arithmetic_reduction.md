# Pointer Arithmetic Reduction and Slice Reshaping

## Where

- [xj-prepare-pointertransform](/xj-prepare-pointertransform)
- [xj-prepare-slicetransform](/xj-prepare-slicetransform)

## What

This pass includes two transformations:

### Pointer Arithmetic Reduction

Convert pointer arithmetic into explicitly subscripted accesses: each
moving pointer's motion is redirected into a companion integer index
variable (`p` → `p_index_xj`), with accesses spelled `p[p_index_xj]`.

The rewrite is **syntactic, local and total**. The pointer variable is its
own base: it stays where it is, stops moving, and holds whatever it was
last assigned, while the index holds the position. Nothing is deleted, and
the pass forms no opinion about what a base *is* — so there is no base to
be unstable, punned, const-qualified or reseated out from under an access,
and no shape has to be declined for any of those reasons. Four rules cover
every use:

| Use | Rewritten |
|---|---|
| `*p`, `p[i]`, `p->f`, `*(p + n)`, `*p++` | `p[p_index_xj + …]` |
| `p++`, `p += n` | `p_index_xj++`, `p_index_xj += n` |
| `p = q + 1` | `(p = q, p_index_xj = q_index_xj + 1)` |
| anything else that reads `p` | `(p + p_index_xj)` |

The last rule is the fallback, and it is what makes the rewrite total. The
only use that still declines a pointer is `&p`, where the pointer's storage
is observable and cannot hold a base while an index holds the position.

For each pointer rewritten by the tool, it records identifying facts — the
pointer's name, its companion index variable, and whether it is a parameter
— in a metadata side-file (`xj-ptrindex.json`, see
`xj-prepare-support/PtrIndexMetadata.h`). Identity only: nothing about a
base crosses the boundary. The (`xj-prepare-slicetransform`) tool runs
immediately afterwards, performs both the candidate *detection* and the
reshaping, from this tool's output plus those records. This tool's output
is always valid, compilable C with the original signatures intact.

**What a base equals is a separate question**, owned by base resolution
(`pointer_transform_base_resolution.md`) rather than by either rewriter.
Until that analysis is wired in, the slice pass cannot show that a local
pointer aliases a parameter, so it detects no roots and reshapes nothing.

### Slice Reshaping

C preparatory refactoring pass that reshapes `(ptr, len)` and `(lo, hi)`
parameter pairs into C struct "slices":

```c
typedef struct { int *ptr; size_t len; } RustSlice_int;
```

It runs immediately after
[pointer arithmetic reduction](pointer_arithmetic_reduction.md)
(`xj-prepare-pointertransform`) — within the same preparation pass — and
consumes the metadata side-file that tool wrote
(`xj-ptrindex.json`, schema in
`xj-prepare-support/PtrIndexMetadata.h`), which identifies each
synthesized index variable. The side-file is internal to the
pass: it is deleted before the pass finishes.

The tool works in two sweeps over the sources:

1. **Detection** (read-only, `SliceDetector`): finds every reshaping
   candidate from the index-transformed C, anchored by the per-pointer
   metadata records — root `(ptr, len)` / `(lo, hi)` functions (an index
   variable over a pointer parameter, bounded by another parameter),
   singleton callees, pointer-pair propagation (to a fixpoint, including
   recursion), and "global-return" functions. Detection completes for
   all translation units before any rewriting, so call sites in one TU
   see candidates defined in another.
2. **Rewriting** (`SliceRewriter`): applies the reshaping. Every
   detected fact is verified against the AST before being applied, so
   stale or already-applied facts (e.g. a shared header rewritten while
   processing an earlier translation unit) are skipped.

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

- A C array can be converted to a Rust slice, but there is no direct analogue
to `ptr++` in Rust. If mutation is redirected to modify integer indices instead
of pointers themselves, the resulting code is much easier to convert into safe
Rust constructs.

- A C `(ptr, len)` pair carries no connection between the two arguments; a
slice struct does, and c2rust (with Tenjin guidance) can translate a
`RustSlice_<T>` parameter into a safe Rust `&[T]`/`&mut [T]` slice.


## Examples

C:

```c
uint32_t get_bits(const uint8_t *p, int n) {
    uint32_t next, cache = 0, s = n & 7;
    int shl = n + s;
    next = *p++ & (255 >> s);  // <- pointer modified
    while ((shl-= 8) > 0) {
        cache |= next << shl;
        next = *p++;           // <- pointer modified
    }
    return cache | (next >>-shl);
}
```

c2rust output:
```rs
pub unsafe extern "C" fn get_bits(
    mut p: *const uint8_t,
    mut n: ::core::ffi::c_int,
) -> uint32_t {
    let mut next: uint32_t = 0;
    let mut cache: uint32_t = 0 as uint32_t;
    let mut s: uint32_t = (n & 7 as ::core::ffi::c_int) as uint32_t;
    let mut shl: ::core::ffi::c_int = (n as uint32_t).wrapping_add(s)
        as ::core::ffi::c_int;
    let c2rust_fresh0 = p;
    p = p.offset(1);
    next = (*c2rust_fresh0 as ::core::ffi::c_int & 255 as ::core::ffi::c_int >> s)
        as uint32_t;
    loop {
        shl -= 8 as ::core::ffi::c_int;
        if !(shl > 0 as ::core::ffi::c_int) {
            break;
        }
        cache |= next << shl;
        let c2rust_fresh1 = p;
        p = p.offset(1);
        next = *c2rust_fresh1 as uint32_t;
    }
    return cache | next >> -shl;
}
```

Transformed C:
```c
static uint32_t get_bits(const uint8_t *p, int n) {
    int p_index_xj = 0;
    uint32_t next, cache = 0, s = n & 7;
    int shl = n + s;
    next = p[p_index_xj++] & (255 >> s);  // <- pointer *not* modified
    while ((shl -= 8) > 0) {
        cache |= next << shl;
        next = p[p_index_xj++];           // <- pointer *not* modified
    }
    return cache | (next >> -shl);
}
```

Transformed Rust, with Tenjin guidance:
```rs
pub extern "C" fn get_bits(mut p: &[u8], mut n: ::core::ffi::c_int) -> uint32_t {
    let mut p_index = 0 as ::core::ffi::c_int;
    let mut next: uint32_t = 0;
    let mut cache = 0 as uint32_t;
    let mut s = (n & 7 as ::core::ffi::c_int) as uint32_t;
    let mut shl = (n as uint32_t).wrapping_add(s) as ::core::ffi::c_int;
    let fresh0 = p_index;
    p_index += 1;
    next = (p[fresh0 as usize] as ::core::ffi::c_int & 255 >> s) as uint32_t;
    loop {
        shl -= 8;
        if (shl <= 0) {
            break;
        }
        cache |= next << shl;
        let fresh1 = p_index;
        p_index += 1;
        next = p[fresh1 as usize] as uint32_t;
    }
    cache | next >> -shl
}
```

Note that the transformed program enable guidance to make the resulting Rust code safe.