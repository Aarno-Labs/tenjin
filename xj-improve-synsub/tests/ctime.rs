use xj_improve_synsub::{Depth, Rewriter};

fn rewrite(code: &str, lift: bool) -> String {
    let mut rw = Rewriter::new();
    rw.enable_ctime(lift);
    let mut file = syn::parse_file(code).unwrap();
    rw.rewrite_file(&mut file, Depth::Unlimited);
    prettyplease::unparse(&file)
}
const RECORD: &str = r#"
    #[repr(C)] #[derive(Copy, Clone)] pub struct tm {
        pub tm_sec: core::ffi::c_int, pub tm_min: core::ffi::c_int,
        pub tm_hour: core::ffi::c_int, pub tm_mday: core::ffi::c_int,
        pub tm_mon: core::ffi::c_int, pub tm_year: core::ffi::c_int,
        pub tm_wday: core::ffi::c_int, pub tm_yday: core::ffi::c_int,
        pub tm_isdst: core::ffi::c_int, pub __tm_gmtoff: core::ffi::c_long,
        pub __tm_zone: *const core::ffi::c_char,
    }
    #[repr(C)] pub struct timespec { pub tv_sec: i64, pub tv_nsec: i64 }
    type time_t = i64;
"#;
#[test]
fn byte_pointer_mapping_preserves_c2rust_character_storage() {
    for char_ty in ["u8", "i8", "core::ffi::c_char"] {
        let source = format!(
            r#"{}
            extern "C" {{
                fn asctime(t: *const tm) -> *mut {char_ty};
                fn strptime(s: *const {char_ty}, f: *const {char_ty}, t: *mut tm) -> *mut {char_ty};
                static mut tzname: [*mut {char_ty}; 2];
            }}
            unsafe fn demo(t: *mut tm, s: *const {char_ty}) -> *mut {char_ty} {{
                let zone: *const {char_ty} = (*t).__tm_zone;
                let name: *mut {char_ty} = tzname[0];
                let names: [*mut {char_ty}; 2] = tzname;
                strptime(s, s, t);
                asctime(t)
            }}"#,
            RECORD.replace("core::ffi::c_char", char_ty)
        );
        for lift in [false, true] {
            let out = rewrite(&source, lift);
            let ty = char_ty.replace("::", " :: ");
            let tokens = syn::parse_file(&out).unwrap();
            use quote::ToTokens;
            let tokens = tokens.to_token_stream().to_string();
            assert!(tokens.contains(&format!("cast :: < {ty} >")), "{out}");
            if char_ty != "core::ffi::c_char" {
                assert!(!out.contains("::core::ffi::c_char"), "{out}");
            }
        }
    }
}
#[test]
fn type_mapping_and_raw_renames_are_compositional() {
    let source = format!(
        r#"{RECORD}
        extern "C" {{ fn localtime(t: *const time_t) -> *mut tm; fn time(t: *mut time_t) -> time_t; }}
        struct embedded {{ times: [tm; 2], nanos: timespec }}
        unsafe fn demo(t: *mut time_t, p: *mut tm) {{
            let value = localtime(t); time(t);
            (*p).__tm_gmtoff += 1;
            (*p).__tm_zone = core::ptr::null();
            (*p).__tm_zone = b"UTC\0".as_ptr().cast();
            let zone: *const core::ffi::c_char = (*p).__tm_zone;
            let size = core::mem::size_of::<tm>();
        }}"#
    );
    let out = rewrite(&source, false);
    assert!(out.contains("pub type tm = ::xj_ctime::Tm;"), "{out}");
    assert!(out.contains("pub type timespec = ::xj_ctime::Timespec;"));
    assert!(out.contains("::xj_ctime::c_abi::localtime(t)"));
    assert!(out.contains("::xj_ctime::c_abi::time(t)"));
    assert!(!out.contains("fn localtime("));
    assert!(out.contains(".tm_gmtoff += 1"));
    assert!(out.contains("::xj_ctime::TzAbbr::NULL"));
    assert!(out.contains("::xj_ctime::TzAbbr::new("));
    assert!(out.contains(".cast::<core::ffi::c_char>()"), "{out}");
}
#[test]
fn mixed_arguments_lift_without_dereferencing_unknown_pointers() {
    let out = rewrite(
        r#"
        unsafe fn demo(mut t: i64, mut tm: xj_ctime::Tm, out: *mut xj_ctime::Tm, raw: *const i64, mut ts: xj_ctime::Timespec) {
            xj_ctime::c_abi::localtime_r(&raw const t, out);
            xj_ctime::c_abi::gmtime_r(raw, &raw mut tm);
            xj_ctime::c_abi::localtime_r(&raw const t, &raw mut tm);
            xj_ctime::c_abi::time(core::ptr::null_mut());
            xj_ctime::c_abi::time(&raw mut t);
            xj_ctime::c_abi::time(out as *mut i64);
            xj_ctime::c_abi::nanosleep(&raw const ts, core::ptr::null_mut());
        }
    "#,
        true,
    );
    assert!(
        out.contains("::xj_ctime::c::localtime_r_raw(&t, out)"),
        "{out}"
    );
    assert!(out.contains("::xj_ctime::c::gmtime_r_raw(raw, &mut tm)"));
    assert!(out.contains("::xj_ctime::c::localtime_r(&t, &mut tm)"));
    assert!(out.contains("::xj_ctime::c::time(None)"));
    assert!(out.contains("::xj_ctime::c::time(Some(&mut t))"));
    assert!(out.contains("xj_ctime::c_abi::time(out as *mut i64)"));
    assert!(out.contains("::xj_ctime::c::nanosleep(&ts, None)"));
}
#[test]
fn static_results_require_no_escape_and_no_intervening_calls() {
    let source = format!(
        r#"{RECORD}
        extern "C" {{ fn localtime(t: *const time_t) -> *mut tm; fn gmtime(t: *const time_t) -> *mut tm; }}
        unsafe fn good(t: time_t) -> tm {{
            let mut p: *mut tm = localtime(&raw const t);
            if p.is_null() {{ return core::mem::zeroed(); }}
            (*p).tm_year += 1;
            let copy: tm = *p;
            copy
        }}
        unsafe fn escape(t: time_t) -> *mut tm {{ let p = localtime(&raw const t); p }}
        unsafe fn overwritten(t: time_t) {{ let p = localtime(&raw const t); gmtime(&raw const t); let year = (*p).tm_year; }}
        unsafe fn indirect(t: time_t) {{ let p = localtime(&raw const t); unknown(); let year = (*p).tm_year; }}
    "#
    );
    let out = rewrite(&source, true);
    // A call in the null branch is conservative: even that branch is rejected.
    assert!(
        out.contains("let mut p: *mut tm = ::xj_ctime::c_abi::localtime(&raw const t)"),
        "{out}"
    );
    let source = source.replace("return core::mem::zeroed();", "loop {}");
    let out = rewrite(&source, true);
    assert!(
        out.contains("let mut p = ::xj_ctime::c::localtime(&t)"),
        "{out}"
    );
    assert!(out.contains("p.is_none()"));
    assert!(out.contains("p.as_mut().unwrap().tm_year += 1"));
    assert!(out.contains("let copy: tm = p.unwrap()"));
    assert_eq!(
        out.matches("::xj_ctime::c_abi::localtime(&raw const t)")
            .count(),
        3
    );
}
#[test]
fn buffers_fold_only_when_all_other_arguments_lift() {
    let out = rewrite(
        r#"
        unsafe fn demo(tm: &xj_ctime::Tm, raw: *const xj_ctime::Tm, mut b: [u8; 64]) {
            xj_ctime::c_abi::strftime(b.as_mut_ptr(), 12, b"%Y\0".as_ptr(), tm);
            xj_ctime::c_abi::strftime(b.as_mut_ptr(), 12, b"%Y\0".as_ptr(), raw);
        }
    "#,
        true,
    );
    assert!(out.contains("::xj_ctime::c::strftime("), "{out}");
    assert!(out.contains(".limit(12 as usize)"));
    assert!(
        out.contains("::xj_ctime::c::strftime_raw(b.as_mut_ptr(), 12, \"%Y\\0\", raw)"),
        "{out}"
    );
}
#[test]
fn shadowed_names_and_other_modules_remain_unchanged() {
    let code = r#"
        extern "C" { fn time(p: *mut i64) -> i64; }
        unsafe fn f(time: fn(*mut i64)->i64, p: *mut i64) { time(p); other::time(p); }
        fn localtime(p: *const i64) -> *mut u8 { core::ptr::null_mut() }
    "#;
    let out = rewrite(code, true);
    assert!(out.contains("time(p);"));
    assert!(out.contains("other::time(p);"));
    assert!(!out.contains("xj_ctime"));
}

