//! Snapshot tests for code rewrites.
//!
//! Each test parses a Rust snippet, applies one or more rewrites via
//! [`Rewriter::rewrite_file`], unparses the result with `prettyplease`,
//! and compares it against an inline `expect_test` snapshot. The snapshot
//! captures any items inserted into the file (e.g. generated `use`
//! statements and helper functions) but not crate-level dependencies
//! recorded via [`Rewriter::add_dep`].
//!
//! Run `cargo test` to verify; run `UPDATE_EXPECT=1 cargo test` to
//! refresh the inline snapshots.

use expect_test::{Expect, expect};
use xj_improve_synsub::{Depth, Rewriter};

fn check(rw: &Rewriter, input: &str, expected: Expect) {
    let mut file = syn::parse_file(input).expect("parsing input snippet");
    rw.rewrite_file(&mut file, Depth::Unlimited);
    expected.assert_eq(&prettyplease::unparse(&file));
}

#[test]
fn errno_reads_and_writes() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_errno_access);
    check(
        &rw,
        r#"unsafe fn demo(value: i32) -> i32 {
            let saved = *__errno_location();
            *__errno_location() = value;
            (*(__errno_location)()) = *__errno_location() + saved;
            *libc::__errno_location() = *::libc::__errno_location();
            println!("{}", *__errno_location());
            *__errno_location()
        }"#,
        expect![[r#"
            unsafe fn demo(value: i32) -> i32 {
                let saved = ::errno::errno().0;
                ::errno::set_errno(::errno::Errno(value));
                ::errno::set_errno(::errno::Errno(::errno::errno().0 + saved));
                ::errno::set_errno(::errno::Errno(::errno::errno().0));
                println!("{}", ::errno::errno().0);
                ::errno::errno().0
            }
        "#]],
    );
    assert_eq!(rw.take_deps(), ["errno".to_owned()].into());
}

#[test]
fn errno_compound_assignment_evaluates_rhs_before_reading() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_errno_access);
    check(
        &rw,
        r#"unsafe fn demo() {
            *__errno_location() += { *__errno_location() = 7; 3 };
            (*__errno_location()) <<= *__errno_location();
        }"#,
        expect![[r#"
            unsafe fn demo() {
                {
                    let xj_errno_rhs = {
                        ::errno::set_errno(::errno::Errno(7));
                        3
                    };
                    let mut xj_errno_value = ::errno::errno().0;
                    xj_errno_value += xj_errno_rhs;
                    ::errno::set_errno(::errno::Errno(xj_errno_value));
                };
                {
                    let xj_errno_rhs = ::errno::errno().0;
                    let mut xj_errno_value = ::errno::errno().0;
                    xj_errno_value <<= xj_errno_rhs;
                    ::errno::set_errno(::errno::Errno(xj_errno_value));
                };
            }
        "#]],
    );
    assert_eq!(rw.take_deps(), ["errno".to_owned()].into());
}

#[test]
fn errno_storage_addresses_and_unrelated_calls_are_preserved() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_errno_access);
    let input = r#"unsafe fn demo(mut other: i32) {
        let _ = __errno_location();
        let _ = &*__errno_location();
        let _ = &mut (*__errno_location());
        let _ = &raw const *__errno_location();
        let _ = &raw mut (*libc::__errno_location());
        (*__errno_location(), other) = (1, 2);
        [other, *__errno_location()] = [1, 2];
        let _ = *other::__errno_location();
        let _ = *__errno_location(1);
        let _ = *__errno_location::<i32>();
    }"#;
    let mut file = syn::parse_file(input).unwrap();
    let original = prettyplease::unparse(&file);
    rw.rewrite_file(&mut file, Depth::Unlimited);
    assert_eq!(prettyplease::unparse(&file), original);
    assert!(rw.deps().is_empty());
}

#[test]
fn nextafter_calls_use_libm() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_nextafter_call);
    check(
        &rw,
        r#"fn demo(a: f64, b: f64, x: f32, y: f32) {
            let _ = nextafter(a, b);
            let _ = nextafterf(x, y);
            let _ = nextafter(nextafter(a, b), b);
            let _ = other::nextafter(a, b);
            let _ = nextafterl(a, b);
            let _ = nextafter(a);
        }"#,
        expect![[r#"
            fn demo(a: f64, b: f64, x: f32, y: f32) {
                let _ = ::libm::nextafter(a, b);
                let _ = ::libm::nextafterf(x, y);
                let _ = ::libm::nextafter(::libm::nextafter(a, b), b);
                let _ = other::nextafter(a, b);
                let _ = nextafterl(a, b);
                let _ = nextafter(a);
            }
        "#]],
    );
    assert_eq!(rw.take_deps(), ["libm".to_owned()].into());
}

