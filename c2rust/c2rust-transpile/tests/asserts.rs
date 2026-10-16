mod common;

use c2rust_rust_tools::RustEdition::Edition2024;
use std::fs;
use std::path::Path;
use std::process::Command;

fn transpile(dir: &Path, source: &str, args: &[&str]) -> String {
    let source_path = dir.join("assertions.c");
    fs::write(&source_path, source).unwrap();
    let mut cfg = common::config(Edition2024, serde_json::json!({}));
    cfg.output_dir = Some(dir.join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source_path]);
    c2rust_transpile::transpile(cfg, &commands, args);
    fs::read_to_string(dir.join("translated/src/assertions.rs")).unwrap()
}

fn build_runner(dir: &Path, body: &str) -> std::path::PathBuf {
    let runner = dir.join("runner.rs");
    fs::write(
        &runner,
        format!(
            "#[path = \"translated/src/assertions.rs\"] mod translated; fn main() {{ {body} }}"
        ),
    )
    .unwrap();
    let executable = dir.join("runner");
    let output = Command::new("rustc")
        .args(["--edition=2024", "-Copt-level=2", "-Cdebug-assertions=off"])
        .arg(&runner)
        .arg("-o")
        .arg(&executable)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    executable
}

#[test]
fn system_assertions_evaluate_once_and_still_fail_in_release() {
    for standard in ["-std=gnu11", "-std=c11"] {
        let dir = tempfile::tempdir().unwrap();
        let rust = transpile(
            dir.path(),
            r#"
            #include <assert.h>
            int check(void) {
                int count = 0;
                assert(++count);
                assert(&count);
                assert(count == 1 && "count must be {1}");
                return count;
            }
            void fail(void) { assert(0 && "failure with {braces}"); }
        "#,
            &[standard],
        );
        assert_eq!(rust.matches("assert!(").count(), 4, "{rust}");
        assert!(rust.contains("failure with {{braces}}"), "{rust}");
        let runner = build_runner(
            dir.path(),
            r#"
            if std::env::args().len() > 1 { unsafe { translated::fail() }; }
            assert_eq!(unsafe { translated::check() }, 1);
        "#,
        );
        assert!(Command::new(&runner).status().unwrap().success());
        let failed = Command::new(&runner).arg("fail").output().unwrap();
        assert!(!failed.status.success());
        assert!(String::from_utf8_lossy(&failed.stderr).contains("failure with {braces}"));
    }
}

#[test]
fn conditional_and_logical_assertion_expansions() {
    let dir = tempfile::tempdir().unwrap();
    let rust = transpile(
        dir.path(),
        r#"
        extern void __assert_fail(const char *, const char *, unsigned, const char *) __attribute__((noreturn));
        extern void __assert_rtn(const char *, const char *, int, const char *) __attribute__((noreturn));
        #define STRICT(c) ((c) ? (void)0 : __assert_fail(#c, "file.c", 1, __func__))
        #define DARWIN(c) (__builtin_expect(!(c), 0) ? __assert_rtn(__func__, "file.c", 1, #c) : (void)0)
        #define MUSL(c) ((void)((c) || (__assert_fail(#c, "file.c", 1, __func__), 0)))
        int check(void) {
            int count = 0;
            STRICT(++count);
            DARWIN(++count);
            MUSL(++count);
            return count;
        }
        void fail(void) { DARWIN(0 && "Darwin failure with {braces}"); }
    "#,
        &[],
    );
    assert_eq!(rust.matches("assert!(").count(), 4, "{rust}");
    assert!(rust.contains("Darwin failure with {{braces}}"), "{rust}");
    let runner = build_runner(dir.path(), "assert_eq!(unsafe { translated::check() }, 3);");
    assert!(Command::new(runner).status().unwrap().success());
}

#[test]
fn custom_assertions_and_side_effecting_failure_arguments_are_preserved() {
    let dir = tempfile::tempdir().unwrap();
    let rust = transpile(
        dir.path(),
        r#"
        int assert(int n) { return n + 1; }
        extern void __assert_fail(const char *, const char *, unsigned, const char *) __attribute__((noreturn));
        int check(void) { return assert(4); }
        void metadata(int *n) {
            *n ? (void)0 : __assert_fail((++*n, "message"), "file.c", 1, "function");
        }
    "#,
        &[],
    );
    assert!(!rust.contains("assert!("), "{rust}");
    assert!(rust.contains("__assert_fail("), "{rust}");
    let runner = build_runner(dir.path(), "assert_eq!(unsafe { translated::check() }, 5);");
    assert!(Command::new(runner).status().unwrap().success());
}
