#![allow(
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    unused_assignments,
    unused_mut
)]
#[allow(unused_imports)]
use ::main;
use ::std::process::ExitCode;
extern "C" {

    fn __errno_location() -> *mut ::core::ffi::c_int;
}
pub type __time_t = ::core::ffi::c_long;
pub type time_t = __time_t;
unsafe fn _xj_wrap_time_xjtr_0(mut _xj_errno: &mut i32, mut __timer: *mut time_t) -> time_t {
    let mut ret = xj_ctime::compat::time(__timer.as_mut());
    *_xj_errno = *__errno_location();
    ret
}
unsafe fn main_0(
    mut argc: ::core::ffi::c_int,
    mut argv: *mut *mut ::core::ffi::c_char,
) -> ::core::ffi::c_int {
    let mut _xj_local_errno: i32 = 0;
    _xj_local_errno = 0;
    let mut t: time_t = 0;
    _xj_wrap_time_xjtr_0((&raw mut _xj_local_errno).as_mut().unwrap(), &raw mut t);
    if _xj_local_errno == 0 {
        return 0;
    }
    1
}
pub fn main() -> ExitCode {
    let mut args_strings: Vec<Vec<u8>> = ::std::env::args()
        .map(|arg| {
            ::std::ffi::CString::new(arg)
                .expect("Failed to convert argument into CString.")
                .into_bytes_with_nul()
        })
        .collect();
    let mut args_ptrs: Vec<*mut ::core::ffi::c_char> = args_strings
        .iter_mut()
        .map(|arg| arg.as_mut_ptr() as *mut ::core::ffi::c_char)
        .chain(::core::iter::once(::core::ptr::null_mut()))
        .collect();
    unsafe {
        ExitCode::from(main_0(
            (args_ptrs.len() - 1) as ::core::ffi::c_int,
            args_ptrs.as_mut_ptr() as *mut *mut ::core::ffi::c_char,
        ) as u8)
    }
}
