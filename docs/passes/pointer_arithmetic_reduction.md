# Pointer Arithmetic Reduction

## Where

- [xj-prepare-pointertransform](/xj-prepare-pointertransform)

## What

C preparatory refactoring pass to convert pointer arithmetic into explicitly
subscripted accesses: each moving pointer's motion is redirected into a
companion integer index variable (`p` → `p_index_xj`), with accesses spelled
`base[p_index_xj]`.

This pass also *detects* which functions can have their `(ptr, len)` /
`(lo, hi)` parameter pairs reshaped into `RustSlice_<T>` structs, but does
not apply that reshaping itself: the detection results are written to a
metadata side-file (`tenjin_ptr_index_metadata.json`, see
`xj-prepare-support/PtrIndexMetadata.h`) which the immediately-following
[slice signature reshaping](slice_signature_reshaping.md) pass
(`xj-prepare-slicetransform`) consumes. This pass's output is always valid,
compilable C with the original signatures intact.

## Why

A C array can be converted to a Rust slice, but there is no direct analogue
to `ptr++` in Rust. If mutation is redirected to modify integer indices instead
of pointers themselves, the resulting code is much easier to convert into safe
Rust constructs.

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
static uint32_t get_bits(bs_t *bs, int n) {
    uint32_t next, cache = 0, s = bs->pos & 7;
    int shl = n + s;
    const uint8_t *p = bs->buf + (bs->pos >> 3);
    if ((bs->pos += n) > bs->limit) return 0;

    int p_index = (bs->pos >> 3);

    next = (bs->buf)[p_index++] & (255 >> s);  // <- pointer *not* modified
    while ((shl-= 8) > 0) {
        cache |= next << shl;
        next = (bs->buf)[p_index++];           // <- pointer *not* modified
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