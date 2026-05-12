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
