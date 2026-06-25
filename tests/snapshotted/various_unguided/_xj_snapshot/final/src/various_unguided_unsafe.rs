extern "C" {
    fn isatty(_: ::core::ffi::c_int) -> ::core::ffi::c_int;
}
pub unsafe fn isatty_var(mut fd: ::core::ffi::c_int) -> ::core::ffi::c_int {
    isatty(fd)
}
pub mod _xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn isatty_var(arg0: ::core::ffi::c_int) -> ::core::ffi::c_int {
        super::isatty_var(arg0)
    }
}
