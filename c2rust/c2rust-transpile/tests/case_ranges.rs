mod common;

use c2rust_rust_tools::RustEdition::Edition2024;
use std::{fs, process::Command};

fn successful(command: &mut Command) {
    let output = command.output().unwrap();
    assert!(
        output.status.success(),
        "{command:?}\n{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn gnu_case_ranges_preserve_endpoints_and_fallthrough() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("case_ranges.c");
    fs::write(
        &source,
        r#"
        int identifier(int c) {
            switch (c) {
            case 'a' ... 'z': case 'A' ... 'Z': case '_': case '$': return 1;
            default: return 0;
            }
        }
        int signed_range(int n) {
            switch (n) {
            case -5 ... -3: return 1;
            case 2 - 1 ... 2 + 1: return 2;
            default: return 0;
            }
        }
        int fallthrough(int n) {
            int result = 0;
            switch (n) {
            case 0: result += 1;
            case 8 ... 7: result += 2;
            case 1 ... 3: result += 4; break;
            default: result = -1;
            }
            return result;
        }
        int unsigned_range(unsigned long long n) {
            switch (n) {
            case 0xfffffffffffffffdULL ... 0xffffffffffffffffULL: return 1;
            default: return 0;
            }
        }
        int signed_limit(long long n) {
            switch (n) {
            case (-9223372036854775807LL - 1) ... -9223372036854775806LL: return 1;
            default: return 0;
            }
        }
        enum letter { FIRST = 'a', LAST = 'z' };
        int enum_range(enum letter c) {
            switch (c) {
            case FIRST ... LAST: return 1;
            default: return 0;
            }
        }
        int check(void) {
            for (int c = -1; c <= 256; c++) {
                int expected = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
                if (identifier(c) != expected) return 1;
                if (enum_range((enum letter)c) != (c >= 'a' && c <= 'z')) return 2;
            }
            for (int n = -8; n <= 8; n++) {
                int expected = n >= -5 && n <= -3 ? 1 : n >= 1 && n <= 3 ? 2 : 0;
                if (signed_range(n) != expected) return 3;
            }
            if (fallthrough(0) != 7 || fallthrough(1) != 4 || fallthrough(3) != 4 || fallthrough(8) != -1) return 4;
            if (unsigned_range(0xfffffffffffffffcULL) != 0 || unsigned_range(0xfffffffffffffffdULL) != 1 || unsigned_range(~0ULL) != 1) return 5;
            if (signed_limit(-9223372036854775807LL - 1) != 1 || signed_limit(-9223372036854775806LL) != 1 || signed_limit(-9223372036854775805LL) != 0) return 6;
            int count = 0;
            switch (++count) { case 1 ... 3: break; default: return 7; }
            return count != 1;
        }
        "#,
    )
    .unwrap();

    let native = dir.path().join("native.c");
    fs::write(
        &native,
        "#include \"case_ranges.c\"\nint main(void) { return check(); }",
    )
    .unwrap();
    let c_runner = dir.path().join("c_runner");
    successful(
        Command::new("cc")
            .arg("-std=gnu11")
            .arg(&native)
            .arg("-o")
            .arg(&c_runner),
    );
    successful(&mut Command::new(c_runner));

    let mut config = common::config(Edition2024, serde_json::json!({}));
    config.output_dir = Some(dir.path().join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(config, &commands, &["-std=gnu11"]);

    let runner = dir.path().join("runner.rs");
    fs::write(
        &runner,
        "#[path = \"translated/src/case_ranges.rs\"] mod translated;\nfn main() { assert_eq!(unsafe { translated::check() }, 0); }",
    )
    .unwrap();
    let rust_runner = dir.path().join("rust_runner");
    successful(
        Command::new("rustc")
            .args(["--edition=2024", "-Awarnings"])
            .arg(runner)
            .arg("-o")
            .arg(&rust_runner),
    );
    successful(&mut Command::new(rust_runner));
}
