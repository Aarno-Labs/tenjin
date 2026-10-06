mod common;

use c2rust_rust_tools::{rustc, RustEdition::Edition2024};
use std::fs;
use std::path::Path;
use std::process::Command;

fn transpile_zstr(
    dir: &Path,
    source: &str,
    guidance: serde_json::Value,
) -> (std::path::PathBuf, String) {
    let source_path = dir.join("coercions.c");
    fs::write(&source_path, source).unwrap();
    let mut cfg = common::config(Edition2024, guidance);
    cfg.output_dir = Some(dir.join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source_path]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    let output = dir.join("translated/src/coercions.rs");
    let translated = fs::read_to_string(&output).unwrap();
    (output, translated)
}

fn compile(command: &mut Command) {
    let output = command.output().unwrap();
    assert!(
        output.status.success(),
        "rustc failed:\n{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

fn zstr_library(dir: &Path) -> std::path::PathBuf {
    let source = dir.join("xj_zstr.rs");
    let library = dir.join("libxj_zstr.rlib");
    // Match the methods and borrow mutability without depending on the external crate.
    fs::write(
        &source,
        r#"
        pub struct ZString(Vec<u8>);
        pub struct ZStr([u8]);
        impl ZString {
            pub fn as_bytes_with_nul(&self) -> &[u8] { &self.0 }
            pub fn as_bytes_mut(&mut self) -> &mut [u8] { &mut self.0 }
        }
        impl ZStr {
            pub fn as_bytes_with_nul(&self) -> &[u8] { &self.0 }
        }
        "#,
    )
    .unwrap();
    compile(
        Command::new("rustc")
            .args([
                "--crate-type=rlib",
                "--crate-name=xj_zstr",
                "--edition=2024",
            ])
            .arg(source)
            .arg("-o")
            .arg(&library),
    );
    library
}

#[test]
fn guided_zstr_values_coerce_at_argument_initializer_and_assignment_boundaries() {
    let dir = tempfile::tempdir().unwrap();
    let library = zstr_library(dir.path());
    for (ty, target, methods) in [
        ("ZString", "const char", "as_bytes_with_nul().as_ptr()"),
        (
            "::xj_zstr::ZString",
            "const char",
            "as_bytes_with_nul().as_ptr()",
        ),
        ("&ZStr", "const char", "as_bytes_with_nul().as_ptr()"),
        (
            "&'static ::xj_zstr::ZStr",
            "const char",
            "as_bytes_with_nul().as_ptr()",
        ),
        (
            "&mut xj_zstr::ZStr",
            "const char",
            "as_bytes_with_nul().as_ptr()",
        ),
        (
            "&xj_zstr::ZString",
            "const char",
            "as_bytes_with_nul().as_ptr()",
        ),
        ("ZString", "char", "as_bytes_mut().as_mut_ptr()"),
        ("xj_zstr::ZString", "char", "as_bytes_mut().as_mut_ptr()"),
    ] {
        let c_source = format!(
            "typedef {target} Byte; typedef Byte *Raw;\n\
             void sink(Raw value) {{}}\n\
             void consumer(char *value) {{\n\
                 sink(value);\n\
                 sink((value));\n\
                 sink(((Raw)value));\n\
                 Raw local = value;\n\
                 local = value;\n\
             }}"
        );
        let (output, translated) = transpile_zstr(
            dir.path(),
            &c_source,
            serde_json::json!({"vars_of_type": {ty: "consumer:value"}}),
        );
        assert_eq!(
            translated.matches(methods).count(),
            5,
            "{ty}:\n{translated}"
        );
        compile(
            Command::new("rustc")
                .args(["--crate-type=lib", "--edition=2024", "--extern"])
                .arg(format!("xj_zstr={}", library.display()))
                .arg(output)
                .arg("-o")
                .arg(dir.path().join("libcoercions.rlib")),
        );
    }
}

#[test]
fn guided_zstr_subscripts_and_return_values_coerce_to_pointers() {
    let dir = tempfile::tempdir().unwrap();
    let library = zstr_library(dir.path());
    for (ty, element, target, methods) in [
        (
            "&[ZString]",
            "values[0]",
            "const char",
            "as_bytes_with_nul().as_ptr()",
        ),
        (
            "&[&xj_zstr::ZStr]",
            "values[0]",
            "const char",
            "as_bytes_with_nul().as_ptr()",
        ),
        (
            "&mut [xj_zstr::ZString]",
            "values[0]",
            "char",
            "as_bytes_mut().as_mut_ptr()",
        ),
        (
            "::xj_zstr::ZString",
            "make_value()",
            "const char",
            "as_bytes_with_nul().as_ptr()",
        ),
        (
            "::xj_zstr::ZString",
            "make_value()",
            "char",
            "as_bytes_mut().as_mut_ptr()",
        ),
    ] {
        let c_source = format!(
            "char *make_value(void);\n\
             void sink({target} *value) {{}}\n\
             void consumer(char **values) {{ sink({element}); }}"
        );
        let guidance = if element == "make_value()" {
            serde_json::json!({"fn_return_type": {"make_value": ty}})
        } else {
            serde_json::json!({"vars_of_type": {ty: "consumer:values"}})
        };
        let (output, translated) = transpile_zstr(dir.path(), &c_source, guidance);
        assert!(translated.contains(methods), "{ty}:\n{translated}");
        compile(
            Command::new("rustc")
                .args(["--crate-type=lib", "--edition=2024", "--extern"])
                .arg(format!("xj_zstr={}", library.display()))
                .arg(output)
                .arg("-o")
                .arg(dir.path().join("libcoercions.rlib")),
        );
    }
}

#[test]
fn zstr_pointer_coercions_respect_destination_guidance_and_borrowing() {
    let dir = tempfile::tempdir().unwrap();
    for (ty, destination) in [
        ("ZString", "ZString"),
        ("&ZStr", "&ZStr"),
        ("&ZStr", "*mut u8"),
        ("&mut ZStr", "*mut u8"),
        ("&ZString", "*mut u8"),
        ("other::ZString", "*const u8"),
        ("Vec<ZString>", "*const u8"),
        ("Option<&ZStr>", "*const u8"),
    ] {
        let mut guidance = serde_json::json!({"vars_of_type": {
            destination: ["sink:value", "consumer:local"]
        }});
        if ty == destination {
            guidance["vars_of_type"][ty] =
                serde_json::json!(["consumer:value", "sink:value", "consumer:local"]);
        } else {
            guidance["vars_of_type"][ty] = serde_json::json!("consumer:value");
        }
        let (_, translated) = transpile_zstr(
            dir.path(),
            "void sink(char *value) {} void consumer(char *value) {\n\
                 sink(value); char *local = value; local = value;\n\
             }",
            guidance,
        );
        assert!(
            !translated.contains("as_bytes_"),
            "{ty} -> {destination}:\n{translated}"
        );
    }
}

#[test]
fn raw_pointer_destination_guidance_overrides_c_pointer_mutability() {
    let dir = tempfile::tempdir().unwrap();
    let library = zstr_library(dir.path());
    for (target, destination, methods) in [
        ("char", "*const u8", "as_bytes_with_nul().as_ptr()"),
        ("const char", "*mut u8", "as_bytes_mut().as_mut_ptr()"),
    ] {
        let (output, translated) = transpile_zstr(
            dir.path(),
            &format!(
                "void sink({target} *value) {{}} void consumer(char *value) {{\n\
                     sink(value); {target} *local = value; local = value;\n\
                 }}"
            ),
            serde_json::json!({"vars_of_type": {
                "ZString": "consumer:value", destination: ["sink:value", "consumer:local"]
            }}),
        );
        assert_eq!(translated.matches(methods).count(), 3, "{translated}");
        compile(
            Command::new("rustc")
                .args(["--crate-type=lib", "--edition=2024", "--extern"])
                .arg(format!("xj_zstr={}", library.display()))
                .arg(output)
                .arg("-o")
                .arg(dir.path().join("libcoercions.rlib")),
        );
    }
}

fn translated_guidance(ty: &str, local: bool) -> (String, String) {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("guided.c");
    let mut guidance = serde_json::json!({"vars_of_type": {ty: "consumer:value"}});
    fs::write(
        &source,
        if local {
            // The guided return value gives the local a matching initializer;
            // only the local's guidance registers the crate and imports.
            guidance["fn_return_type"] =
                serde_json::json!({"make_value": ty.replace('&', "&'static ")});
            "char *make_value(void); void consumer(void) { char *value = make_value(); }"
        } else {
            "void consumer(char *value) {}"
        },
    )
    .unwrap();
    let mut cfg = common::config(Edition2024, guidance);
    cfg.output_dir = Some(dir.path().join("translated"));
    cfg.emit_build_files = true;
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    (
        fs::read_to_string(dir.path().join("translated/src/guided.rs")).unwrap(),
        fs::read_to_string(dir.path().join("translated/Cargo.toml")).unwrap(),
    )
}

#[test]
fn guided_zstr_locals_and_parameters_register_crate_and_imports() {
    for local in [false, true] {
        for (ty, name) in [
            ("xj_zstr::ZString", "ZString"),
            ("&xj_zstr::ZStr", "ZStr"),
            ("&mut ::xj_zstr::ZStr", "ZStr"),
            ("&xj_zstr::ZString", "ZString"),
            ("Vec<xj_zstr::ZString>", "ZString"),
            ("Option<&xj_zstr::ZStr>", "ZStr"),
            ("ZString", "ZString"),
            ("&ZStr", "ZStr"),
        ] {
            let (source, manifest) = translated_guidance(ty, local);
            assert_eq!(
                manifest.matches("xj_zstr =").count(),
                1,
                "{ty}, local={local}:\n{manifest}"
            );
            assert_eq!(
                source.matches(&format!("use ::xj_zstr::{name};")).count(),
                1,
                "{ty}, local={local}:\n{source}"
            );
        }
    }
}

#[test]
fn other_crates_guided_zstring_does_not_register_xj_zstr() {
    let (source, manifest) = translated_guidance("other::ZString", false);
    assert!(!source.contains("xj_zstr"));
    assert!(!manifest.contains("xj_zstr ="));
}

#[test]
fn guided_strings_and_str_slices_have_constant_null_checks() {
    for ty in ["String", "&str", "& str", "&'static str"] {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("null_checks.c");
        fs::write(
            &source,
            "int check(char *value) {\n\
                if (value == 0) return 1;\n\
                if (0 == value) return 2;\n\
                if (value != 0) { if (0 != value) return 0; }\n\
                return 3;\n\
            }\n",
        )
        .unwrap();
        let guidance = serde_json::json!({"vars_of_type": {ty: "check:value"}});
        let mut cfg = common::config(Edition2024, guidance);
        cfg.output_dir = Some(dir.path().join("translated"));
        let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
        c2rust_transpile::transpile(cfg, &commands, &[]);

        let output = dir.path().join("translated/src/null_checks.rs");
        let translated = fs::read_to_string(&output).unwrap();
        assert!(!translated.contains(".is_null()"), "{ty}:\n{translated}");
        assert!(translated.contains("if false"), "{ty}:\n{translated}");
        assert!(translated.contains("if true"), "{ty}:\n{translated}");
        rustc(&output).edition(Edition2024).run();
    }
}

#[test]
fn guided_subscripts_use_element_types_for_null_checks() {
    for (ty, parameter, element) in [
        ("&[&str]", "char **values", "values[0]"),
        ("&mut [&str]", "char **values", "values[0]"),
        ("[&str; 2]", "char *values[2]", "values[0]"),
        ("&[&str; 2]", "char *values[2]", "values[0]"),
        ("&mut [&str; 2]", "char *values[2]", "values[0]"),
        ("&'static [&'static str]", "char **values", "(values)[0]"),
        ("&[&[&str]]", "char ***values", "values[0][0]"),
        ("[[&str; 2]; 2]", "char *values[2][2]", "values[0][0]"),
        ("&[*mut u8]", "char **values", "values[0]"),
        ("[*mut u8; 2]", "char *values[2]", "values[0]"),
        ("*mut *mut u8", "char **values", "values[0]"),
    ] {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("element_null_checks.c");
        fs::write(
            &source,
            format!(
                "int check({parameter}) {{\n\
                    if ({element} == 0) return 1;\n\
                    if (0 == {element}) return 2;\n\
                    if ({element} != 0) {{ if (0 != {element}) return 0; }}\n\
                    return 3;\n\
                }}\n"
            ),
        )
        .unwrap();
        let guidance = serde_json::json!({"vars_of_type": {ty: "check:values"}});
        let mut cfg = common::config(Edition2024, guidance);
        cfg.output_dir = Some(dir.path().join("translated"));
        let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
        c2rust_transpile::transpile(cfg, &commands, &[]);

        let output = dir.path().join("translated/src/element_null_checks.rs");
        let translated = fs::read_to_string(&output).unwrap();
        let constant_checks = ty.contains("str");
        assert_eq!(
            translated.contains(".is_null()"),
            !constant_checks,
            "{ty}:\n{translated}"
        );
        if constant_checks {
            assert!(translated.contains("if false"), "{ty}:\n{translated}");
            assert!(translated.contains("if true"), "{ty}:\n{translated}");
        }
        rustc(&output).edition(Edition2024).run();
    }
}
