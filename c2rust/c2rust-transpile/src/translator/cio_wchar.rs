// `%lc` is a locale conversion, not a narrow byte or a Rust `char`.
mod xj_cio_wchar {
    unsafe extern "C" {
        fn wcrtomb(
            dst: *mut ::core::ffi::c_char,
            value: ::libc::wchar_t,
            state: *mut ::libc::mbstate_t,
        ) -> usize;
    }

    pub(super) fn print(stream: ::xj_cio::FILE_ptr, value: ::libc::wchar_t) -> ::core::ffi::c_int {
        // glibc's MB_LEN_MAX is 16; each fprintf `%lc` starts with initial state.
        let mut bytes = [0u8; 16];
        let mut state: ::libc::mbstate_t = unsafe { ::core::mem::zeroed() };
        let count = unsafe { wcrtomb(bytes.as_mut_ptr().cast(), value, &mut state) };
        if count == usize::MAX {
            return -1;
        }
        if ::xj_cio::fwrite_(&bytes[..count], stream) != count {
            -1
        } else {
            count as ::core::ffi::c_int
        }
    }
}
