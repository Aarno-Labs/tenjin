use xj_improve_synsub::{Depth, Rewriter};

const CRATES: &[&str] = &[
    "xj_bytes", "xj_cfmt", "xj_zstr", "xj_cio", "xj_cstr", "xj_atone", "xj_scanf", "xj_cstat",
    "xj_ctime", "xj_cmath",
];

fn rewrite(rw: &Rewriter, code: &str) -> String {
    let mut file = syn::parse_file(code).unwrap();
    rw.rewrite_file(&mut file, Depth::Unlimited);
    prettyplease::unparse(&file)
}

#[test]
fn known_reexports_canonicalize_without_changing_the_input() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_byte_view_calls);
    let source = |path: &str| {
        format!(
            r#"fn demo(bytes: &[u8]) {{
                {path}::ByteView::view_bytes(bytes);
                {path}::ByteView::view_bytes(b"hi\0");
                {path}::ByteView::view_bytes(&make_bytes());
            }}"#
        )
    };
    let expected = rewrite(&rw, &source("::xj_bytes"));
    assert!(expected.contains("::xj_bytes::ByteView::view_bytes(bytes)"));
    assert!(expected.contains("::xj_bytes::ByteView::view_bytes(\"hi\\0\")"));
    assert_eq!(expected.matches("make_bytes()").count(), 1);
    for krate in CRATES {
        for prefix in ["", "::"] {
            let rw = {
                let mut rw = Rewriter::new();
                rw.add_expr_rewrite(Rewriter::rewrite_byte_view_calls);
                rw
            };
            let actual = rewrite(&rw, &source(&format!("{prefix}{krate}")));
            assert_eq!(actual, expected, "{prefix}{krate}");
            assert!(rw.deps().contains("xj_bytes"));
            assert_eq!(rewrite(&rw, &actual), actual);
        }
    }
}

#[test]
fn known_reexports_enable_both_string_and_formatting_lifts() {
    // Exercise recognition before canonicalization, so normalizing one pass's
    // output cannot hide a missed spelling in another pass's type inference.
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_strlen_of_slice);
    rw.add_macro_rewrite(Rewriter::rewrite_c_printf_safe);
    for krate in CRATES {
        for prefix in ["", "::"] {
            let out = rewrite(
                &rw,
                &format!(
                    r#"unsafe fn demo(bytes: &[u8]) {{
                        xj_cio::printf_raw!("%s", {prefix}{krate}::ByteView::view_bytes(bytes));
                        strlen({prefix}{krate}::ByteView::view_bytes(bytes).as_ptr());
                    }}"#
                ),
            );
            assert!(out.contains("xj_cio::printf!("), "{out}");
            assert!(out.contains("::xj_zstr::strlen("), "{out}");
            assert!(!out.contains("printf_raw!"), "{out}");
        }
    }
}

#[test]
fn unrelated_or_unresolved_trait_paths_do_not_gain_rewrites() {
    let mut rw = Rewriter::new();
    rw.add_expr_rewrite(Rewriter::rewrite_byte_view_calls);
    rw.add_expr_rewrite(Rewriter::rewrite_strlen_of_slice);
    rw.add_macro_rewrite(Rewriter::rewrite_c_printf_safe);
    for function in [
        "other::ByteView::view_bytes",
        "xj_custom::ByteView::view_bytes",
        "xj_bytes::OtherTrait::view_bytes",
        "ByteView::view_bytes",
        "Bytes::view_bytes",
        "<[u8] as xj_bytes::ByteView>::view_bytes",
    ] {
        let source = format!(
            r#"unsafe fn demo(bytes: &[u8]) {{
                xj_cio::printf_raw!("%s", {function}(bytes));
                strlen({function}(bytes).as_ptr());
            }}"#
        );
        let expected = prettyplease::unparse(&syn::parse_file(&source).unwrap());
        assert_eq!(rewrite(&rw, &source), expected, "{function}");
    }
    assert!(rw.deps().is_empty());
}
