//! Snapshot tests for the relinking analysis and rewrite.
//!
//! Each test constructs an in-memory workspace (one or more crate
//! targets), runs [`relink_workspace`], and compares the unparsed result
//! against an inline `expect_test` snapshot.
//!
//! Run `cargo test` to verify; run `UPDATE_EXPECT=1 cargo test` to refresh
//! the inline snapshots.

use std::path::PathBuf;

use expect_test::{Expect, expect};
use xj_improve_relink::collect::{CrateTarget, CrateUnit, FileUnit};
use xj_improve_relink::relink::{DepRequest, relink_workspace};

/// Build a crate target from `(package, target_name, kinds, src_path)` and
/// a list of `(module_path, source)` files.
fn unit(
    package: &str,
    target_name: &str,
    kinds: &[&str],
    src_path: &str,
    files: &[(&[&str], &str)],
) -> CrateUnit {
    let target = CrateTarget {
        package_name: package.to_string(),
        package_dir: PathBuf::from(format!("/ws/{package}")),
        target_name: target_name.to_string(),
        kinds: kinds.iter().map(|s| s.to_string()).collect(),
        src_path: PathBuf::from(src_path),
    };
    let files = files
        .iter()
        .map(|(modpath, src)| FileUnit {
            path: PathBuf::from(src_path),
            modpath: modpath.iter().map(|s| s.to_string()).collect(),
            ast: syn::parse_file(src).expect("parsing test source"),
        })
        .collect();
    CrateUnit { target, files }
}

/// Run relinking and render every file (and any dependency request) into a
/// single string for snapshotting.
fn run(mut units: Vec<CrateUnit>, expected: Expect) {
    let deps = relink_workspace(&mut units);
    let mut out = String::new();
    for unit in &units {
        for file in &unit.files {
            out.push_str(&format!(
                "// crate `{}` ({}), mod [{}]\n",
                unit.target.target_name,
                unit.target.kinds.join(","),
                file.modpath.join("::")
            ));
            out.push_str(&prettyplease::unparse(&file.ast));
            out.push('\n');
        }
    }
    for DepRequest {
        importer_pkg_dir,
        dep_package,
        dep_pkg_dir,
    } in &deps
    {
        out.push_str(&format!(
            "// dep: {} needs {} (at {})\n",
            importer_pkg_dir.display(),
            dep_package,
            dep_pkg_dir.display()
        ));
    }
    expected.assert_eq(&out);
}

