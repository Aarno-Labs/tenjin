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
#[no_mangle]
pub extern "C" fn string_cond_1(mut cond: ::core::ffi::c_int) {
    println!("{:>}", {
        if cond != 0 {
            "true"
        } else {
            "false"
        }
    });
}
#[no_mangle]
pub extern "C" fn assert_plain(mut x: ::core::ffi::c_int) {
    assert!(x > 0);
}
#[no_mangle]
pub extern "C" fn assert_msg(mut x: ::core::ffi::c_int) {
    assert!(x > 0, "x must be positive");
}
#[no_mangle]
pub extern "C" fn assert_msg_chained(mut x: ::core::ffi::c_int, mut y: ::core::ffi::c_int) {
    assert!(x > 0 && y > 0, "both must be positive");
}
#[no_mangle]
pub extern "C" fn assert_msg_braces(mut x: ::core::ffi::c_int) {
    assert!(x > 0, "x must not be {{0}}");
}
