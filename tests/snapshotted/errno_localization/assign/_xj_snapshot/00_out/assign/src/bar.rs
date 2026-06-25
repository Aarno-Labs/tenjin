pub unsafe fn bar() -> ::core::ffi::c_int {
    return 0 as ::core::ffi::c_int;
}
pub mod _xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub unsafe extern "C" fn bar() -> ::core::ffi::c_int {
        super::bar()
    }
}
