// Keep helper bindings separate from the translated C globals.
mod xj_cio_ptr_args {
    pub(super) fn xj_cio_ptr_arg(
        fmt: &core::ffi::CStr,
        position: usize,
        fallback: xj_cio::Arg,
    ) -> xj_cio::Arg {
        let bytes = fmt.to_bytes();
        let mut i = 0;
        let mut arg = 0;
        while i < bytes.len() {
            if bytes[i] != b'%' {
                i += 1;
                continue;
            }
            i += 1;
            if bytes.get(i) == Some(&b'%') {
                i += 1;
                continue;
            }
            while bytes.get(i).is_some_and(|b| b"-+ #0'".contains(b)) {
                i += 1;
            }
            while bytes.get(i).is_some_and(|b| b"0123456789.*".contains(b)) {
                arg += usize::from(bytes[i] == b'*');
                i += 1;
            }
            while bytes.get(i).is_some_and(|b| b"hlLjzt".contains(b)) {
                i += 1;
            }
            let Some(&conv) = bytes.get(i) else { break };
            i += 1;
            if arg == position {
                return if conv == b'n' {
                    let ptr = match fallback {
                        xj_cio::Arg::P(p) => p as *mut core::ffi::c_int,
                        xj_cio::Arg::S(p) => p as *mut core::ffi::c_int,
                        _ => unreachable!(),
                    };
                    xj_cio::Arg::OutN(ptr)
                } else {
                    fallback
                };
            }
            arg += 1;
        }
        fallback
    }
}