#[test]
fn memcmp_float_comparisons_use_bits() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_memcmp_float_comparison);
    check(
        &rw,
        r#"struct Pair {
            first: f64, second: ::core::ffi::c_double,
            ffirst: f32, fsecond: ::core::ffi::c_float,
        }
        fn demo(mut e1: f64, mut e2: ::core::ffi::c_double,
                mut f1: f32, mut f2: ::core::ffi::c_float, pair: &Pair, ptr: *mut f64) {
            let _ = memcmp(&raw mut e1 as *const ::core::ffi::c_void,
                &raw mut e2 as *const ::core::ffi::c_void,
                ::core::mem::size_of::<::core::ffi::c_double>()) != 0 as ::core::ffi::c_int;
            let _ = memcmp(&raw const e1 as *const ::core::ffi::c_void,
                &raw const e2 as *const ::core::ffi::c_void,
                ::core::mem::size_of::<f64>()) == 0;
            let _ = 0 == memcmp(&raw const pair.first as *const ::core::ffi::c_void,
                &raw const pair.second as *const ::core::ffi::c_void,
                std::mem::size_of::<f64>());
            let _ = 0i32 != (memcmp((&raw mut *ptr) as *const ::core::ffi::c_void,
                (&raw mut e1) as *const ::core::ffi::c_void,
                (::core::mem::size_of::<f64>())));
            let local: c_double = e2;
            let _ = memcmp((&raw const (local)) as *const c_void,
                &raw const e1 as *const c_void, core::mem::size_of::<c_double>()) != 0;
            let _ = memcmp(&raw mut f1 as *const ::core::ffi::c_void,
                &raw mut f2 as *const ::core::ffi::c_void,
                ::core::mem::size_of::<::core::ffi::c_float>()) != 0 as ::core::ffi::c_int;
            let _ = memcmp(&raw const pair.ffirst as *const c_void,
                &raw const pair.fsecond as *const c_void,
                core::mem::size_of::<f32>()) == 0;
        }
        unsafe extern "C" fn translated(mut got: ::core::ffi::c_double,
            mut want: ::core::ffi::c_double) {
            if memcmp(&raw mut got as *const ::core::ffi::c_void,
                &raw mut want as *const ::core::ffi::c_void,
                ::core::mem::size_of::<::core::ffi::c_double>()) != 0 as ::core::ffi::c_int
                && !(got.is_nan() && want.is_nan()) {}
        }"#,
        expect![[r#"
            struct Pair {
                first: f64,
                second: ::core::ffi::c_double,
                ffirst: f32,
                fsecond: ::core::ffi::c_float,
            }
            fn demo(
                mut e1: f64,
                mut e2: ::core::ffi::c_double,
                mut f1: f32,
                mut f2: ::core::ffi::c_float,
                pair: &Pair,
                ptr: *mut f64,
            ) {
                let _ = e1.to_bits() != e2.to_bits();
                let _ = e1.to_bits() == e2.to_bits();
                let _ = pair.first.to_bits() == pair.second.to_bits();
                let _ = (*ptr).to_bits() != e1.to_bits();
                let local: c_double = e2;
                let _ = local.to_bits() != e1.to_bits();
                let _ = f1.to_bits() != f2.to_bits();
                let _ = pair.ffirst.to_bits() == pair.fsecond.to_bits();
            }
            unsafe extern "C" fn translated(
                mut got: ::core::ffi::c_double,
                mut want: ::core::ffi::c_double,
            ) {
                if got.to_bits() != want.to_bits() && !(got.is_nan() && want.is_nan()) {}
            }
        "#]],
    );
}

#[test]
fn memcmp_float_comparisons_leave_other_operations_unchanged() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_memcmp_float_comparison);
    let left = "&raw mut e1 as *const ::core::ffi::c_void";
    let right = "&raw mut e2 as *const ::core::ffi::c_void";
    let size = "::core::mem::size_of::<::core::ffi::c_double>()";
    let comparisons = [
        format!("memcmp({left}, {right}, {size}) < 0"),
        format!("memcmp({left}, {right}, {size}) > 0"),
        format!("memcmp({left}, {right}, {size}) <= 0"),
        format!("memcmp({left}, {right}, {size}) >= 0"),
        format!("memcmp({left}, {right}, {size}) != 1"),
        format!("memcmp({left}, {right}, {size}) == -1"),
        format!("memcmp({left}, {right}, {size})"),
        format!("memcmp({left}, {right}, 4) != 0"),
        format!("memcmp({left}, {right}, {size} / 2) != 0"),
        format!("memcmp({left}, {right}, core::mem::size_of::<f32>()) != 0"),
        format!(
            "memcmp(&raw mut small as *const c_void, &raw mut small as *const c_void, {size}) != 0"
        ),
        format!("memcmp({left}, {right}, core::mem::size_of::<u64>()) != 0"),
        format!("memcmp({left}, {right}, other::mem::size_of::<f64>()) != 0"),
        format!("memcmp({left}, {right}, core::mem::size_of::<f64>(1)) != 0"),
        format!("memcmp({left}, {right}) != 0"),
        format!("memcmp({left}, {right}, {size}, 1) != 0"),
        format!("other_function({left}, {right}, {size}) != 0"),
        format!("memcmp(pointer, {right}, {size}) != 0"),
        format!("memcmp(&raw mut e1 as *const u8, {right}, {size}) != 0"),
        format!("memcmp({left}, &raw mut small as *const c_void, {size}) != 0"),
        format!(
            "memcmp(&raw mut small as *const c_void, {right}, core::mem::size_of::<f32>()) != 0"
        ),
        format!("memcmp({left}, {right}, {size}) != 0 as f64"),
        format!("memcmp(&raw mut integer as *const c_void, {right}, {size}) != 0"),
        format!("memcmp({left}, &raw mut unknown as *const c_void, {size}) != 0"),
        format!("{{ let e1 = unknown; memcmp({left}, {right}, {size}) != 0 }}"),
    ];
    for comparison in comparisons {
        let input = format!(
            "fn demo(mut e1: f64, mut e2: f64, mut small: f32, mut integer: u64, \
                pointer: *const core::ffi::c_void) {{ let _ = {comparison}; }}"
        );
        let mut file = syn::parse_file(&input).unwrap();
        let original = prettyplease::unparse(&file);
        rw.rewrite_file(&mut file, Depth::Unlimited);
        assert_eq!(prettyplease::unparse(&file), original, "{comparison}");
    }
}

