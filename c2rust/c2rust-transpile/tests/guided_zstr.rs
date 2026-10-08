mod common;

use c2rust_rust_tools::{rustc, RustEdition::Edition2024};
use std::fs;

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
