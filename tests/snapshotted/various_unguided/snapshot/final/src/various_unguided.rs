extern "C" {
    fn isatty(_: ::core::ffi::c_int) -> ::core::ffi::c_int;
}
pub const STDIN_FILENO: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
pub const STDOUT_FILENO: ::core::ffi::c_int = 1 as ::core::ffi::c_int;
pub const STDERR_FILENO: ::core::ffi::c_int = 2 as ::core::ffi::c_int;
#[no_mangle]
pub unsafe extern "C" fn isatty_stdout() -> ::core::ffi::c_int {
    isatty(STDOUT_FILENO)
}
#[no_mangle]
pub unsafe extern "C" fn isatty_stderr() -> ::core::ffi::c_int {
    isatty(STDERR_FILENO)
}
#[no_mangle]
pub unsafe extern "C" fn isatty_stdin() -> ::core::ffi::c_int {
    isatty(STDIN_FILENO)
}
