use ::libc;
pub unsafe fn ffi_via_cstr_first_byte(mut s: &[u8]) -> ::core::ffi::c_uchar {
    return s[0 as usize] as ::core::ffi::c_uchar;
}
pub unsafe fn ffi_slice_literal_len(mut xs: &[::core::ffi::c_int]) -> ::core::ffi::c_int {
    return xs[0 as usize] + xs[1 as usize] + xs[2 as usize];
}
pub unsafe fn ffi_slice_var_len(
    mut xs: &[::core::ffi::c_int],
    mut n: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    return xs[(n - 1 as ::core::ffi::c_int) as usize];
}
pub unsafe fn ffi_mut_slice_var_len(mut xs: &mut [::core::ffi::c_int], mut n: ::core::ffi::c_int) {
    xs[0 as usize] = n;
}
pub unsafe fn ffi_from_slice_ret(mut buf: &mut [u8], mut n: ::core::ffi::c_int) -> &mut [u8] {
    buf[0 as usize] = 0 as ::core::ffi::c_uchar;
    return buf;
}
pub unsafe fn ffi_ref_unwrap(mut p: &mut ::core::ffi::c_int) -> ::core::ffi::c_int {
    *p += 1 as ::core::ffi::c_int;
    return *p;
}
pub unsafe fn ffi_from_ref_ret(mut p: &mut ::core::ffi::c_int) -> &mut ::core::ffi::c_int {
    *p += 1 as ::core::ffi::c_int;
    return p;
}
pub unsafe fn ffi_via_cstr_empty_if_null(mut s: &[u8]) -> ::core::ffi::c_int {
    return s[0 as usize] as ::core::ffi::c_int;
}
pub unsafe fn ffi_pointer_reinterp(mut p: *const Option<&u8>) -> ::core::ffi::c_int {
    return !p.is_null() as ::core::ffi::c_int;
}
pub mod xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn ffi_via_cstr_first_byte(
        s: *const ::core::ffi::c_char,
    ) -> ::core::ffi::c_uchar {
        super::ffi_via_cstr_first_byte(std::slice::from_raw_parts(s.cast(), libc::strlen(s) + 1))
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
    #[no_mangle]
    pub unsafe extern "C" fn ffi_ref_unwrap(p: *mut ::core::ffi::c_int) -> ::core::ffi::c_int {
        super::ffi_ref_unwrap(p.as_mut().unwrap())
    }
    #[no_mangle]
    pub unsafe extern "C" fn ffi_from_ref_ret(
        p: *mut ::core::ffi::c_int,
    ) -> *mut ::core::ffi::c_int {
        super::ffi_from_ref_ret(p.as_mut().unwrap()) as *mut _
    }
    #[no_mangle]
    pub unsafe extern "C" fn ffi_via_cstr_empty_if_null(
        s: *const ::core::ffi::c_char,
    ) -> ::core::ffi::c_int {
        let s = if !s.is_null() {
            std::slice::from_raw_parts(s.cast(), libc::strlen(s) + 1)
        } else {
            &[]
        };
        super::ffi_via_cstr_empty_if_null(s)
    }
    #[no_mangle]
    pub unsafe extern "C" fn ffi_pointer_reinterp(
        p: *const ::core::ffi::c_uchar,
    ) -> ::core::ffi::c_int {
        super::ffi_pointer_reinterp(p.cast::<Option<&u8>>())
    }
}
