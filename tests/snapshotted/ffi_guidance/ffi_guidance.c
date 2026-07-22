// These tests exercise the `ffi` guidance, which generates a C-ABI wrapper
// (collected into the `xj_ffi` module) that preserves the original C API and
// forwards to the translated Rust function using its guided argument/return
// types. See docs/USE.md.

typedef unsigned long size_t;
size_t strlen(const char *s);

// XREF:ffi_via_cstr
unsigned char ffi_via_cstr_first_byte(const char *s)
{
    return (unsigned char)s[0];
}

// XREF:ffi_slice_with_literal_length
int ffi_slice_literal_len(int *xs)
{
    return xs[0] + xs[1] + xs[2];
}

// XREF:ffi_slice_with_variable_length
int ffi_slice_var_len(int *xs, int n)
{
    return xs[n - 1];
}

// XREF:ffi_mut_slice_with_variable_length
void ffi_mut_slice_var_len(int *xs, int n)
{
    xs[0] = n;
}

// XREF:ffi_from_slice_return
unsigned char *ffi_from_slice_ret(unsigned char *buf, int n)
{
    buf[0] = 0;
    return buf;
}

// XREF:ffi_ref_unwrap
int ffi_ref_unwrap(int *p)
{
    *p += 1;
    return *p;
}

// XREF:ffi_from_ref_return
int *ffi_from_ref_ret(int *p)
{
    *p += 1;
    return p;
}

// XREF:ffi_lift_from_ref_shared
// `p` and the return value are guided to `&i32`. On the way out, the return
// conversion is a `pipe` of `lift` (wrap the `&i32` in `Some`) then `from-ref`,
// lowering the resulting `Option<&i32>` back to a `*const i32` (NULL if `None`).
const int *ffi_lift_from_ref_shared(const int *p)
{
    return p;
}

// XREF:ffi_via_cstr_empty_if_null
int ffi_via_cstr_empty_if_null(const char *s)
{
    return s[0];
}

// XREF:ffi_pointer_reinterp
const unsigned char *ffi_pointer_reinterp(const unsigned char **p)
{
    return *p;
}
