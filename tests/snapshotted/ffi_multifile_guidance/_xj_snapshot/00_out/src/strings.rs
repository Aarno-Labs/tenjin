use ::libc;
pub unsafe fn first_byte(mut s: &[u8]) -> ::core::ffi::c_uchar {
    return s[0 as usize] as ::core::ffi::c_uchar;
}
pub unsafe fn zero_first(mut buf: &mut [u8], mut n: ::core::ffi::c_int) {
    if n > 0 as ::core::ffi::c_int {
        buf[0 as usize] = 0 as ::core::ffi::c_uchar;
    }
}
#[no_mangle]
pub unsafe extern "C" fn strings_demo() -> ::core::ffi::c_int {
    let mut buf: [::core::ffi::c_uchar; 4] = [
        9 as ::core::ffi::c_uchar,
        9 as ::core::ffi::c_uchar,
        9 as ::core::ffi::c_uchar,
        9 as ::core::ffi::c_uchar,
    ];
    zero_first(&mut buf, 4 as ::core::ffi::c_int);
    return buf[0 as usize] as ::core::ffi::c_int;
}
pub mod xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn first_byte(s: *const ::core::ffi::c_char) -> ::core::ffi::c_uchar {
        super::first_byte(std::slice::from_raw_parts(s.cast(), libc::strlen(s) + 1))
    }
    #[no_mangle]
    pub unsafe extern "C" fn zero_first(buf: *mut ::core::ffi::c_uchar, n: ::core::ffi::c_int) {
        super::zero_first(std::slice::from_raw_parts_mut(buf.cast(), n as usize), n)
    }
}
