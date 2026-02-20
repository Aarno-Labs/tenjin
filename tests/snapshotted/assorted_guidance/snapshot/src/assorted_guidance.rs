extern "C" {

    fn snprintf(
        buf: *mut ::core::ffi::c_char,
        _: ::core::ffi::c_ulong,
        fmt: *const ::core::ffi::c_char,
        ...
    ) -> ::core::ffi::c_int;
    fn sprintf(
        buf: *mut ::core::ffi::c_char,
        fmt: *const ::core::ffi::c_char,
        ...
    ) -> ::core::ffi::c_int;
    fn strlen(s: *const ::core::ffi::c_char) -> ::core::ffi::c_long;
    fn memset(
        s: *mut ::core::ffi::c_void,
        c: ::core::ffi::c_int,
        n: ::core::ffi::c_long,
    ) -> *mut ::core::ffi::c_void;
    fn strcspn(
        _: *const ::core::ffi::c_char,
        _: *const ::core::ffi::c_char,
    ) -> ::core::ffi::c_ulong;

    static mut extern_int_unguided: ::core::ffi::c_int;
    static mut extern_int_nonmutbl: ::core::ffi::c_int;
}
#[derive(Copy, Clone)]
#[repr(C)]
pub struct StructWithMembersA {
    pub uptr: *mut ::core::ffi::c_uchar,
    pub zu8: ::core::ffi::c_uchar,
}
static mut static_int_nonmutbl: ::core::ffi::c_int = 0;
#[no_mangle]
pub unsafe extern "C" fn use_global_ints() {
    static mut static_local_nonmutbl: ::core::ffi::c_int = 0;
    extern_int_unguided = 5 + extern_int_nonmutbl + static_int_nonmutbl + static_local_nonmutbl;
}
#[no_mangle]
pub unsafe extern "C" fn print_owned_String(mut ostr: *const ::core::ffi::c_char) {
    println!("{:>}", {
        std::ffi::CStr::from_ptr(ostr as *const core::ffi::c_char)
            .to_str()
            .unwrap()
    });
}
#[no_mangle]
pub unsafe extern "C" fn print_unguided_ptr(mut ptr: *const ::core::ffi::c_char) {
    println!("{:>}", {
        std::ffi::CStr::from_ptr(ptr as *const core::ffi::c_char)
            .to_str()
            .unwrap()
    });
}
#[no_mangle]
pub unsafe extern "C" fn print_shared_vec_u8(mut rvu8: *const ::core::ffi::c_char) {
    println!("{:>}", {
        std::ffi::CStr::from_ptr(rvu8 as *const core::ffi::c_char)
            .to_str()
            .unwrap()
    });
}
#[no_mangle]
pub unsafe extern "C" fn print_owned_vec_u8(mut ovu8: *const ::core::ffi::c_uchar) {
    println!("{:>}", {
        std::ffi::CStr::from_ptr(ovu8 as *const core::ffi::c_char)
            .to_str()
            .unwrap()
    });
}
#[no_mangle]
pub unsafe extern "C" fn sprint_into_mutref_vec_u8(mut xvu8: *mut ::core::ffi::c_char) {
    snprintf(
        xvu8,
        24,
        b"%d\n\0" as *const u8 as *const ::core::ffi::c_char,
        42 as ::core::ffi::c_int,
    );
    sprintf(
        xvu8,
        b"%d\n\0" as *const u8 as *const ::core::ffi::c_char,
        42 as ::core::ffi::c_int,
    );
}
#[no_mangle]
pub unsafe extern "C" fn guided_str_init_lit() {
    let mut ostr = b"owned String\0" as *const u8 as *const ::core::ffi::c_char;
    print_owned_String(b"ddedd\0" as *const u8 as *const ::core::ffi::c_char);
    let mut uptr = b"unguided pointer\0" as *const u8 as *const ::core::ffi::c_char;
}
#[no_mangle]
pub unsafe extern "C" fn guided_str_init_empty_lit() {
    let mut ostr: [::core::ffi::c_char; 1] =
        ::core::mem::transmute::<[u8; 1], [::core::ffi::c_char; 1]>(*b"\0");
    print_owned_String(b"\0" as *const u8 as *const ::core::ffi::c_char);
}
#[no_mangle]
pub unsafe extern "C" fn guided_array_vec() {
    let mut ovu8: [::core::ffi::c_uchar; 4] = [1, 2, 3, 4];
    print_owned_vec_u8(ovu8.as_mut_ptr());
}
#[no_mangle]
pub extern "C" fn recognize_call_exit() {
    ::std::process::exit(1);
}
#[no_mangle]
pub extern "C" fn recognize_int_float_bitcast() {
    let mut ui = 0x40490fdb as ::core::ffi::c_int as ::core::ffi::c_uint;
    let mut f = f32::from_bits(ui);
    println!("float f = {:}", { f as ::core::ffi::c_double });
}
#[no_mangle]
pub extern "C" fn guided_static() {}
#[no_mangle]
pub extern "C" fn guided_ret_ostr() -> *const ::core::ffi::c_char {
    b"\0" as *const u8 as *const ::core::ffi::c_char
}
#[no_mangle]
pub extern "C" fn guided_condition_string_null_check_neq(
    mut ostr: *const ::core::ffi::c_char,
) -> ::core::ffi::c_int {
    if !ostr.is_null() {
        2
    } else {
        5
    }
}
#[no_mangle]
pub unsafe extern "C" fn guided_c_assignment_string_pop(mut ostr: *mut ::core::ffi::c_char) {
    *ostr.offset((strlen(ostr) - 1) as isize) = '\0' as ::core::ffi::c_char;
}
#[no_mangle]
pub unsafe extern "C" fn guided_c_strlen(
    mut ostr: *mut ::core::ffi::c_char,
) -> ::core::ffi::c_ulong {
    strlen(ostr) as ::core::ffi::c_ulong
}
#[no_mangle]
pub extern "C" fn guided_isalnum() -> ::core::ffi::c_int {
    xj_isalnum('A' as i32)
}
#[no_mangle]
pub extern "C" fn guided_tolower() -> ::core::ffi::c_int {
    xj_tolower('A' as i32) as ::core::ffi::c_int
}
#[no_mangle]
pub unsafe extern "C" fn guided_strcspn(
    mut ostr: *const ::core::ffi::c_char,
    mut delimiters: *const ::core::ffi::c_char,
) -> ::core::ffi::c_int {
    strcspn(ostr, delimiters) as ::core::ffi::c_int
}
#[no_mangle]
pub unsafe extern "C" fn guided_vec_memset_zero_mulsizeof_ty(mut ovu8: *mut ::core::ffi::c_char) {
    memset(
        ovu8 as *mut ::core::ffi::c_void,
        0,
        ::core::mem::size_of::<::core::ffi::c_char>().wrapping_mul(3) as ::core::ffi::c_long,
    );
}
#[no_mangle]
pub unsafe extern "C" fn guided_vec_memset_zero_mulsizeof_deref(
    mut ovu8: *mut ::core::ffi::c_char,
) {
    memset(
        ovu8 as *mut ::core::ffi::c_void,
        0,
        ::core::mem::size_of::<::core::ffi::c_char>().wrapping_mul(3) as ::core::ffi::c_long,
    );
}
#[no_mangle]
pub extern "C" fn guided_mut_ref_neq(
    mut xstr: *const ::core::ffi::c_char,
    mut xstr2: *const ::core::ffi::c_char,
) -> ::core::ffi::c_int {
    (xstr != xstr2) as ::core::ffi::c_int
}
#[no_mangle]
pub unsafe extern "C" fn guided_1d_slice(
    mut x: *mut ::core::ffi::c_int,
    mut index: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut x2 = x.offset(3_isize);
    *x.offset(index as isize)
}
#[no_mangle]
pub unsafe extern "C" fn guided_2d_slice(
    mut x2d: *mut *mut ::core::ffi::c_int,
    mut i: ::core::ffi::c_int,
    mut j: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    *(*x2d.offset(i as isize)).offset(j as isize)
}
#[no_mangle]
pub unsafe extern "C" fn guided_1d_vec(
    mut x: *mut ::core::ffi::c_int,
    mut index: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    *x.offset(index as isize)
}
#[no_mangle]
pub unsafe extern "C" fn guided_2d_vec(
    mut x2d: *mut *mut ::core::ffi::c_int,
    mut i: ::core::ffi::c_int,
    mut j: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    *(*x2d.offset(i as isize)).offset(j as isize)
}
#[no_mangle]
pub extern "C" fn guided_local_int_as_char() {
    let mut unguided = 65 as ::core::ffi::c_char;
    let mut oc = 65 as ::core::ffi::c_char;
}
#[no_mangle]
pub extern "C" fn takes_shared_str(mut rstr: *const ::core::ffi::c_char) {}
#[no_mangle]
pub extern "C" fn takes_shared_u8(mut ru8: *mut ::core::ffi::c_uchar) {}
#[no_mangle]
pub extern "C" fn guided_coerce_borrow_arg() {
    let mut ostr = guided_ret_ostr();
    takes_shared_str(ostr);
}
#[no_mangle]
pub extern "C" fn unguided_coerce_asref(mut unguided: *mut ::core::ffi::c_uchar) {
    takes_shared_u8(unguided);
}
#[no_mangle]
pub extern "C" fn guided_string_zero_empty() {
    let mut ostr = ::core::ptr::null::<::core::ffi::c_char>();
}
#[no_mangle]
pub unsafe extern "C" fn struct_unguided_ptr_with_guided_members(
    mut ug_ptr: *mut StructWithMembersA,
) {
    *(*ug_ptr).uptr.offset(0) = 42;
    (*ug_ptr).zu8 = 43;
}
#[no_mangle]
pub unsafe extern "C" fn struct_guided_ptr_with_guided_members(
    mut gm_ptr: *mut StructWithMembersA,
) {
    *(*gm_ptr).uptr.offset(0) = 42;
    (*gm_ptr).zu8 = 43;
}
fn xj_isalnum(c: core::ffi::c_int) -> core::ffi::c_int {
    if c == -1 {
        0
    } else {
        let c = c as u8 as char;
        (c.is_ascii_alphanumeric()) as core::ffi::c_int
    }
}
fn xj_tolower(c: core::ffi::c_int) -> core::ffi::c_int {
    if c == -1 {
        -1
    } else {
        (c as u8 as char).to_ascii_lowercase() as core::ffi::c_int
    }
}