#[test]
fn passthrough_wrapper_rewires_to_underlying_safe_fn() {
    // The `main` example: a bin imports `foo` via `extern "C"`, the lib
    // exports it through an `_xj_ffi` passthrough wrapper.
    let lib = unit(
        "main",
        "main",
        &["lib"],
        "/ws/main/lib.rs",
        &[
            (&[], "pub mod src { pub mod lib; }"),
            (
                &["src", "lib"],
                r#"
                    pub fn foo() -> ::core::ffi::c_int { 0 }
                    pub mod _xj_ffi {
                        #[no_mangle]
                        pub extern "C" fn foo() -> ::core::ffi::c_int { super::foo() }
                    }
                "#,
            ),
        ],
    );
    let bin = unit(
        "main",
        "main",
        &["bin"],
        "/ws/main/src/main.rs",
        &[(
            &[],
            r#"
                extern "C" {
                    fn foo() -> ::core::ffi::c_int;
                }
                unsafe fn main_0() -> ::core::ffi::c_int { foo() }
            "#,
        )],
    );
    run(
        vec![lib, bin],
        expect![[r#"
            // crate `main` (lib), mod []
            pub mod src {
                pub mod lib;
            }

            // crate `main` (lib), mod [src::lib]
            pub fn foo() -> ::core::ffi::c_int {
                0
            }
            pub mod _xj_ffi {
                #[no_mangle]
                pub extern "C" fn foo() -> ::core::ffi::c_int {
                    super::foo()
                }
            }

            // crate `main` (bin), mod []
            unsafe fn main_0() -> ::core::ffi::c_int {
                main::src::lib::foo()
            }

        "#]],
    );
}

#[test]
fn non_passthrough_export_rewires_to_no_mangle_item() {
    // No underlying safe fn / not a passthrough: fall back to the
    // `#[no_mangle]` export item itself.
    let lib = unit(
        "lib",
        "lib",
        &["lib"],
        "/ws/lib/src/lib.rs",
        &[(
            &[],
            r#"
                #[no_mangle]
                pub extern "C" fn bar() -> ::core::ffi::c_int { 42 }
            "#,
        )],
    );
    let bin = unit(
        "app",
        "app",
        &["bin"],
        "/ws/app/src/main.rs",
        &[(
            &[],
            r#"
                extern "C" {
                    fn bar() -> ::core::ffi::c_int;
                }
                unsafe fn use_it() -> ::core::ffi::c_int { bar() }
            "#,
        )],
    );
    run(
        vec![lib, bin],
        expect![[r#"
            // crate `lib` (lib), mod []
            #[no_mangle]
            pub extern "C" fn bar() -> ::core::ffi::c_int {
                42
            }

            // crate `app` (bin), mod []
            unsafe fn use_it() -> ::core::ffi::c_int {
                lib::bar()
            }

            // dep: /ws/app needs lib (at /ws/lib)
        "#]],
    );
}

#[test]
fn same_target_uses_crate_path() {
    // Importer and exporter are the same crate target: use `crate::`.
    let lib = unit(
        "k",
        "k",
        &["lib"],
        "/ws/k/src/lib.rs",
        &[(
            &[],
            r#"
                pub fn baz() {}
                pub mod _xj_ffi {
                    #[no_mangle]
                    pub extern "C" fn baz() { super::baz() }
                }
                extern "C" {
                    fn baz();
                }
                fn user() { unsafe { baz() } }
            "#,
        )],
    );
    run(
        vec![lib],
        expect![[r#"
            // crate `k` (lib), mod []
            pub fn baz() {}
            pub mod _xj_ffi {
                #[no_mangle]
                pub extern "C" fn baz() {
                    super::baz()
                }
            }
            fn user() {
                unsafe { crate::baz() }
            }

        "#]],
    );
}

#[test]
fn calls_inside_macros_are_rewritten() {
    // `printf("%d", foo())` becomes `println!("{}", foo())`; the call lives
    // inside an opaque macro token stream and must still be relinked.
    let lib = unit(
        "lib",
        "lib",
        &["lib"],
        "/ws/lib/src/lib.rs",
        &[(
            &[],
            r#"
                #[no_mangle]
                pub extern "C" fn foo() -> i32 { 7 }
            "#,
        )],
    );
    let bin = unit(
        "app",
        "app",
        &["bin"],
        "/ws/app/src/main.rs",
        &[(
            &[],
            r#"
                extern "C" {
                    fn foo() -> i32;
                }
                fn main() { println!("{}", unsafe { foo() }); let _ = (5i32).foo; }
            "#,
        )],
    );
    run(
        vec![lib, bin],
        expect![[r#"
            // crate `lib` (lib), mod []
            #[no_mangle]
            pub extern "C" fn foo() -> i32 {
                7
            }

            // crate `app` (bin), mod []
            fn main() {
                println!("{}", unsafe { lib::foo() });
                let _ = (5i32).foo;
            }

            // dep: /ws/app needs lib (at /ws/lib)
        "#]],
    );
}

#[test]
fn unmatched_extern_is_left_untouched() {
    // A foreign symbol nobody exports stays exactly as it was.
    let bin = unit(
        "app",
        "app",
        &["bin"],
        "/ws/app/src/main.rs",
        &[(
            &[],
            r#"
                extern "C" {
                    fn external_thing() -> ::core::ffi::c_int;
                }
                unsafe fn use_it() -> ::core::ffi::c_int { external_thing() }
            "#,
        )],
    );
    run(
        vec![bin],
        expect![[r#"
            // crate `app` (bin), mod []
            extern "C" {
                fn external_thing() -> ::core::ffi::c_int;
            }
            unsafe fn use_it() -> ::core::ffi::c_int {
                external_thing()
            }

        "#]],
    );
}

#[test]
fn partially_matched_extern_block_keeps_unmatched_entry() {
    let lib = unit(
        "lib",
        "lib",
        &["lib"],
        "/ws/lib/src/lib.rs",
        &[(
            &[],
            r#"
                #[no_mangle]
                pub extern "C" fn known() {}
            "#,
        )],
    );
    let bin = unit(
        "app",
        "app",
        &["bin"],
        "/ws/app/src/main.rs",
        &[(
            &[],
            r#"
                extern "C" {
                    fn known();
                    fn unknown();
                }
                fn user() { unsafe { known(); unknown(); } }
            "#,
        )],
    );
    run(
        vec![lib, bin],
        expect![[r#"
            // crate `lib` (lib), mod []
            #[no_mangle]
            pub extern "C" fn known() {}

            // crate `app` (bin), mod []
            extern "C" {
                fn unknown();
            }
            fn user() {
                unsafe {
                    lib::known();
                    unknown();
                }
            }

            // dep: /ws/app needs lib (at /ws/lib)
        "#]],
    );
}
