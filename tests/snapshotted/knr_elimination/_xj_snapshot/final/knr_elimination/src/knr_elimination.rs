#![allow(
    clippy::missing_safety_doc,
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    unused_assignments,
    unused_mut
)]
#[allow(unused_imports)]
use ::knr_elimination;
use ::std::process::ExitCode;

#[no_mangle]
pub extern "C" fn scale(
    mut x: ::core::ffi::c_int,
    mut factor: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    x * factor
}
#[no_mangle]
pub extern "C" fn blend(
    mut c_xjknr: ::core::ffi::c_int,
    mut weight_xjknr: ::core::ffi::c_double,
) -> ::core::ffi::c_int {
    let mut c = c_xjknr as ::core::ffi::c_char;
    let mut weight = weight_xjknr as ::core::ffi::c_float;
    (c as ::core::ffi::c_int as ::core::ffi::c_float * weight) as ::core::ffi::c_int
}
#[no_mangle]
pub unsafe extern "C" fn total(
    mut a: ::core::ffi::c_int,
    mut b: ::core::ffi::c_int,
    mut label: *mut ::core::ffi::c_char,
) -> ::core::ffi::c_int {
    a + b + *label.offset(0) as ::core::ffi::c_int
}
#[no_mangle]
pub unsafe extern "C" fn first_of(
    mut buf: *mut ::core::ffi::c_char,
    mut n: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    *buf.offset(n as isize) as ::core::ffi::c_int
}
#[no_mangle]
pub extern "C" fn seven() -> ::core::ffi::c_int {
    7
}
unsafe fn main_0() -> ::core::ffi::c_int {
    println!(
        "{:} {:} {:} {:} {:}",
        scale(6, 7) as core::ffi::c_int,
        blend('A' as ::core::ffi::c_int, 2.0f64) as core::ffi::c_int,
        total(
            1,
            2,
            b"z\0".as_ptr() as *const ::core::ffi::c_char as *mut ::core::ffi::c_char,
        ) as core::ffi::c_int,
        first_of(
            b"xyz\0".as_ptr() as *const ::core::ffi::c_char as *mut ::core::ffi::c_char,
            1,
        ) as core::ffi::c_int,
        seven() as core::ffi::c_int,
    );
    0
}
pub fn main() -> ExitCode {
    unsafe { ExitCode::from(main_0() as u8) }
}
