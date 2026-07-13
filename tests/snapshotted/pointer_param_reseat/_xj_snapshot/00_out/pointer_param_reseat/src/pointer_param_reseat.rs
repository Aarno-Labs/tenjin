#![allow(
    clippy::missing_safety_doc,
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    unused_assignments,
    unused_mut
)]
#![feature(raw_ref_op)]
use ::std::process::ExitCode;
#[allow(unused_imports)]
use ::pointer_param_reseat;
#[no_mangle]
pub unsafe extern "C" fn write_not_null(mut dest: *mut ::core::ffi::c_int) -> ::core::ffi::c_int {
    let mut buf: [::core::ffi::c_int; 4] = [0 as ::core::ffi::c_int; 4];
    if dest.is_null() {
        dest = &raw mut buf as *mut ::core::ffi::c_int;
    }
    let c2rust_fresh0 = dest;
    dest = dest.offset(1);
    *c2rust_fresh0 = 1 as ::core::ffi::c_int;
    let c2rust_fresh1 = dest;
    dest = dest.offset(1);
    *c2rust_fresh1 = 2 as ::core::ffi::c_int;
    let c2rust_fresh2 = dest;
    dest = dest.offset(1);
    *c2rust_fresh2 = 3 as ::core::ffi::c_int;
    return buf[0 as usize] + buf[1 as usize] + buf[2 as usize];
}
unsafe fn main_0() -> ::core::ffi::c_int {
    return write_not_null(::core::ptr::null_mut::<::core::ffi::c_int>());
}
pub fn main() -> ExitCode {
    unsafe { ExitCode::from(main_0() as u8) }
}
