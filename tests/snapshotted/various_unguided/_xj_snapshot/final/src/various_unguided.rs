extern "C" {
    fn isatty(_: ::core::ffi::c_int) -> ::core::ffi::c_int;

}
pub const STDIN_FILENO: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
pub const STDOUT_FILENO: ::core::ffi::c_int = 1 as ::core::ffi::c_int;
pub const STDERR_FILENO: ::core::ffi::c_int = 2 as ::core::ffi::c_int;
pub unsafe fn isatty_stdout() -> ::core::ffi::c_int {
    isatty(STDOUT_FILENO)
}
pub unsafe fn isatty_stderr() -> ::core::ffi::c_int {
    isatty(STDERR_FILENO)
}
pub unsafe fn isatty_stdin() -> ::core::ffi::c_int {
    isatty(STDIN_FILENO)
}
pub fn string_cond_1(mut cond: ::core::ffi::c_int) {
    println!("{:>}", {
        if cond != 0 {
            "true"
        } else {
            "false"
        }
    });
}
pub mod _xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn isatty_stdout() -> ::core::ffi::c_int {
        super::isatty_stdout()
    }
    #[no_mangle]
    pub unsafe extern "C" fn isatty_stderr() -> ::core::ffi::c_int {
        super::isatty_stderr()
    }
    #[no_mangle]
    pub unsafe extern "C" fn isatty_stdin() -> ::core::ffi::c_int {
        super::isatty_stdin()
    }
    #[no_mangle]
    pub extern "C" fn string_cond_1(arg0: ::core::ffi::c_int) {
        super::string_cond_1(arg0)
    }
}
