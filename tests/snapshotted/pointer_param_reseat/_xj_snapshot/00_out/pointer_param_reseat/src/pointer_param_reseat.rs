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
    let mut dest_index_xj = 0 as ::core::ffi::c_int;
    let mut buf: [::core::ffi::c_int; 4] = [0 as ::core::ffi::c_int; 4];
    if dest.is_null() {
        dest = &raw mut buf as *mut ::core::ffi::c_int;
        dest_index_xj = 0 as ::core::ffi::c_int;
    }
    let c2rust_fresh0 = dest_index_xj;
    dest_index_xj += 1;
    *dest.offset(c2rust_fresh0 as isize) = 1 as ::core::ffi::c_int;
    let c2rust_fresh1 = dest_index_xj;
    dest_index_xj += 1;
    *dest.offset(c2rust_fresh1 as isize) = 2 as ::core::ffi::c_int;
    let c2rust_fresh2 = dest_index_xj;
    dest_index_xj += 1;
    *dest.offset(c2rust_fresh2 as isize) = 3 as ::core::ffi::c_int;
    return buf[0usize] + buf[1usize] + buf[2usize];
}
unsafe fn main_0() -> ::core::ffi::c_int {
    return write_not_null(::core::ptr::null_mut::<::core::ffi::c_int>());
}
pub fn main() -> ExitCode {
    unsafe { ExitCode::from(main_0() as u8) }
}
