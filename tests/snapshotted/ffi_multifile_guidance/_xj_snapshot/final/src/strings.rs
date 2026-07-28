use ::libc;
pub fn first_byte(mut s: &[u8]) -> ::core::ffi::c_uchar {
    s[0] as ::core::ffi::c_uchar
}
pub fn zero_first(mut buf: &mut [u8], mut n: ::core::ffi::c_int) {
    if n > 0 {
        buf[0] = 0;
    }
}
#[no_mangle]
pub extern "C" fn strings_demo() -> ::core::ffi::c_int {
    let mut buf: [::core::ffi::c_uchar; 4] = [9, 9, 9, 9];
    zero_first(&mut buf, 4);
    buf[0] as ::core::ffi::c_int
}
pub mod xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn first_byte(s: *const ::core::ffi::c_char) -> ::core::ffi::c_uchar {
        super::first_byte({
            let __lift_2_931_0 = libc::strlen(s) + 1;
            std::slice::from_raw_parts(s.cast(), __lift_2_931_0)
        })
    }
    #[no_mangle]
    pub unsafe extern "C" fn zero_first(buf: *mut ::core::ffi::c_uchar, n: ::core::ffi::c_int) {
        super::zero_first(std::slice::from_raw_parts_mut(buf.cast(), n as usize), n)
    }
}
