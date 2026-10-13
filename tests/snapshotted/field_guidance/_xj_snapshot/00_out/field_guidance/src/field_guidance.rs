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
use ::std::process::ExitCode;
#[allow(unused_imports)]
use ::field_guidance as _;
extern "C" {
    fn printf(__format: *const u8, ...) -> ::core::ffi::c_int;
}
#[derive(Copy, Clone)]
#[repr(C)]
pub struct Rec {
    pub name: &'static str,
    pub buf: [u8; 8],
    pub vals: &'static [::core::ffi::c_int],
    pub n: ::core::ffi::c_int,
}
static table: [::core::ffi::c_int; 3] = [
    1 as ::core::ffi::c_int,
    2 as ::core::ffi::c_int,
    3 as ::core::ffi::c_int,
];
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
    return t;
}
unsafe extern "C" fn count_char(mut r: *const Rec, mut c: u8) -> ::core::ffi::c_int {
    let mut k = 0 as ::core::ffi::c_int;
    let mut i = 0 as ::core::ffi::c_int;
    while (&(*r).name)
        .as_bytes()
        .get(i as usize)
        .copied()
        .unwrap_or(0) as u8 as ::core::ffi::c_int
        != '\0' as ::core::ffi::c_int
    {
        if (&(*r).name)
            .as_bytes()
            .get(i as usize)
            .copied()
            .unwrap_or(0) as u8 as ::core::ffi::c_int
            == c as ::core::ffi::c_int
        {
            k += 1;
        }
        i += 1;
    }
    return k;
}
unsafe fn main_0() -> ::core::ffi::c_int {
    let mut r = Rec {
        name: "init",
        buf: [0 as ::core::ffi::c_uchar, 0, 0, 0, 0, 0, 0, 0],
        vals: &table,
        n: 3 as ::core::ffi::c_int,
    };
    let mut p: *mut Rec = &raw mut r;
    if str::is_empty(&(*p).name) {
        return 1 as ::core::ffi::c_int;
    }
    let mut first = (&(*p).name)
        .as_bytes()
        .get(0 as usize)
        .copied()
        .unwrap_or(0) as u8;
    (*p).name = "rewritten";
    r.buf[2usize] = 7 as ::core::ffi::c_uchar;
    (*p).buf[3usize] =
        (r.buf[2usize] as ::core::ffi::c_int + 1 as ::core::ffi::c_int) as ::core::ffi::c_uchar;
    let mut s = sum((&(*p).vals).as_ptr(), (*p).n);
    println!(
        "{:} {:} {:} {:} {:}",
        first as ::core::ffi::c_int as u8 as char,
        (*p).buf[3usize] as ::core::ffi::c_int as core::ffi::c_int,
        s as core::ffi::c_int,
        (*p).vals[1 as usize] as core::ffi::c_int,
        count_char(p, 't' as u8) as core::ffi::c_int
    );
    return 0 as ::core::ffi::c_int;
}
pub fn main() -> ExitCode {
    unsafe { ExitCode::from(main_0() as u8) }
}
