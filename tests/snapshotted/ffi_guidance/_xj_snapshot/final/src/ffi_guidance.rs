use ::libc;
pub fn ffi_via_cstr_first_byte(mut s: &[u8]) -> ::core::ffi::c_uchar {
    s[0] as ::core::ffi::c_uchar
}
pub fn ffi_slice_literal_len(mut xs: &[::core::ffi::c_int]) -> ::core::ffi::c_int {
    xs[0] + xs[1] + xs[2]
}
pub fn ffi_slice_var_len(
    mut xs: &[::core::ffi::c_int],
    mut n: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    xs[(n - 1) as usize]
}
pub fn ffi_mut_slice_var_len(mut xs: &mut [::core::ffi::c_int], mut n: ::core::ffi::c_int) {
    xs[0] = n;
}
pub fn ffi_from_slice_ret(mut buf: &mut [u8], mut n: ::core::ffi::c_int) -> &mut [u8] {
    buf[0] = 0;
    buf
}
pub mod xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn ffi_via_cstr_first_byte(
        s: *const ::core::ffi::c_char,
    ) -> ::core::ffi::c_uchar {
        super::ffi_via_cstr_first_byte({
            let __lift_2_1090_0 = libc::strlen(s) + 1;
            std::slice::from_raw_parts(s.cast(), __lift_2_1090_0)
        })
    }
    #[no_mangle]
    pub unsafe extern "C" fn ffi_slice_literal_len(
        xs: *mut ::core::ffi::c_int,
    ) -> ::core::ffi::c_int {
        super::ffi_slice_literal_len(std::slice::from_raw_parts(xs.cast(), 3))
    }
    #[no_mangle]
    pub unsafe extern "C" fn ffi_slice_var_len(
        xs: *mut ::core::ffi::c_int,
        n: ::core::ffi::c_int,
    ) -> ::core::ffi::c_int {
        super::ffi_slice_var_len(std::slice::from_raw_parts(xs.cast(), n as usize), n)
    }
    #[no_mangle]
    pub unsafe extern "C" fn ffi_mut_slice_var_len(
        xs: *mut ::core::ffi::c_int,
        n: ::core::ffi::c_int,
    ) {
        super::ffi_mut_slice_var_len(std::slice::from_raw_parts_mut(xs.cast(), n as usize), n)
    }
    #[no_mangle]
    pub unsafe extern "C" fn ffi_from_slice_ret(
        buf: *mut ::core::ffi::c_uchar,
        n: ::core::ffi::c_int,
    ) -> *mut ::core::ffi::c_uchar {
        super::ffi_from_slice_ret(std::slice::from_raw_parts_mut(buf.cast(), n as usize), n)
            .as_mut_ptr()
    }
}
