// These tests exercise the `ffi` guidance, which generates a C-ABI wrapper
// (collected into the `xj_ffi` module) that preserves the original C API and
// forwards to the translated Rust function using its guided argument/return
// types. See docs/USE.md.

typedef unsigned long size_t;
size_t strlen(const char *s);

// XREF:ffi_via_cstr
// The `s` parameter is guided to `&[u8]`; the wrapper reconstructs the slice
// from the incoming `char*` using `strlen` (`via-cstr`).
unsigned char ffi_via_cstr_first_byte(const char *s)
{
    return (unsigned char)s[0];
}

// XREF:ffi_slice_with_literal_length
// The `xs` parameter is guided to `&[i32]`; the wrapper reconstructs the slice
// with a fixed compile-time length of 3.
int ffi_slice_literal_len(int *xs)
{
    return xs[0] + xs[1] + xs[2];
}

// XREF:ffi_slice_with_variable_length
// The `xs` parameter is guided to `&[i32]`; the wrapper reconstructs the slice
// using the `n` argument as its length.
int ffi_slice_var_len(int *xs, int n)
{
    return xs[n - 1];
}

// XREF:ffi_mut_slice_with_variable_length
// The `xs` parameter is guided to `&mut [i32]`; the wrapper reconstructs a
// mutable slice using the `n` argument as its length.
void ffi_mut_slice_var_len(int *xs, int n)
{
    xs[0] = n;
}

// XREF:ffi_from_slice_return
// Both `buf` and the return value are guided to `&mut [u8]`. The wrapper
// reconstructs the mutable slice on the way in (`slice-with-length`) and lowers
// the returned slice back to a raw pointer on the way out (`from-slice`).
unsigned char *ffi_from_slice_ret(unsigned char *buf, int n)
{
    buf[0] = 0;
    return buf;
}