#[test]
fn atomic_local_initialization_and_intrinsics() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_intrinsic);
    rw.add_stmt_rewrite(Rewriter::rewrite_local);
    check(
        &rw,
        r#"fn demo() {
            let mut value: ::core::sync::atomic::AtomicI32 = 5 as i32;
            ::core::intrinsics::atomic_store_seqcst(&raw mut value, 10);
            let _ = ::core::intrinsics::atomic_load_acquire(&raw mut value);
            let _ = ::core::intrinsics::atomic_xadd_relaxed(&raw mut value, 3);
            let _ = ::core::intrinsics::atomic_xchg_seqcst(&raw mut value, 42);
        }"#,
        expect![[r#"
            fn demo() {
                let mut value: ::core::sync::atomic::AtomicI32 = ::core::sync::atomic::AtomicI32::new(
                    5 as i32,
                );
                value.store(10, ::core::sync::atomic::Ordering::SeqCst);
                let _ = value.load(::core::sync::atomic::Ordering::Acquire);
                let _ = value.fetch_add(3, ::core::sync::atomic::Ordering::Relaxed);
                let _ = value.swap(42, ::core::sync::atomic::Ordering::SeqCst);
            }
        "#]],
    );
}

#[test]
fn atomic_operations_accept_const_raw_addresses_cast_to_mutable_pointers() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_initialization);
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_intrinsic);
    check(
        &rw,
        r#"static value: ::core::sync::atomic::AtomicI32 =
            ::core::sync::atomic::AtomicI32::new(0);
        fn demo() {
            *(&raw const value as *mut ::core::sync::atomic::AtomicI32) = 5;
            ::core::intrinsics::atomic_store_seqcst(
                &raw const value as *mut ::core::sync::atomic::AtomicI32,
                10,
            );
            let _ = ::core::intrinsics::atomic_load_acquire(
                &raw const value as *mut ::core::sync::atomic::AtomicI32,
            );
        }"#,
        expect![[r#"
            static value: ::core::sync::atomic::AtomicI32 = ::core::sync::atomic::AtomicI32::new(0);
            fn demo() {
                value.store(5, ::core::sync::atomic::Ordering::SeqCst);
                value.store(10, ::core::sync::atomic::Ordering::SeqCst);
                let _ = value.load(::core::sync::atomic::Ordering::Acquire);
            }
        "#]],
    );
}

#[test]
fn atomic_intrinsics_preserve_c_pointer_pointee_result_type() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_intrinsic);
    check(
        &rw,
        r#"pub type __tenjin_atomic_usize_t = ::core::ffi::c_ulong;
        static total: __tenjin_atomic_usize_t = 0;
        fn demo(lines: *mut ::core::ffi::c_ulong) {
            unsafe {
                *lines = ::core::intrinsics::atomic_load_seqcst(
                    &raw const total as *mut __tenjin_atomic_usize_t,
                );
                let _ = ::core::intrinsics::atomic_xadd_relaxed(
                    &raw const total as *mut __tenjin_atomic_usize_t,
                    1 as ::core::ffi::c_ulong,
                );
                ::core::intrinsics::atomic_store_release(
                    &raw const total as *mut __tenjin_atomic_usize_t,
                    2 as ::core::ffi::c_ulong,
                );
            }
        }"#,
        expect![[r#"
            pub type __tenjin_atomic_usize_t = ::core::ffi::c_ulong;
            static total: ::core::sync::atomic::AtomicUsize = ::core::sync::atomic::AtomicUsize::new(
                (0) as usize,
            );
            fn demo(lines: *mut ::core::ffi::c_ulong) {
                unsafe {
                    *lines = (total.load(::core::sync::atomic::Ordering::SeqCst))
                        as __tenjin_atomic_usize_t;
                    let _ = (total
                        .fetch_add(
                            (1 as ::core::ffi::c_ulong) as usize,
                            ::core::sync::atomic::Ordering::Relaxed,
                        )) as __tenjin_atomic_usize_t;
                    total
                        .store(
                            (2 as ::core::ffi::c_ulong) as usize,
                            ::core::sync::atomic::Ordering::Release,
                        );
                }
            }
        "#]],
    );
}

#[test]
fn atomic_struct_initialization_and_intrinsics() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_initialization);
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_intrinsic);
    check(
        &rw,
        r#"#[derive(Copy, Clone)]
        struct Globals {
            initialized: atomic_int,
            stored: atomic_int,
        }
        fn demo(xjg: &mut Globals) {
            *&raw mut xjg.initialized = 5;
            ::core::intrinsics::atomic_store_release(&raw mut xjg.stored, 10);
        }
        fn make() -> Globals { Globals { initialized: 0, stored: 0 } }"#,
        expect![[r#"
            struct Globals {
                initialized: ::core::sync::atomic::AtomicI32,
                stored: ::core::sync::atomic::AtomicI32,
            }
            fn demo(xjg: &mut Globals) {
                xjg.initialized.store(5, ::core::sync::atomic::Ordering::SeqCst);
                xjg.stored.store(10, ::core::sync::atomic::Ordering::Release);
            }
            fn make() -> Globals {
                Globals {
                    initialized: ::core::sync::atomic::AtomicI32::new(0),
                    stored: ::core::sync::atomic::AtomicI32::new(0),
                }
            }
        "#]],
    );
}

#[test]
fn atomic_intrinsics_nested_in_array_subscripts() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_intrinsic);
    rw.add_expr_rewrite(Rewriter::rewrite_usize_array_subscript_literal);
    rw.add_expr_rewrite(Rewriter::rewrite_decayed_array_redundant_borrow);
    check(
        &rw,
        r#"static index: ::core::sync::atomic::AtomicUsize =
            ::core::sync::atomic::AtomicUsize::new(0);
        fn demo(values: &mut [[i32; 1]; 2]) {
            values[::core::intrinsics::atomic_load_seqcst(&raw mut index)][0usize] = 1;
            (&values)[::core::intrinsics::atomic_load_seqcst(&raw mut index)][0usize] = 2;
        }"#,
        expect![[r#"
            static index: ::core::sync::atomic::AtomicUsize = ::core::sync::atomic::AtomicUsize::new(
                0,
            );
            fn demo(values: &mut [[i32; 1]; 2]) {
                values[index.load(::core::sync::atomic::Ordering::SeqCst)][0] = 1;
                values[index.load(::core::sync::atomic::Ordering::SeqCst)][0] = 2;
            }
        "#]],
    );
}

#[test]
fn atomic_intrinsic_nested_in_cstr_if_rewrite() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_intrinsic);
    rw.add_expr_rewrite(Rewriter::rewrite_cstr_ctor_over_if);
    check(
        &rw,
        r#"static enabled: ::core::sync::atomic::AtomicI32 =
            ::core::sync::atomic::AtomicI32::new(0);
        fn demo() {
            xj_str_from_ptr((if ::core::intrinsics::atomic_load_seqcst(&raw mut enabled) != 0 {
                b"ON\0".as_ptr() as *const ::core::ffi::c_char
            } else {
                b"OFF\0".as_ptr() as *const ::core::ffi::c_char
            }) as *const core::ffi::c_char);
        }"#,
        expect![[r#"
            static enabled: ::core::sync::atomic::AtomicI32 = ::core::sync::atomic::AtomicI32::new(
                0,
            );
            fn demo() {
                if enabled.load(::core::sync::atomic::Ordering::SeqCst) != 0 { "ON" } else { "OFF" };
            }
        "#]],
    );
}

#[test]
fn assignment_to_immutable_atomic_global_becomes_store() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_initialization);
    check(
        &rw,
        r#"static global_initialized: ::core::sync::atomic::AtomicI32 =
            ::core::sync::atomic::AtomicI32::new(0);
        fn demo() {
            *&raw mut global_initialized = 5 as ::core::ffi::c_int;
        }"#,
        expect![[r#"
            static global_initialized: ::core::sync::atomic::AtomicI32 = ::core::sync::atomic::AtomicI32::new(
                0,
            );
            fn demo() {
                global_initialized
                    .store(5 as ::core::ffi::c_int, ::core::sync::atomic::Ordering::SeqCst);
            }
        "#]],
    );
}

#[test]
fn all_rust_atomic_types_get_constructors_and_method_calls() {
    let cases = [
        ("AtomicBool", "false", "false", "fetch_and"),
        ("AtomicI8", "0i8", "1i8", "fetch_add"),
        ("AtomicI16", "0i16", "1i16", "fetch_add"),
        ("AtomicI32", "0i32", "1i32", "fetch_add"),
        ("AtomicI64", "0i64", "1i64", "fetch_add"),
        ("AtomicI128", "0i128", "1i128", "fetch_add"),
        ("AtomicIsize", "0isize", "1isize", "fetch_add"),
        ("AtomicU8", "0u8", "1u8", "fetch_add"),
        ("AtomicU16", "0u16", "1u16", "fetch_add"),
        ("AtomicU32", "0u32", "1u32", "fetch_add"),
        ("AtomicU64", "0u64", "1u64", "fetch_add"),
        ("AtomicU128", "0u128", "1u128", "fetch_add"),
        ("AtomicUsize", "0usize", "1usize", "fetch_add"),
    ];

    for (atomic_type, initial, operand, expected_method) in cases {
        let mut rw = Rewriter::new();
        rw.add_expr_rewrite(Rewriter::rewrite_atomic_intrinsic);
        rw.add_stmt_rewrite(Rewriter::rewrite_local);
        let input = format!(
            "fn demo() {{\n\
                 let mut value: ::core::sync::atomic::{atomic_type} = {initial};\n\
                 ::core::intrinsics::atomic_load_acquire(&raw mut value);\n\
                 ::core::intrinsics::atomic_{}_relaxed(&raw mut value, {operand});\n\
             }}",
            if atomic_type == "AtomicBool" {
                "and"
            } else {
                "xadd"
            }
        );
        let mut file = syn::parse_file(&input).expect("parsing atomic snippet");
        rw.rewrite_file(&mut file, Depth::Unlimited);
        let output = prettyplease::unparse(&file);
        let compact_output = output.split_whitespace().collect::<String>();

        assert!(
            compact_output.contains(&format!("{atomic_type}::new(")),
            "missing {atomic_type} constructor in:\n{output}"
        );
        assert!(
            output.contains(&format!("value.{expected_method}")),
            "missing {expected_method} call for {atomic_type} in:\n{output}"
        );
        assert!(!output.contains("::core::intrinsics::atomic_"), "{output}");
    }

    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atomic_intrinsic);
    rw.add_stmt_rewrite(Rewriter::rewrite_local);
    let mut file = syn::parse_file(
        r#"fn demo() {
            let mut value: ::core::sync::atomic::AtomicPtr<u8> = ::core::ptr::null_mut();
            ::core::intrinsics::atomic_load_acquire(&raw mut value);
            ::core::intrinsics::atomic_xadd_relaxed(&raw mut value, 1usize);
        }"#,
    )
    .expect("parsing AtomicPtr snippet");
    rw.rewrite_file(&mut file, Depth::Unlimited);
    let output = prettyplease::unparse(&file);
    let compact_output = output.split_whitespace().collect::<String>();
    assert!(
        compact_output.contains("AtomicPtr::<u8,>::new(::core::ptr::null_mut()"),
        "missing AtomicPtr constructor in:\n{output}"
    );
    assert!(output.contains("value.fetch_ptr_add"));
    assert!(!output.contains("::core::intrinsics::atomic_"));
}

