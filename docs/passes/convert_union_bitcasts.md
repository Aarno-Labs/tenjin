# Union Bitcast Conversion

## Where

- [xj-prepare-unionbitcasts](/xj-prepare-unionbitcasts)

## What

C preparatory refactoring pass to insert wrapper functions marking
uses of unions that can be converted to Rust's `to_bits()` and `from_bits()`
methods on `f32`/`f64`.

## Why

Unions are unsafe in their unrestricted general form, but restricted forms of
usage can be recognized and translated to safe code.

## Examples

C:

```c
union conv {
    float flt;
    uint32_t num;
};
uint32_t float_to_bits(float flt) {
    union conv in;
    in.flt = flt;
    return in.num;
}
```

c2rust output:
```rs
pub union conv {
    pub flt: ::core::ffi::c_float,
    pub num: uint32_t,
}
pub unsafe extern "C" fn float_to_bits(mut flt: ::core::ffi::c_float) -> uint32_t {
    let mut in_0: conv = conv { flt: 0. };
    in_0.flt = flt;
    return in_0.num;
}
```

Transformed C:
```c
static void __tenjin_bvm_float_to_uint32_t(float x, uint32_t *out) {
    memcpy(out, &x, 4);
}
uint32_t float_to_bits(float flt) {
    // new temp vars for union fields
    float __tenjin_tmp_in;
    uint32_t __tenjin_tmp_out;
    __tenjin_tmp_in = flt;
    __tenjin_bvm_float_to_uint32_t(__tenjin_tmp_in, &__tenjin_tmp_out);
    return __tenjin_tmp_out;
}
```

Transformed Rust:
```rs
#[no_mangle]
pub extern "C" fn float_to_bits(mut flt: ::core::ffi::c_float) -> uint32_t {
    let mut __tenjin_tmp_in_in: ::core::ffi::c_float = 0.;
    let mut __tenjin_tmp_out_in: uint32_t = 0;
    __tenjin_tmp_in_in = flt;
    __tenjin_tmp_out_in = __tenjin_tmp_in_in.to_bits() as uint32_t;
    __tenjin_tmp_out_in
}
```

Note that the transformed Rust is safe (albeit ugly before the temporaries are removed).