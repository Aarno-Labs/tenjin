unsafe fn xj_cio_vargs(fmt: &core::ffi::CStr, mut ap: ::core::ffi::VaList) -> Vec<xj_cio::Arg> {
    use xj_cio::Arg;
    let bytes = fmt.to_bytes();
    let mut out = Vec::new();
    let mut i = 0;
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
        if bytes.get(i) == Some(&b'*') {
            out.push(Arg::I(unsafe { ap.arg::<core::ffi::c_int>() } as i64));
            i += 1;
        } else {
            while bytes.get(i).is_some_and(u8::is_ascii_digit) {
                i += 1;
            }
        }
        if bytes.get(i) == Some(&b'.') {
            i += 1;
            if bytes.get(i) == Some(&b'*') {
                out.push(Arg::I(unsafe { ap.arg::<core::ffi::c_int>() } as i64));
                i += 1;
            } else {
                while bytes.get(i).is_some_and(u8::is_ascii_digit) {
                    i += 1;
                }
            }
        }
        let mut length = 0;
        if bytes.get(i) == Some(&b'l') {
            length = 1;
            i += 1;
            if bytes.get(i) == Some(&b'l') {
                length = 2;
                i += 1;
            }
        } else if bytes.get(i).is_some_and(|b| b"jzt".contains(b)) {
            length = 2;
            i += 1;
        } else {
            while bytes.get(i) == Some(&b'h') {
                i += 1;
            }
        }
        let Some(&conv) = bytes.get(i) else {
            break;
        };
        i += 1;
        out.push(match conv {
            b'd' | b'i' => Arg::I(unsafe {
                match length {
                    0 => ap.arg::<core::ffi::c_int>() as i64,
                    1 => ap.arg::<core::ffi::c_long>() as i64,
                    _ => ap.arg::<core::ffi::c_longlong>() as i64,
                }
            }),
            b'u' | b'o' | b'x' | b'X' => Arg::U(unsafe {
                match length {
                    0 => ap.arg::<core::ffi::c_uint>() as u64,
                    1 => ap.arg::<core::ffi::c_ulong>() as u64,
                    _ => ap.arg::<core::ffi::c_ulonglong>() as u64,
                }
            }),
            b'e' | b'E' | b'f' | b'F' | b'g' | b'G' | b'a' | b'A' => {
                Arg::F(unsafe { ap.arg::<f64>() })
            }
            b's' => Arg::S(unsafe { ap.arg::<*const core::ffi::c_char>() }),
            b'c' => Arg::C(unsafe { ap.arg::<core::ffi::c_int>() }),
            b'p' => Arg::P(unsafe { ap.arg::<*const core::ffi::c_void>() }),
            b'n' => Arg::OutN(unsafe { ap.arg::<*mut core::ffi::c_int>() }),
            _ => return out,
        });
    }
    out
}

