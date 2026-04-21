extern "C" {
    fn memcpy(
        dest: *mut ::core::ffi::c_void,
        src: *const ::core::ffi::c_void,
        n: ::core::ffi::c_ulong,
    ) -> *mut ::core::ffi::c_void;
    fn printf(fmt: *const ::core::ffi::c_char, ...) -> ::core::ffi::c_int;
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
    fn strlen(s: *const ::core::ffi::c_char) -> size_t;
    fn memset(
        s: *mut ::core::ffi::c_void,
        c: ::core::ffi::c_int,
        n: ::core::ffi::c_long,
    ) -> *mut ::core::ffi::c_void;
    fn strcspn(
        _: *const ::core::ffi::c_char,
        _: *const ::core::ffi::c_char,
    ) -> ::core::ffi::c_ulong;
    fn isalnum(c: ::core::ffi::c_int) -> ::core::ffi::c_int;
    fn tolower(c: ::core::ffi::c_int) -> ::core::ffi::c_char;
    fn exit(status: ::core::ffi::c_int) -> !;
    fn putchar(c: ::core::ffi::c_int) -> ::core::ffi::c_int;
    static mut extern_int_unguided: ::core::ffi::c_int;
    static extern_int_nonmutbl: ::core::ffi::c_int;
}
pub type size_t = ::core::ffi::c_ulong;
#[derive(Copy, Clone)]
#[repr(C)]
pub struct StructWithMembersA {
    pub uptr: *mut ::core::ffi::c_uchar,
    pub zu8: ::core::ffi::c_uchar,
}
#[derive(Copy, Clone)]
#[repr(C)]
pub union UsedForFloatIntBitcast {
    pub ui: ::core::ffi::c_uint,
    pub f: ::core::ffi::c_float,
}
static static_int_nonmutbl: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
#[no_mangle]
pub unsafe fn use_global_ints() {
    static static_local_nonmutbl: ::core::ffi::c_int = 0 as ::core::ffi::c_int;
    extern_int_unguided =
        5 as ::core::ffi::c_int + extern_int_nonmutbl + static_int_nonmutbl + static_local_nonmutbl;
}
#[no_mangle]
pub unsafe fn print_owned_String(mut ostr: String) {
    println!("{:>}", ostr);
}
#[no_mangle]
pub unsafe fn print_unguided_ptr(mut ptr: *const ::core::ffi::c_char) {
    println!("{:>}", {
        std::ffi::CStr::from_ptr(ptr as *const core::ffi::c_char)
            .to_str()
            .unwrap()
    });
}
#[no_mangle]
pub unsafe fn print_shared_vec_u8(mut rvu8: &Vec<u8>) {
    println!("{:>}", String::from_utf8_lossy(&rvu8));
}
#[no_mangle]
pub unsafe fn print_owned_vec_u8(mut ovu8: Vec<u8>) {
    println!("{:>}", String::from_utf8_lossy(&ovu8));
}
#[no_mangle]
pub unsafe fn sprint_into_mutref_vec_u8(mut xvu8: &mut Vec<u8>) {
    xj_sprintf_Vec_u8(&mut xvu8, Some(24), format!("{:}", 42 as core::ffi::c_int));
    xj_sprintf_Vec_u8(&mut xvu8, None, format!("{:}", 42 as core::ffi::c_int));
}
#[no_mangle]
pub unsafe fn guided_str_init_lit() {
    let ostr: String = String::from("owned String");
    print_owned_String(String::from("ddedd"));
    let mut uptr = b"unguided pointer\0" as *const u8 as *const ::core::ffi::c_char;
}
#[no_mangle]
pub unsafe fn guided_str_init_empty_lit() {
    let mut ostr: String = String::new();
    print_owned_String(String::new());
}
#[no_mangle]
pub unsafe fn guided_array_vec() {
    let mut ovu8: Vec<u8> = vec![
        1 as ::core::ffi::c_uchar,
        2 as ::core::ffi::c_uchar,
        3 as ::core::ffi::c_uchar,
        4 as ::core::ffi::c_uchar,
    ];
    print_owned_vec_u8(ovu8);
}
#[no_mangle]
pub unsafe fn guided_immutable_u8_array_slice_decay_to_ptr() {
    let rsu8: &[u8] = ::core::mem::transmute::<[u8; 1], [::core::ffi::c_uchar; 1]>(*b"\0");
    strlen(rsu8.as_ptr() as *const ::core::ffi::c_char);
}
#[no_mangle]
pub unsafe fn guided_immutable_u8_pointer() {
    let mut rsu8: &[u8] = b"\0" as *const u8 as *const ::core::ffi::c_char;
    strlen(rsu8.as_ptr() as *const ::core::ffi::c_char);
}
#[no_mangle]
pub unsafe fn recognize_call_exit() {
    ::std::process::exit(1 as i32);
}
#[no_mangle]
pub unsafe fn recognize_int_float_bitcast() {
    let mut ui = 0x40490fdb as ::core::ffi::c_int as ::core::ffi::c_uint;
    let mut f = f32::from_bits(ui);
    println!("float f = {:.6}", f as ::core::ffi::c_double as f64);
}
#[no_mangle]
pub unsafe fn guided_static() {
    static mut u8: u8 = 0;
}
#[no_mangle]
pub unsafe fn guided_ret_ostr() -> String {
    return String::new();
}
#[no_mangle]
pub unsafe fn guided_condition_string_null_check_neq(mut ostr: String) -> ::core::ffi::c_int {
    return if true {
        2 as ::core::ffi::c_int
    } else {
        5 as ::core::ffi::c_int
    };
}
#[no_mangle]
pub unsafe fn guided_c_assignment_string_pop(mut ostr: String) {
    ostr.pop();
}
#[no_mangle]
pub unsafe fn guided_c_strlen(mut ostr: String) -> ::core::ffi::c_ulong {
    return ostr.len() as size_t;
}
#[no_mangle]
pub unsafe fn guided_isalnum() -> ::core::ffi::c_int {
    return xj_isalnum('A' as i32);
}
#[no_mangle]
pub unsafe fn guided_tolower() -> ::core::ffi::c_int {
    return xj_tolower('A' as i32) as ::core::ffi::c_int;
}
#[no_mangle]
pub unsafe fn guided_strcspn(mut ostr: String, mut delimiters: String) -> ::core::ffi::c_int {
    return strcspn_str(&ostr, &delimiters) as ::core::ffi::c_int;
}
#[no_mangle]
pub unsafe fn guided_vec_memset_zero_mulsizeof_ty(mut ovu8: Vec<u8>) {
    ovu8[..3].fill(0);
}
#[no_mangle]
pub unsafe fn guided_vec_memset_zero_mulsizeof_deref(mut ovu8: Vec<u8>) {
    ovu8[..3].fill(0);
}
#[no_mangle]
pub unsafe fn guided_mut_ref_neq(mut xstr: &mut str, mut xstr2: &mut str) -> ::core::ffi::c_int {
    return 1 as ::core::ffi::c_int;
}
#[no_mangle]
pub unsafe fn guided_1d_slice(
    mut x: &[::core::ffi::c_int],
    mut index: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    let mut x2: &[::core::ffi::c_int] = &x.as_ptr()[3 as usize..];
    return x.as_ptr()[index as usize];
}
#[no_mangle]
pub unsafe fn guided_2d_slice(
    mut x2d: &[&[::core::ffi::c_int]],
    mut i: ::core::ffi::c_int,
    mut j: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    return x2d.as_ptr()[i as usize][j as usize];
}
#[no_mangle]
pub unsafe fn guided_1d_vec(
    mut x: Vec<::core::ffi::c_int>,
    mut index: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    return x[index as usize];
}
#[no_mangle]
pub unsafe fn guided_2d_vec(
    mut x2d: Vec<Vec<::core::ffi::c_int>>,
    mut i: ::core::ffi::c_int,
    mut j: ::core::ffi::c_int,
) -> ::core::ffi::c_int {
    return x2d[i as usize][j as usize];
}
#[no_mangle]
pub unsafe fn guided_local_int_as_char() {
    let mut unguided = 65 as ::core::ffi::c_char;
    let mut oc: char = 'A';
}
#[no_mangle]
pub unsafe fn takes_shared_str(mut rstr: &str) {}
#[no_mangle]
pub unsafe fn takes_shared_u8(mut ru8: &u8) {}
#[no_mangle]
pub unsafe fn guided_coerce_borrow_arg() {
    let mut ostr: String = guided_ret_ostr();
    takes_shared_str(&ostr);
}
#[no_mangle]
pub unsafe fn unguided_coerce_asref(mut unguided: *mut ::core::ffi::c_uchar) {
    takes_shared_u8(unguided.as_ref().unwrap());
}
#[no_mangle]
pub unsafe fn guided_string_zero_empty() {
    let mut ostr: String = String::new();
}
#[no_mangle]
pub unsafe fn struct_unguided_ptr_with_guided_members(mut ug_ptr: *mut StructWithMembersA) {
    *(*ug_ptr).uptr.offset(0 as isize) = 42 as ::core::ffi::c_uchar;
    (*ug_ptr).zu8 = 43 as ::core::ffi::c_uchar;
}
#[no_mangle]
pub unsafe fn struct_guided_ptr_with_guided_members(mut gm_ptr: &mut StructWithMembersA) {
    *gm_ptr.uptr.offset(0 as isize) = 42 as ::core::ffi::c_uchar;
    gm_ptr.zu8 = 43 as ::core::ffi::c_uchar;
}
unsafe fn __tenjin_bvm_279_7_float_to_unsigned_int(
    mut x: ::core::ffi::c_float,
    mut out: *mut ::core::ffi::c_uint,
) {
    memcpy(
        out as *mut ::core::ffi::c_void,
        &raw mut x as *const ::core::ffi::c_void,
        4 as ::core::ffi::c_ulong,
    );
}
#[no_mangle]
pub unsafe fn guided_union_float_int_bitcast(mut f: ::core::ffi::c_float) -> ::core::ffi::c_uint {
    let mut __tenjin_tmp_in_u: ::core::ffi::c_float = 0.;
    let mut __tenjin_tmp_out_u: ::core::ffi::c_uint = 0;
    let mut u = UsedForFloatIntBitcast { ui: 0 };
    __tenjin_tmp_in_u = f;
    __tenjin_tmp_out_u = __tenjin_tmp_in_u.to_bits() as ::core::ffi::c_uint;
    return __tenjin_tmp_out_u;
}
unsafe fn __tenjin_bvm_279_7_unsigned_int_to_float(
    mut x: ::core::ffi::c_uint,
    mut out: *mut ::core::ffi::c_float,
) {
    memcpy(
        out as *mut ::core::ffi::c_void,
        &raw mut x as *const ::core::ffi::c_void,
        4 as ::core::ffi::c_ulong,
    );
}
#[no_mangle]
pub unsafe fn guided_union_int_float_bitcast(mut ui: ::core::ffi::c_uint) -> ::core::ffi::c_float {
    let mut __tenjin_tmp_in_u: ::core::ffi::c_uint = 0;
    let mut __tenjin_tmp_out_u: ::core::ffi::c_float = 0.;
    let mut u = UsedForFloatIntBitcast { ui: 0 };
    __tenjin_tmp_in_u = ui;
    __tenjin_tmp_out_u = f32::from_bits(__tenjin_tmp_in_u as u32);
    return __tenjin_tmp_out_u;
}
#[no_mangle]
pub unsafe fn unguided_int_putchar(mut c: ::core::ffi::c_int) {
    print!("{:}", c as u8 as char);
}
#[no_mangle]
pub unsafe fn guided_int_putchar(mut oc: char) {
    print!("{:}", oc as u8 as char);
}
#[no_mangle]
pub unsafe fn unguided_char_putchar(mut c: ::core::ffi::c_char) {
    print!("{:}", c as ::core::ffi::c_int as u8 as char);
}
fn xj_sprintf_Vec_u8(dest: &mut Vec<u8>, lim: Option<usize>, val: String) -> usize {
    if lim == Some(0) {
        return 0;
    }
    let bytes = val.as_bytes();
    let to_copy = if let Some(lim) = lim {
        std::cmp::min(lim - 1, bytes.len())
    } else {
        bytes.len()
    };
    dest.clear();
    dest.extend_from_slice(&bytes[..to_copy]);
    to_copy
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
fn strcspn_str(s: &str, chars: &str) -> usize {
    s.chars().take_while(|c| !chars.contains(*c)).count()
}