#[test]
fn globals_and_zone_printing_use_interned_names() {
    let source = format!(
        r#"{RECORD}
        extern "C" {{ static mut tzname: [*mut core::ffi::c_char; 2]; static mut timezone: core::ffi::c_long; static mut daylight: core::ffi::c_int; }}
        unsafe fn demo(t: &tm, i: usize) {{
            let p = tzname[i]; let west = timezone; let dst = daylight;
            xj_cio::printf!("%s %s", t.__tm_zone, tzname[i]);
        }}"#
    );
    let abi = rewrite(&source, false);
    assert!(abi.contains("::xj_ctime::c_abi::tzname()"), "{abi}");
    let out = rewrite(&source, true);
    assert!(out.contains("::xj_ctime::c::timezone()"), "{out}");
    assert!(out.contains("::xj_ctime::c::daylight()"));
    assert!(out.contains("::xj_ctime::c::tzname()[i]"));
    assert!(
        out.split_whitespace()
            .collect::<String>()
            .contains("::xj_bytes::ByteView::view_bytes(&(t.tm_zone))"),
        "{out}"
    );
    assert!(!out.contains("static mut tzname"));
    assert_eq!(rewrite(&out, true), out, "time rewrites must be idempotent");
}

#[test]
fn lifted_strptime_uses_the_shared_trait_and_records_its_dependency() {
    let mut rw = Rewriter::new();
    rw.enable_ctime(true);
    let mut file = syn::parse_file(
        r#"
        unsafe fn demo(input: &[u8], mut tm: xj_ctime::Tm) {
            xj_ctime::c_abi::strptime(input.as_ptr(), b"%Y\0".as_ptr(), &raw mut tm);
        }
    "#,
    )
    .unwrap();
    rw.rewrite_file(&mut file, Depth::Unlimited);
    let out = prettyplease::unparse(&file);
    assert!(out.contains("::xj_ctime::c::strptime("), "{out}");
    assert!(
        out.contains("::xj_bytes::ByteView::view_bytes(xj_time_input)"),
        "{out}"
    );
    assert!(rw.deps().contains("xj_bytes"));
}
#[test]
fn unrelated_foreign_signatures_are_not_time_apis() {
    let out = rewrite(
        r#"extern "C" { fn time(p: *mut u8) -> usize; fn localtime(p: *const u8) -> *mut u8; }
        unsafe fn demo(p: *mut u8) { time(p); localtime(p); }"#,
        true,
    );
    assert!(!out.contains("xj_ctime"), "{out}");
}
#[test]
fn static_zone_literals_remain_const_constructible() {
    let source = format!(
        r#"{RECORD}
        const BASE: tm = tm {{ tm_sec:0, tm_min:0, tm_hour:0, tm_mday:1, tm_mon:0,
            tm_year:70, tm_wday:4, tm_yday:0, tm_isdst:0, __tm_gmtoff:0,
            __tm_zone: b"UTC\0".as_ptr() as *const core::ffi::c_char }};"#
    );
    let out = rewrite(&source, true);
    assert!(out.contains("::xj_ctime::TzAbbr::from_raw"), "{out}");
    assert!(!out.contains("::xj_ctime::TzAbbr::new"));
    assert_eq!(rewrite(&out, true), out);
}
#[test]
fn static_text_values_lift_only_for_checked_local_macro_uses() {
    let out = rewrite(
        r#"
        unsafe fn demo(t: i64) {
            let p: *mut core::ffi::c_char = xj_ctime::c_abi::ctime(&raw const t).cast();
            if p.is_null() { return; }
            xj_cio::printf!("date: %s", p);
        }
    "#,
        true,
    );
    assert!(out.contains("let p = ::xj_ctime::c::ctime(&t)"), "{out}");
    assert!(out.contains("p.is_none()"));
    assert!(out.contains("p.as_ref().unwrap().as_bytes_with_nul()"));
}

