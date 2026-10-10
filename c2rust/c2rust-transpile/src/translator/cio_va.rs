// Toolchain-dependent va_list access; syntax and requirements live in the library.
mod xj_cio_varargs {
    pub(super) unsafe fn xj_cio_vargs(
        plan: &xj_cio::cformat::Plan<'_>,
        mut ap: ::core::ffi::VaList,
    ) -> Vec<xj_cio::Arg<'static>> {
        use xj_cio::{Arg, cformat::{Length, Requirement}};
        plan.requirements.iter().map(|r| {
            match r {
                Requirement::Int => Arg::I(ap.arg::<core::ffi::c_int>() as i64),
                Requirement::UInt | Requirement::WideChar => Arg::U(ap.arg::<core::ffi::c_uint>() as u64),
                Requirement::Long => Arg::I(ap.arg::<core::ffi::c_long>() as i64),
                Requirement::ULong => Arg::U(ap.arg::<core::ffi::c_ulong>() as u64),
                Requirement::LongLong => Arg::I(ap.arg::<core::ffi::c_longlong>() as i64),
                Requirement::ULongLong => Arg::U(ap.arg::<core::ffi::c_ulonglong>() as u64),
                Requirement::IntMax => Arg::I(ap.arg::<xj_cio::__libc::intmax_t>() as i64),
                Requirement::UIntMax => Arg::U(ap.arg::<xj_cio::__libc::uintmax_t>() as u64),
                Requirement::SignedSize => Arg::I(ap.arg::<xj_cio::__libc::ssize_t>() as i64),
                Requirement::PtrDiff => Arg::I(ap.arg::<xj_cio::__libc::ptrdiff_t>() as i64),
                Requirement::Size | Requirement::UnsignedPtrDiff => Arg::U(ap.arg::<usize>() as u64),
                Requirement::Double => Arg::F(ap.arg::<f64>()),
                Requirement::String => Arg::Raw(ap.arg::<*const core::ffi::c_char>().cast_mut().cast()),
                Requirement::Pointer => Arg::Raw(ap.arg::<*mut core::ffi::c_void>()),
                Requirement::Count(length) => Arg::Raw(match length {
                    Length::Hh => ap.arg::<*mut i8>().cast(),
                    Length::H => ap.arg::<*mut core::ffi::c_short>().cast(),
                    Length::None => ap.arg::<*mut core::ffi::c_int>().cast(),
                    Length::L => ap.arg::<*mut core::ffi::c_long>().cast(),
                    Length::Ll => ap.arg::<*mut core::ffi::c_longlong>().cast(),
                    Length::J => ap.arg::<*mut xj_cio::__libc::intmax_t>().cast(),
                    Length::Z => ap.arg::<*mut xj_cio::__libc::ssize_t>().cast(),
                    Length::T => ap.arg::<*mut xj_cio::__libc::ptrdiff_t>().cast(),
                    Length::BigL => unreachable!(),
                }),
            }
        }).collect()
    }
}