#[test]
fn fixed_representation_c_atomic_typedefs_are_normalized() {
    let rw = Rewriter::new();
    let mut file = syn::parse_file(
        r#"#[derive(Copy, Clone, Debug)]
        struct Atomics {
            bool_value: atomic_bool,
            i8_value: atomic_schar,
            u8_value: atomic_uchar,
            i16_value: atomic_short,
            u16_value: atomic_ushort,
            i32_value: atomic_int,
            u32_value: atomic_uint,
            i64_value: atomic_llong,
            u64_value: atomic_ullong,
            isize_value: atomic_intptr_t,
            usize_value: atomic_size_t,
        }"#,
    )
    .expect("parsing C atomic typedef snippet");
    rw.rewrite_file(&mut file, Depth::Unlimited);
    let output = prettyplease::unparse(&file);

    for atomic_type in [
        "AtomicBool",
        "AtomicI8",
        "AtomicU8",
        "AtomicI16",
        "AtomicU16",
        "AtomicI32",
        "AtomicU32",
        "AtomicI64",
        "AtomicU64",
        "AtomicIsize",
        "AtomicUsize",
    ] {
        assert!(
            output.contains(atomic_type),
            "missing {atomic_type} in:\n{output}"
        );
    }
    assert!(!output.contains("Copy"));
    assert!(!output.contains("Clone"));
    assert!(output.contains("#[derive(Debug)]"));
}

