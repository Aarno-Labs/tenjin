extern "C" {
    fn isatty(_: ::core::ffi::c_int) -> ::core::ffi::c_int;
    fn puts(_: *const ::core::ffi::c_char) -> ::core::ffi::c_int;
}
pub const STDIN_FILENO: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
pub const STDOUT_FILENO: ::core::ffi::c_int = 1 as ::core::ffi::c_int;
pub const STDERR_FILENO: ::core::ffi::c_int = 2 as ::core::ffi::c_int;
#[no_mangle]
pub unsafe extern "C" fn isatty_stdout() -> ::core::ffi::c_int {
    return isatty(STDOUT_FILENO);
}
#[no_mangle]
pub unsafe extern "C" fn isatty_stderr() -> ::core::ffi::c_int {
    return isatty(STDERR_FILENO);
}
#[no_mangle]
pub unsafe extern "C" fn isatty_stdin() -> ::core::ffi::c_int {
    return isatty(STDIN_FILENO);
}
#[no_mangle]
pub unsafe extern "C" fn string_cond_1(mut cond: ::core::ffi::c_int) {
    println!("{:>}", {
        std::ffi::CStr::from_ptr(
            (if cond != 0 {
                b"true\0" as *const u8 as *const ::core::ffi::c_char
            } else {
                b"false\0" as *const u8 as *const ::core::ffi::c_char
            }) as *const core::ffi::c_char,
        )
        .to_str()
        .unwrap()
    });
}
