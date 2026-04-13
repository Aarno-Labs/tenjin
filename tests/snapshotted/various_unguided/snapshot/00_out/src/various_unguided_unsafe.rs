extern "C" {
    fn isatty(_: ::core::ffi::c_int) -> ::core::ffi::c_int;
}
#[no_mangle]
pub unsafe extern "C" fn isatty_var(mut fd: ::core::ffi::c_int) -> ::core::ffi::c_int {
    return isatty(fd);
}