#[test]
fn outer_paren_stripping() {
    let mut rw = Rewriter::new();
    rw.add_stmt_rewrite(Rewriter::rewrite_stmt_outer_parens);
    check(
        &rw,
        "fn demo() -> i32 { ((foo())); (((value))) }",
        expect![[r#"
            fn demo() -> i32 {
                foo();
                value
            }
        "#]],
    );
}

#[test]
fn usize_suffix_is_removed_from_array_subscript_literals() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_usize_array_subscript_literal);
    check(
        &rw,
        "fn demo(a: &[u8]) { let _ = a[0usize]; let _ = a[0_usize]; let _ = a[0xffusize]; }",
        expect![[r#"
            fn demo(a: &[u8]) {
                let _ = a[0];
                let _ = a[0];
                let _ = a[0xff];
            }
        "#]],
    );
}

#[test]
fn usize_suffix_is_kept_outside_direct_array_subscript_literals() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_usize_array_subscript_literal);
    check(
        &rw,
        "fn demo(a: &[u8]) { let n = 0usize; let _ = a[0u32 as usize]; let _ = a[0usize + 1]; }",
        expect![[r#"
            fn demo(a: &[u8]) {
                let n = 0usize;
                let _ = a[0u32 as usize];
                let _ = a[0usize + 1];
            }
        "#]],
    );
}

#[test]
fn null_pointer_offsetof_with_zero_one_or_two_integer_casts() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_null_pointer_offsetof);
    check(
        &rw,
        r#"struct s_t { f1: u8, f2: u32 }
        fn demo() {
            let a = (&raw mut (*::core::ptr::null_mut::<s_t>()).f1 as *mut ::core::ffi::c_char)
                .offset_from(::core::ptr::null_mut::<s_t>() as *mut ::core::ffi::c_char);
            let b = (&raw mut (*::core::ptr::null_mut::<s_t>()).f2 as *mut ::core::ffi::c_char)
                .offset_from(::core::ptr::null_mut::<s_t>() as *mut ::core::ffi::c_char)
                as ::core::ffi::c_uint;
            let c = (&raw mut (*::core::ptr::null_mut::<s_t>()).f1 as *mut ::core::ffi::c_char)
                .offset_from(::core::ptr::null_mut::<s_t>() as *mut ::core::ffi::c_char)
                as ::core::ffi::c_uint as core::ffi::c_uint;
            let d = (&raw mut (*::core::ptr::null_mut::<s_t>()).f2 as *mut ::core::ffi::c_char)
                .offset_from(::core::ptr::null_mut::<s_t>() as *mut ::core::ffi::c_char)
                as u16 as usize;
        }"#,
        expect![[r#"
            struct s_t {
                f1: u8,
                f2: u32,
            }
            fn demo() {
                let a = ::core::mem::offset_of!(s_t, f1);
                let b = ::core::mem::offset_of!(s_t, f2) as ::core::ffi::c_uint;
                let c = ::core::mem::offset_of!(s_t, f1) as ::core::ffi::c_uint;
                let d = ::core::mem::offset_of!(s_t, f2) as u16 as usize;
            }
        "#]],
    );
}

#[test]
fn null_pointer_offsetof_from_expose_provenance() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_null_pointer_offsetof);
    check(
        &rw,
        r#"type size_t = usize;
        struct interface_t { iff_opts: i32 }
        struct para_t { interface: interface_t }
        struct xiosingle_t { howtoend: i32, para: para_t }
        fn demo() {
            let a = (&raw mut (*::core::ptr::null_mut::<xiosingle_t>()).howtoend)
                .expose_provenance();
            let b = (&raw mut (*::core::ptr::null_mut::<xiosingle_t>()).howtoend)
                .expose_provenance() as size_t;
            let c = (&raw mut (*::core::ptr::null_mut::<xiosingle_t>()).howtoend)
                .expose_provenance() as size_t as ::core::ffi::c_long;
            let major = (&raw mut (*::core::ptr::null_mut::<xiosingle_t>())
                .para
                .interface
                .iff_opts)
                .expose_provenance() as size_t as ::core::ffi::c_long;
            let nested_byte_offset = (&raw mut (*::core::ptr::null_mut::<xiosingle_t>())
                .para.interface.iff_opts as *mut ::core::ffi::c_char)
                .offset_from(::core::ptr::null_mut::<xiosingle_t>() as *mut ::core::ffi::c_char)
                as size_t;
        }"#,
        expect![[r#"
            type size_t = usize;
            struct interface_t {
                iff_opts: i32,
            }
            struct para_t {
                interface: interface_t,
            }
            struct xiosingle_t {
                howtoend: i32,
                para: para_t,
            }
            fn demo() {
                let a = ::core::mem::offset_of!(xiosingle_t, howtoend);
                let b = ::core::mem::offset_of!(xiosingle_t, howtoend) as size_t;
                let c = ::core::mem::offset_of!(xiosingle_t, howtoend) as size_t
                    as ::core::ffi::c_long;
                let major = ::core::mem::offset_of!(xiosingle_t, para.interface.iff_opts) as size_t
                    as ::core::ffi::c_long;
                let nested_byte_offset = ::core::mem::offset_of!(
                    xiosingle_t, para.interface.iff_opts
                ) as size_t;
            }
        "#]],
    );
}

#[test]
fn null_pointer_offsetof_rejects_mismatched_types_and_nonbyte_offsets() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_null_pointer_offsetof);
    check(
        &rw,
        r#"struct s_t { f1: u32 }
        struct t_t { f1: u32 }
        fn demo(p: *mut s_t) {
            let a = (&raw mut (*::core::ptr::null_mut::<s_t>()).f1 as *mut ::core::ffi::c_char)
                .offset_from(::core::ptr::null_mut::<t_t>() as *mut ::core::ffi::c_char);
            let b = (&raw mut (*::core::ptr::null_mut::<s_t>()).f1 as *mut ::core::ffi::c_int)
                .offset_from(::core::ptr::null_mut::<s_t>() as *mut ::core::ffi::c_int);
            let c = (&raw mut (*p).f1 as *mut ::core::ffi::c_char)
                .offset_from(::core::ptr::null_mut::<s_t>() as *mut ::core::ffi::c_char);
            let d = (&raw mut (*p).f1).expose_provenance();
        }"#,
        expect![[r#"
            struct s_t {
                f1: u32,
            }
            struct t_t {
                f1: u32,
            }
            fn demo(p: *mut s_t) {
                let a = (&raw mut (*::core::ptr::null_mut::<s_t>()).f1 as *mut ::core::ffi::c_char)
                    .offset_from(::core::ptr::null_mut::<t_t>() as *mut ::core::ffi::c_char);
                let b = (&raw mut (*::core::ptr::null_mut::<s_t>()).f1 as *mut ::core::ffi::c_int)
                    .offset_from(::core::ptr::null_mut::<s_t>() as *mut ::core::ffi::c_int);
                let c = (&raw mut (*p).f1 as *mut ::core::ffi::c_char)
                    .offset_from(::core::ptr::null_mut::<s_t>() as *mut ::core::ffi::c_char);
                let d = (&raw mut (*p).f1).expose_provenance();
            }
        "#]],
    );
}

#[test]
fn string_pop_trailing_nul_on_string() {
    let mut rw = Rewriter::new();
    rw.add_stmt_rewrite(Rewriter::rewrite_string_pop_trailing_nul);
    check(
        &rw,
        "fn demo(s1: String) { *s1.offset(s1.len().wrapping_sub(1 as size_t) as isize) = '\\0' as ::core::ffi::c_char; }",
        expect![[r#"
            fn demo(s1: String) {
                s1.pop();
            }
        "#]],
    );
}

#[test]
fn string_pop_trailing_nul_left_alone_on_byte_slice() {
    let mut rw = Rewriter::new();
    rw.add_stmt_rewrite(Rewriter::rewrite_string_pop_trailing_nul);
    check(
        &rw,
        "fn demo(s1: &[u8]) { *s1.offset(s1.len().wrapping_sub(1 as size_t) as isize) = '\\0' as ::core::ffi::c_char; }",
        expect![[r#"
            fn demo(s1: &[u8]) {
                *s1.offset(s1.len().wrapping_sub(1 as size_t) as isize) = '\0'
                    as ::core::ffi::c_char;
            }
        "#]],
    );
}

#[test]
fn strlen_of_slice_adds_use_item() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_strlen_of_slice);
    check(
        &rw,
        "fn demo(buf: &[u16]) { let _ = strlen(buf.as_mut_ptr()); let _ = strlen(buf.as_mut_ptr()); }",
        expect![[r#"
            use ::xj_cstr::ByteSlice;
            fn demo(buf: &[u16]) {
                let _ = (::std::ffi::CStr::from_bytes_until_nul(buf.as_u8_slice())
                    .unwrap()
                    .count_bytes()) as size_t;
                let _ = (::std::ffi::CStr::from_bytes_until_nul(buf.as_u8_slice())
                    .unwrap()
                    .count_bytes()) as size_t;
            }
        "#]],
    );
}

#[test]
fn strlen_of_u8_array_borrows_slice() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_strlen_of_slice);
    check(
        &rw,
        "fn demo() { let mut buf: [u8; 100] = [0; 100]; let _ = strlen(buf.as_mut_ptr()); }",
        expect![[r#"
            fn demo() {
                let mut buf: [u8; 100] = [0; 100];
                let _ = (::std::ffi::CStr::from_bytes_until_nul(&buf[..]).unwrap().count_bytes())
                    as size_t;
            }
        "#]],
    );
}

#[test]
fn atone_rewrites_all_null_endptr_and_simple_calls() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atone_of_slice);
    let mut file = syn::parse_file(
        r#"fn demo(bytes: &[u8], chars: &[::core::ffi::c_char], text: &str,
                  vector: Vec<i8>, string: String) {
            let _ = atof(bytes.as_ptr());
            let _ = atof(vector.as_ptr());
            let _ = atoi(chars.as_ptr());
            let _ = atoi(string.as_ptr());
            let _ = atoi("17".as_ptr());
            let _ = atol(chars);
            let _ = atol(bytes[1..].as_ptr());
            let _ = atoll(b"42\0" as *const u8 as *const ::core::ffi::c_char);
            let _ = strtod(chars.as_ptr(), ::core::ptr::null_mut());
            let _ = strtof(text, 0 as *mut *mut ::core::ffi::c_char);
            let _ = strtof(text.as_ptr(), ::core::ptr::null_mut());
            let _ = strtol(bytes.as_ptr(), ::std::ptr::null_mut(), 10);
            let _ = strtoul(bytes, ::core::ptr::null_mut(), 16);
            let _ = strtoll(chars, ::core::ptr::null_mut(), 0);
            let _ = strtoull(chars.as_ptr(), ::core::ptr::null_mut(), 8);
        }"#,
    )
    .unwrap();

    rw.rewrite_file(&mut file, Depth::Unlimited);
    let output = prettyplease::unparse(&file);
    for call in [
        "::xj_atone::atof(bytes.as_u8_slice())",
        "::xj_atone::atof(vector.as_u8_slice())",
        "::xj_atone::atoi(chars.as_u8_slice())",
        "::xj_atone::atoi(string.as_bytes())",
        "::xj_atone::atoi(\"17\".as_bytes())",
        "::xj_atone::atol(chars.as_u8_slice())",
        "::xj_atone::atol(bytes[1..].as_u8_slice())",
        "::xj_atone::atoll(b\"42\")",
        "::xj_atone::strtod_n(chars.as_u8_slice())",
        "::xj_atone::strtof_n(text.as_bytes())",
        "::xj_atone::strtol_n(bytes.as_u8_slice(), 10)",
        "::xj_atone::strtoul_n(bytes.as_u8_slice(), 16)",
        "::xj_atone::strtoll_n(chars.as_u8_slice(), 0)",
        "::xj_atone::strtoull_n(chars.as_u8_slice(), 8)",
    ] {
        assert!(output.contains(call), "missing {call} in {output}");
    }
    assert_eq!(
        output
            .matches("::xj_atone::strtof_n(text.as_bytes())")
            .count(),
        2
    );
    assert_eq!(output.matches("use ::xj_cstr::ByteSlice;").count(), 1);
    assert_eq!(
        rw.deps(),
        ["xj_atone", "xj_cstr"].map(str::to_string).into()
    );
}

#[test]
fn atone_adapts_non_null_endptr_and_preserves_raw_pointer_calls() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_atone_of_slice);
    let mut file = syn::parse_file(
        r#"fn demo(input: &[i8], raw: *const i8) {
            let mut end = ::core::ptr::null_mut();
            let _ = strtod(input.as_ptr(), &mut end);
            let _ = strtod(input.as_ptr(), &mut end);
            let _ = strtof(input, &mut end);
            let _ = strtol(input, &mut end, 10);
            let _ = strtoul(input, &mut end, 10);
            let _ = strtoll(input, &mut end, 10);
            let _ = strtoull(input, &mut end, 10);
            let _ = atoi(raw);
            let _ = strtod(raw, ::core::ptr::null_mut());
        }"#,
    )
    .unwrap();

    rw.rewrite_file(&mut file, Depth::Unlimited);
    let output = prettyplease::unparse(&file);
    for function in [
        "strtod", "strtof", "strtol", "strtoul", "strtoll", "strtoull",
    ] {
        assert!(
            output.contains(&format!("fn xj_synsub_{function}_with_endptr(")),
            "missing adapter for {function}: {output}"
        );
        assert!(
            output.contains(&format!(
                "xj_synsub_{function}_with_endptr(input.as_u8_slice()"
            )),
            "missing call for {function}: {output}"
        );
    }
    assert_eq!(
        output.matches("fn xj_synsub_strtod_with_endptr(").count(),
        1
    );
    assert!(output.contains("::xj_atone::strtod_u_e(input, &mut endoff)"));
    assert!(output.contains("::errno::set_errno(error)"));
    assert!(output.contains("::xj_atone::strtol(input, &mut endoff, base)"));
    assert!(output.contains("atoi(raw)"));
    assert!(output.contains("strtod(raw, ::core::ptr::null_mut())"));
    assert_eq!(
        rw.deps(),
        ["errno", "xj_atone", "xj_cstr"].map(str::to_string).into()
    );
}

