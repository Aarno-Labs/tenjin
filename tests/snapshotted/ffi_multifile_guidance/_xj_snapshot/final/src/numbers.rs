extern "C" {
    fn first_byte(s: *const ::core::ffi::c_char) -> ::core::ffi::c_uchar;
    fn strings_demo() -> ::core::ffi::c_int;
}
pub fn sum_n(mut xs: &[::core::ffi::c_int], mut n: ::core::ffi::c_int) -> ::core::ffi::c_int {
    let mut total = 0 as ::core::ffi::c_int;
    let mut i = 0 as ::core::ffi::c_int;
    while i < n {
        total += xs[i as usize];
        i += 1;
    }
    total
}
pub fn bump(mut p: &mut ::core::ffi::c_int) -> ::core::ffi::c_int {
    *p += 1;
    *p
}
#[no_mangle]
pub unsafe extern "C" fn numbers_demo() -> ::core::ffi::c_int {
    let mut xs: [::core::ffi::c_int; 3] = [1, 2, 3];
    let mut total = sum_n(&xs, 3 as ::core::ffi::c_int);
    total += bump(
        ((&raw mut xs as *mut ::core::ffi::c_int).offset(0) as *mut ::core::ffi::c_int)
            .as_mut()
            .unwrap(),
    );
    total += first_byte(b"world\0".as_ptr() as *const ::core::ffi::c_char) as ::core::ffi::c_int;
    total + strings_demo()
}
pub mod xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn sum_n(
        xs: *mut ::core::ffi::c_int,
        n: ::core::ffi::c_int,
    ) -> ::core::ffi::c_int {
        super::sum_n(std::slice::from_raw_parts(xs.cast(), n as usize), n)
    }
    #[no_mangle]
    pub unsafe extern "C" fn bump(p: *mut ::core::ffi::c_int) -> ::core::ffi::c_int {
        super::bump(p.as_mut().unwrap())
    }
}