#[test]
fn static_pointer_captures_and_borrows_keep_the_original_buffer() {
    let source = format!(
        r#"{RECORD}
        extern "C" {{ fn localtime(t: *const time_t) -> *mut tm; }}
        unsafe fn capture(t: time_t) {{ let p = localtime(&raw const t); let f = || (*p).tm_year; }}
        unsafe fn borrowed(t: time_t) {{ let p = localtime(&raw const t); let r = &(*p).tm_year; }}
    "#
    );
    let out = rewrite(&source, true);
    assert_eq!(
        out.matches("::xj_ctime::c_abi::localtime(&raw const t)")
            .count(),
        2,
        "{out}"
    );
}
#[test]
fn inferred_records_still_map_zone_fields() {
    let source = format!(
        r#"{RECORD}
        unsafe fn demo() {{
            let mut value = tm {{ tm_sec:0, tm_min:0, tm_hour:0, tm_mday:1, tm_mon:0,
                tm_year:70, tm_wday:4, tm_yday:0, tm_isdst:0, __tm_gmtoff:0,
                __tm_zone: core::ptr::null() }};
            value.__tm_zone = b"UTC\0".as_ptr().cast();
            let pointer = value.__tm_zone;
        }}"#
    );
    let out = rewrite(&source, true);
    assert!(!out.contains("__tm_zone"), "{out}");
    assert!(out.contains("value.tm_zone = ::xj_ctime::TzAbbr::new"));
    assert_eq!(rewrite(&out, true), out);
}
#[test]
fn a_record_with_a_different_layout_is_preserved() {
    let source = RECORD.replace("pub tm_sec: core::ffi::c_int", "pub tm_sec: u8");
    let out = rewrite(&source, false);
    assert!(out.contains("pub struct tm"), "{out}");
    assert!(!out.contains("pub type tm"));
}

