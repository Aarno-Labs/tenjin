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
pub fn ffi_ref_unwrap(mut p: &mut ::core::ffi::c_int) -> ::core::ffi::c_int {
    *p += 1;
    *p
}
pub fn ffi_from_ref_ret(mut p: &mut ::core::ffi::c_int) -> &mut ::core::ffi::c_int {
    *p += 1;
    p
}
pub fn ffi_via_cstr_empty_if_null(mut s: &[u8]) -> ::core::ffi::c_int {
    s[0] as ::core::ffi::c_int
}
pub fn ffi_pointer_reinterp(mut p: *const Option<&u8>) -> ::core::ffi::c_int {
    !p.is_null() as ::core::ffi::c_int
}
pub mod xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn ffi_via_cstr_first_byte(
        s: *const ::core::ffi::c_char,
    ) -> ::core::ffi::c_uchar {
        super::ffi_via_cstr_first_byte({
            let __lift_2_1641_1 = libc::strlen(s) + 1;
            std::slice::from_raw_parts(s.cast(), __lift_2_1641_1)
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
            {
                let __lift_2_3398_0 = libc::strlen(s) + 1;
                std::slice::from_raw_parts(s.cast(), __lift_2_3398_0)
            }
        } else {
            &[]
        };
        super::ffi_via_cstr_empty_if_null(s)
    }
    #[no_mangle]
    pub extern "C" fn ffi_pointer_reinterp(p: *const ::core::ffi::c_uchar) -> ::core::ffi::c_int {
        super::ffi_pointer_reinterp(p.cast::<Option<&u8>>())
    }
}
