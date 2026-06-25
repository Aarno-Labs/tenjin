#![allow(
    clippy::missing_safety_doc,
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    unused_assignments,
    unused_mut
)]
#![feature(extern_types, raw_ref_op)]
#[allow(unused_imports)]
use ::assign;
use ::std::process::ExitCode;
extern "C" {
    pub type _IO_wide_data;
    pub type _IO_codecvt;
    pub type _IO_marker;
    fn fclose(__stream: *mut FILE) -> ::core::ffi::c_int;
    fn __errno_location() -> *mut ::core::ffi::c_int;
}
pub type size_t = usize;
pub type __off_t = ::core::ffi::c_long;
pub type __off64_t = ::core::ffi::c_long;
#[derive(Copy, Clone)]
#[repr(C)]
pub struct _IO_FILE {
    pub _flags: ::core::ffi::c_int,
    pub _IO_read_ptr: *mut ::core::ffi::c_char,
    pub _IO_read_end: *mut ::core::ffi::c_char,
    pub _IO_read_base: *mut ::core::ffi::c_char,
    pub _IO_write_base: *mut ::core::ffi::c_char,
    pub _IO_write_ptr: *mut ::core::ffi::c_char,
    pub _IO_write_end: *mut ::core::ffi::c_char,
    pub _IO_buf_base: *mut ::core::ffi::c_char,
    pub _IO_buf_end: *mut ::core::ffi::c_char,
    pub _IO_save_base: *mut ::core::ffi::c_char,
    pub _IO_backup_base: *mut ::core::ffi::c_char,
    pub _IO_save_end: *mut ::core::ffi::c_char,
    pub _markers: *mut _IO_marker,
    pub _chain: *mut _IO_FILE,
    pub _fileno: ::core::ffi::c_int,
    pub _flags2: ::core::ffi::c_int,
    pub _old_offset: __off_t,
    pub _cur_column: ::core::ffi::c_ushort,
    pub _vtable_offset: ::core::ffi::c_schar,
    pub _shortbuf: [::core::ffi::c_char; 1],
    pub _lock: *mut ::core::ffi::c_void,
    pub _offset: __off64_t,
    pub _codecvt: *mut _IO_codecvt,
    pub _wide_data: *mut _IO_wide_data,
    pub _freeres_list: *mut _IO_FILE,
    pub _freeres_buf: *mut ::core::ffi::c_void,
    pub __pad5: size_t,
    pub _mode: ::core::ffi::c_int,
    pub _unused2: [::core::ffi::c_char; 20],
}

pub type FILE = _IO_FILE;
pub const EINVAL: ::core::ffi::c_int = 22 as ::core::ffi::c_int;
pub fn foo() -> ::core::ffi::c_int {
    0
}
pub unsafe fn doesnt_use_errno(mut f: *mut FILE) -> ::core::ffi::c_int {
    fclose(f);
    0
}
unsafe fn _xj_wrap_fclose_xjtr_0(
    mut _xj_errno: &mut i32,
    mut __stream: *mut FILE,
) -> ::core::ffi::c_int {
    let mut ret = fclose(__stream);
    *_xj_errno = *__errno_location();
    ret
}
pub unsafe fn does_use_errno(mut f: *mut FILE) -> ::core::ffi::c_int {
    let mut _xj_local_errno: i32 = 0;
    let mut r = _xj_wrap_fclose_xjtr_0(&mut _xj_local_errno, f);
    if r < 0 {
        return _xj_local_errno;
    }
    0
}
unsafe fn main_0(
    mut argc: ::core::ffi::c_int,
    mut argv: *mut *mut ::core::ffi::c_char,
) -> ::core::ffi::c_int {
    let mut _xj_local_errno: i32 = 0;
    foo();
    _xj_local_errno = 0;
    if _xj_local_errno == EINVAL {
        assign::src::bar::bar();
    }
    0
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
    let argc = (args_ptrs.len() - 1) as ::core::ffi::c_int;
    let argv = args_ptrs.as_mut_ptr() as *mut *mut ::core::ffi::c_char;
    unsafe { ExitCode::from(main_0(argc, argv) as u8) }
}
pub mod _xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub extern "C" fn foo() -> ::core::ffi::c_int {
        super::foo()
    }
    #[no_mangle]
    pub unsafe extern "C" fn doesnt_use_errno(arg0: *mut FILE) -> ::core::ffi::c_int {
        super::doesnt_use_errno(arg0)
    }
    #[no_mangle]
    pub unsafe extern "C" fn does_use_errno(arg0: *mut FILE) -> ::core::ffi::c_int {
        super::does_use_errno(arg0)
    }
}