#[test]
fn getchar_variants_add_use_and_helper_fn() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_getchar_variants);
    check(
        &rw,
        "fn demo() -> i32 { let _ = getchar(); let _ = fgetc(stdin); 0 }",
        expect![[r#"
            use ::std::io::Read;
            fn xj_getchar_i() -> ::core::ffi::c_int {
                std::io::stdin().bytes().next().map_or(-1, |b| b.map_or(-1, |byte| byte as i32))
            }
            fn demo() -> i32 {
                let _ = xj_getchar_i();
                let _ = xj_getchar_i();
                0
            }
        "#]],
    );
}

#[test]
fn isinf_isnan_strips_f64_cast() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_isinf_isnan_comparisons);
    check(
        &rw,
        "fn demo(x: f64) -> bool { xj_isinf(x as f64) != 0 && xj_isnan(x as f64) == 0 }",
        expect![[r#"
            fn demo(x: f64) -> bool {
                x.is_infinite() && !x.is_nan()
            }
        "#]],
    );
}

#[test]
fn isinf_isnan_without_cast() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_isinf_isnan_comparisons);
    check(
        &rw,
        "fn demo(x: f64) -> bool { xj_isinf(x) != 0 && xj_isnan(x) == 0 }",
        expect![[r#"
            fn demo(x: f64) -> bool {
                x.is_infinite() && !x.is_nan()
            }
        "#]],
    );
}

#[test]
fn usleep_to_thread_sleep() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_usleep);
    check(
        &rw,
        "fn demo() { usleep(1000); }",
        expect![[r#"
            fn demo() {
                std::thread::sleep(std::time::Duration::from_micros(1000 as u64));
            }
        "#]],
    );
}

#[test]
fn casted_literal_comparison_strips_outer_casts() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_casted_literal_comparison);
    check(
        &rw,
        "fn demo(x: i32, y: i32) { let _ = (x < 5 as i32) as i64 == (y < 3 as i32) as i64; }",
        expect![[r#"
            fn demo(x: i32, y: i32) {
                let _ = (x < 5 as i32) == (y < 3 as i32);
            }
        "#]],
    );
}

#[test]
fn casted_literal_comparison_skips_non_comparison_outer_op() {
    // Inner ops are comparisons (bool result), but the outer op is bitwise — no rewrite.
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_casted_literal_comparison);
    check(
        &rw,
        "fn demo(x: i32, y: i32) { let _ = (x < 5 as i32) as i64 | (y < 3 as i32) as i64; }",
        expect![[r#"
            fn demo(x: i32, y: i32) {
                let _ = (x < 5 as i32) as i64 | (y < 3 as i32) as i64;
            }
        "#]],
    );
}

#[test]
fn casted_literal_comparison_skips_non_comparison_inner_op() {
    // Outer op is a comparison, but inner ops are arithmetic — no rewrite.
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_casted_literal_comparison);
    check(
        &rw,
        "fn demo(x: i32, y: i32) { let _ = (x + 5 as i32) as i64 == (y < 3 as i32) as i64; }",
        expect![[r#"
            fn demo(x: i32, y: i32) {
                let _ = (x + 5 as i32) as i64 == (y < 3 as i32) as i64;
            }
        "#]],
    );
    check(
        &rw,
        "fn demo(x: i32, b: bool) { let _ = (x & b as i32) as i64 == (0 + b as i32) as i64; }",
        expect![[r#"
            fn demo(x: i32, b: bool) {
                let _ = (x & b as i32) as i64 == (0 + b as i32) as i64;
            }
        "#]],
    );
}

#[test]
fn fgets_is_null_with_raw_addr_buffer() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_fgets_stdin_is_null);
    check(
        &rw,
        r#"fn demo() {
            let mut input: [::core::ffi::c_char; 256] = [0; 256];
            let stdin: i32 = 0;
            fgets(
                &raw mut input as *mut ::core::ffi::c_char,
                256 as ::core::ffi::c_int,
                stdin,
            ).is_null();
        }"#,
        expect![[r#"
            use ::std::io::BufRead;
            use ::xj_cstr::ByteSlice;
            fn fgets_stdin_u8_count(buf: &mut [u8], limit: usize) -> Option<usize> {
                let f = std::io::stdin();
                let mut handle = f.lock();
                let Ok(src) = handle.fill_buf() else {
                    return None;
                };
                if src.is_empty() {
                    return None;
                }
                let n = src
                    .iter()
                    .position(|&b| b == b'\n')
                    .map(|i| i + 1)
                    .unwrap_or(src.len())
                    .min(limit - 1);
                buf[..n].copy_from_slice(&src[..n]);
                buf[n] = 0;
                handle.consume(n);
                Some(n)
            }
            fn demo() {
                let mut input: [::core::ffi::c_char; 256] = [0; 256];
                let stdin: i32 = 0;
                fgets_stdin_u8_count(input.as_mut_u8_slice(), 256 as ::core::ffi::c_int as usize)
                    .is_none();
            }
        "#]],
    );
}

#[test]
fn sscanf_with_raw_addr_buffer() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_scanf_variants);
    check(
        &rw,
        r#"fn demo(mut input: [::core::ffi::c_char; 256], mut choice: ::core::ffi::c_int) -> bool {
            if sscanf(
                &raw mut input as *mut ::core::ffi::c_char,
                b"%d\0".as_ptr() as *const ::core::ffi::c_char,
                &raw mut choice,
            ) != 1 as ::core::ffi::c_int { return false; }
            true
        }"#,
        expect![[r#"
            use ::xj_cstr::ByteSlice;
            fn demo(mut input: [::core::ffi::c_char; 256], mut choice: ::core::ffi::c_int) -> bool {
                if xj_scanf::bscanf!(input.as_u8_slice(), "%d", & mut choice)
                    != 1 as ::core::ffi::c_int
                {
                    return false;
                }
                true
            }
        "#]],
    );
}

#[test]
fn memset_on_cast() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_memset_on_slice_or_array);
    check(
        &rw,
        "fn demo(buf: &mut [u8]) { memset(buf.as_mut_ptr() as quux,
                                                 0 as foo, buf.len() as somelongtype); }",
        expect![[r#"
            use ::xj_cstr::ByteSlice;
            fn demo(buf: &mut [u8]) {
                buf.as_mut_u8_slice()[..buf.len() as somelongtype as usize]
                    .fill((0 as foo).try_into().unwrap());
            }
        "#]],
    );
}

#[test]
fn deref_on_slice() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_decayed_array_deref);
    check(
        &rw,
        "fn test(s: &[u8]) -> u8 { *s.as_ptr() }",
        expect![[r#"
            fn test(s: &[u8]) -> u8 {
                s[0]
            }
        "#]],
    );
    check(
        &rw,
        "fn test(s: &[u8]) -> u8 { *s }",
        expect![[r#"
            fn test(s: &[u8]) -> u8 {
                s[0]
            }
        "#]],
    );
    check(
        &rw,
        "fn test(s: *const u8) -> u8 { *s }",
        expect![[r#"
            fn test(s: *const u8) -> u8 {
                *s
            }
        "#]],
    );
}
