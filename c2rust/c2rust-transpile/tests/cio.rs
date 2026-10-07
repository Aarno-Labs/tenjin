mod common;

use c2rust_rust_tools::RustEdition::Edition2024;
use std::fs;
use std::process::Command;

#[test]
fn stdio_and_formatting_use_one_byte_exact_backend() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("io.c");
    fs::write(
        &source,
        r#"
        #include <stdio.h>
        #include <stdarg.h>
        int runtime(FILE *f, const char *fmt, ...) {
            va_list ap; va_start(ap, fmt);
            int n = vfprintf(f, fmt, ap); va_end(ap); return n;
        }
        int main(void) {
            FILE *f = tmpfile();
            if (!f) return 1;
            if (fprintf(f, "%s:%d", "hello", 23) != 8) return 2;
            if (runtime(f, "|%*.*s", 5, 2, "abc") != 6) return 3;
            if (fseek(f, 0, SEEK_SET)) return 4;
            char buf[32] = {0};
            if (fread(buf, 1, 14, f) != 14) return 5;
            if (buf[0] != 'h' || buf[13] != 'b') return 6;
            if (fclose(f)) return 7;
            char out[5];
            if (snprintf(out, 5, "%s", "abcdef") != 6) return 8;
            if (out[3] != 'd' || out[4] != 0) return 9;
            const char *fmt = "%s:%d\n";
            if (printf(fmt, "runtime", 7) != 10) return 10;
            if (printf("\xff%5.3s|%c\n", "\xc3\xa9\xf0\x9f\x91\xa8", 255) != 9) return 11;
            puts("puts");
            fwrite("write", 1, 5, stdout);
            printf("tail\n");
            return 0;
        }
    "#,
    )
    .unwrap();
    let mut cfg = common::config(Edition2024, serde_json::json!({"use_libc": false}));
    cfg.output_dir = Some(dir.path().join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    let rust = fs::read_to_string(dir.path().join("translated/src/io.rs")).unwrap();
    assert!(rust.contains("::xj_cio::FILE_ptr"), "{rust}");
    assert!(rust.contains("xj_cio::cfprintf!"), "{rust}");
    assert!(rust.contains("xj_cfmt::csnprintf!"), "{rust}");
    assert!(!rust.contains("pub struct _IO_FILE"), "{rust}");
    assert!(!rust.contains("to_str().unwrap()"), "{rust}");

    let package = dir.path().join("runtime");
    fs::create_dir_all(package.join("src")).unwrap();
    fs::write(package.join("Cargo.toml"),
        "[package]\nname = \"cio-runtime\"\nversion = \"0.0.0\"\nedition = \"2024\"\n[dependencies]\nxj_cio = {path = \"/home/brk/xj-cio/xj_cio\", features = [\"cfmt\"]}\nxj_cfmt = {path = \"/home/brk/xj_cfmt\"}\n").unwrap();
    fs::write(package.join("src/main.rs"), rust).unwrap();
    let output = Command::new("cargo")
        .args([Edition2024.toolchain(), "run", "--offline", "--quiet"])
        .arg("--manifest-path")
        .arg(package.join("Cargo.toml"))
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "status {:?}\n{}",
        output.status.code(),
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(
        output.stdout,
        b"runtime:7\n\xff  \xc3\xa9\xf0|\xff\nputs\nwritetail\n"
    );
}
