#![allow(
    clippy::missing_safety_doc,
    dead_code,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    unused_assignments,
    unused_mut
)]
#![feature(raw_ref_op)]
#[allow(unused_imports)]
use ::field_guidance as _;
use ::std::process::ExitCode;

#[derive(Copy, Clone)]
#[repr(C)]
pub struct Rec {
    pub name: &'static str,
    pub buf: [u8; 8],
    pub vals: &'static [::core::ffi::c_int],
    pub n: ::core::ffi::c_int,
}
static table: [::core::ffi::c_int; 3] = [1, 2, 3];
unsafe extern "C" fn sum(
    mut xs: *const ::core::ffi::c_int,
    mut n: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut t = 0 as ::core::ffi::c_int;
    let mut i = 0 as ::core::ffi::c_int;
    while i < n {
        t += *xs.offset(i as isize);
        i += 1;
    }
    t
}
unsafe extern "C" fn count_char(mut r: *const Rec, mut c: u8) -> ::core::ffi::c_int {
    let mut k = 0 as ::core::ffi::c_int;
    let mut i = 0 as ::core::ffi::c_int;
    while (*r).name.as_bytes().get(i as usize).copied().unwrap_or(0) as ::core::ffi::c_int
        != '\0' as ::core::ffi::c_int
    {
        if (*r).name.as_bytes().get(i as usize).copied().unwrap_or(0) as ::core::ffi::c_int
            == c as ::core::ffi::c_int
        {
            k += 1;
        }
        i += 1;
    }
    k
}
unsafe fn main_0() -> ::core::ffi::c_int {
    let mut r = Rec {
        name: "init",
        buf: [0 as ::core::ffi::c_uchar, 0, 0, 0, 0, 0, 0, 0],
        vals: &table,
        n: 3,
    };
    let mut p: *mut Rec = &raw mut r;
    if str::is_empty((*p).name) {
        return 1;
    }
    let mut first = (*p).name.as_bytes().first().copied().unwrap_or(0);
    (*p).name = "rewritten";
    r.buf[2] = 7;
    (*p).buf[3] = (r.buf[2] as ::core::ffi::c_int + 1) as ::core::ffi::c_uchar;
    let mut s = sum((*p).vals.as_ptr(), (*p).n);
    println!(
        "{:} {:} {:} {:} {:}",
        first as ::core::ffi::c_int as u8 as char,
        (*p).buf[3] as ::core::ffi::c_int as core::ffi::c_int,
        s as core::ffi::c_int,
        (*p).vals[1] as core::ffi::c_int,
        count_char(p, b't') as core::ffi::c_int
    );
    0
}
pub fn main() -> ExitCode {
    unsafe { ExitCode::from(main_0() as u8) }
}