#[test]
fn cli_records_the_release_version() {
    let unique = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let root = std::env::temp_dir().join(format!("synsub-time-{}-{unique}", std::process::id()));
    std::fs::create_dir_all(root.join("src")).unwrap();
    std::fs::write(root.join("Cargo.toml"), "[package]\nname=\"time_fixture\"\nversion=\"0.0.0\"\nedition=\"2021\"\n[dependencies]\nxj_ctime=\"0.1.1\"\n").unwrap();
    std::fs::write(
        root.join("src/lib.rs"),
        format!(
            r#"{RECORD}
        fn demo(t: &tm) {{ xj_cio::printf!("%s", t.__tm_zone); }}
    "#
        ),
    )
    .unwrap();
    let output = std::process::Command::new(env!("CARGO_BIN_EXE_xj-improve-synsub"))
        .arg(&root)
        .arg("--modify-in-place")
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let manifest: toml_edit::DocumentMut = std::fs::read_to_string(root.join("Cargo.toml"))
        .unwrap()
        .parse()
        .unwrap();
    assert_eq!(manifest["dependencies"]["xj_ctime"].as_str(), Some("0.2.0"));
    assert_eq!(manifest["dependencies"]["xj_bytes"].as_str(), Some("0.1.0"));
    std::fs::remove_dir_all(root).unwrap();
}

#[test]
fn zone_fields_on_call_results_and_function_pointer_results_are_mapped() {
    let source = format!(
        r#"{RECORD}
        fn value() -> tm {{ loop {{}} }}
        unsafe fn pointer(p: *mut tm) -> *mut tm {{ p }}
        unsafe fn demo(p: *mut tm, f: Option<unsafe extern "C" fn() -> *mut tm>) {{
            let a=value().__tm_zone;
            let b=(*pointer(p)).__tm_zone;
            let c=(*f.unwrap()()).__tm_zone;
            let offset=value().__tm_gmtoff;
        }}"#
    );
    let out = rewrite(&source, false);
    assert!(!out.contains("__tm_zone"), "{out}");
    assert!(!out.contains("__tm_gmtoff"), "{out}");
    assert_eq!(rewrite(&out, false), out);
}

#[test]
fn union_members_and_macro_arguments_follow_the_shared_tm_mapping() {
    let source = format!(
        r#"{RECORD}
        union Holder {{ value: tm, bits: [u8; 56] }}
        unsafe fn demo(h: Holder, fmt: *const u8) {{
            let zone=h.value.__tm_zone;
            println!("{{:?}}", h.value.__tm_zone);
            xj_cio::printf_raw!(fmt,h.value.__tm_zone);
            let offset=core::mem::offset_of!(tm,__tm_zone);
        }}"#
    );
    let out = rewrite(&source, false);
    assert!(!out.contains("__tm_zone"), "{out}");
    assert!(out.contains("offset_of!(tm, tm_zone)"), "{out}");
}

#[test]
fn tm_arrays_and_pointer_arithmetic_keep_zone_accesses_well_typed() {
    let source = format!(
        r#"{RECORD}
        unsafe fn demo(mut records:[tm;2], p:*mut tm, i:isize, mut v:Vec<tm>) {{
            let a=(*p.offset(i)).__tm_zone;
            let b=(*records.as_mut_ptr().offset(i)).__tm_zone;
            let c=p.as_ref().unwrap().__tm_zone;
            let d=(*(&raw const records[0])).__tm_zone;
            let e=(*v.as_mut_ptr().offset(i)).__tm_zone;
            let f=v[0].__tm_zone;
        }}"#
    );
    let out = rewrite(&source, false);
    assert!(!out.contains("__tm_zone"), "{out}");
    assert_eq!(rewrite(&out, false), out);
}
