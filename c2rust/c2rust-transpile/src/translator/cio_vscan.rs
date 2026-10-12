// Keep helper bindings separate from the translated C globals.
mod xj_cio_scanargs {
    pub(super) unsafe fn xj_cio_vscanargs<B: xj_cio::ByteView + ?Sized>(
        fmt: &B,
        mut ap: ::core::ffi::VaList,
    ) -> Vec<xj_cio::OutArg<'static>> {
        use xj_cio::OutArg;
        let bytes = xj_cio::__cfmt::c_bytes(xj_cio::ByteView::view_bytes(fmt));
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
            let suppressed = bytes.get(i) == Some(&b'*');
            if suppressed {
                i += 1;
            }
            while bytes.get(i).is_some_and(u8::is_ascii_digit) {
                i += 1;
            }
            let mut length = 0;
            if bytes.get(i) == Some(&b'h') {
                length = -1;
                i += 1;
            } else if bytes.get(i) == Some(&b'l') {
                length = 1;
                i += 1;
                if bytes.get(i) == Some(&b'l') {
                    length = 2;
                    i += 1;
                }
            } else if bytes.get(i).is_some_and(|b| b"jzt".contains(b)) {
                length = 2;
                i += 1;
            }
            let Some(&conv) = bytes.get(i) else {
                break;
            };
            i += 1;
            if conv == b'[' {
                if bytes.get(i) == Some(&b'^') {
                    i += 1;
                }
                if bytes.get(i) == Some(&b']') {
                    i += 1;
                }
                while bytes.get(i).is_some_and(|b| *b != b']') {
                    i += 1;
                }
                i += usize::from(i < bytes.len());
            }
            if suppressed {
                continue;
            }
            out.push(match conv {
                b'd' | b'i' | b'n' => unsafe {
                    match length {
                        -1 => OutArg::Short(ap.arg::<*mut core::ffi::c_short>()),
                        0 => OutArg::Int(ap.arg::<*mut core::ffi::c_int>()),
                        1 => OutArg::Long(ap.arg::<*mut core::ffi::c_long>()),
                        _ => OutArg::LongLong(ap.arg::<*mut core::ffi::c_longlong>()),
                    }
                },
                b'u' | b'o' | b'x' | b'X' => unsafe {
                    match length {
                        0 => OutArg::UInt(ap.arg::<*mut core::ffi::c_uint>()),
                        1 => OutArg::ULong(ap.arg::<*mut core::ffi::c_ulong>()),
                        _ => OutArg::ULongLong(ap.arg::<*mut core::ffi::c_ulonglong>()),
                    }
                },
                b'e' | b'E' | b'f' | b'F' | b'g' | b'G' | b'a' | b'A' => unsafe {
                    if length == 0 {
                        OutArg::Float(ap.arg::<*mut f32>())
                    } else {
                        OutArg::Double(ap.arg::<*mut f64>())
                    }
                },
                b's' | b'c' | b'[' => OutArg::Str(unsafe { ap.arg::<*mut core::ffi::c_char>().cast() }),
                _ => return out,
            });
        }
        out
    }
}
