pub fn bar() -> ::core::ffi::c_int {
    0
}
pub mod _xj_ffi {
    #[allow(unused_imports)]
    use super::*;
    #[no_mangle]
    pub extern "C" fn bar() -> ::core::ffi::c_int {
        super::bar()
    }
}
