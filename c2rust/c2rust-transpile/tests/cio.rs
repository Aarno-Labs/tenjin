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
        #define __OPTIMIZE__ 1
        #include <stdio.h>
        #include <stdarg.h>
        #include <stdint.h>
        #include <inttypes.h>
        #include <stddef.h>
        #include <locale.h>
        #include <wchar.h>
        int fmt, bytes, i, arg, out, length, position, fallback, ap, suppressed, conv;
        int runtime(FILE *f, const char *fmt, ...) {
            va_list ap; va_start(ap, fmt);
            int n = vfprintf(f, fmt, ap); va_end(ap); return n;
        }
        int runtime_scan(FILE *f, const char *fmt, ...) {
            va_list ap; va_start(ap, fmt);
            int n = vfscanf(f, fmt, ap); va_end(ap); return n;
        }
        int main(void) {
            if (fmt + bytes + i + arg + out + length + position + fallback + ap + suppressed + conv) return 22;
            FILE *f = NULL;
            f = tmpfile();
            if (!f) return 1;
            if (fprintf(f, "%s:%d", "hello", 23) != 8) return 2;
            if (runtime(f, "|%*.*s", 5, 2, "abc") != 6) return 3;
            if (fseek(f, 0, SEEK_SET)) return 4;
            char buf[32] = {0};
            if (fread(buf, 1, 14, f) != 14) return 5;
            if (buf[0] != 'h' || buf[13] != 'b') return 6;
            if (fseek(f, 0, SEEK_SET)) return 19;
            flockfile(f);
            int first = getc_unlocked(f);
            funlockfile(f);
            if (first != 'h' || feof_unlocked(f) || ferror_unlocked(f)) return 20;
            if (fseek(f, 14, SEEK_SET)) return 21;
            if (fseek(f, 0, SEEK_CUR)) return 17;
            int written = -1;
            if (fprintf(f, "%o:%i%n", 8, -3, &written) != 5 || written != 5) return 13;
            if (fseek(f, 14, SEEK_SET)) return 14;
            unsigned value = 0; int signed_value = 0;
            if (runtime_scan(f, "%o:%d", &value, &signed_value) != 2) return 15;
            if (value != 8 || signed_value != -3) return 16;
            if (fclose(f)) return 7;
            f = tmpfile();
            if (!f || fprintf(f, "16 7 -12 65535 -123 4294967295 -1234567890123 1234567890123 -42 42 -3 4") < 0) return 32;
            rewind(f);
            uint32_t flags = 0; size_t count = 0;
            int16_t small_signed = 0; uint16_t small_unsigned = 0;
            int32_t signed32 = 0; uint32_t unsigned32 = 0;
            int64_t signed64 = 0; uint64_t unsigned64 = 0;
            intmax_t signedmax = 0; uintmax_t unsignedmax = 0;
            ptrdiff_t difference = 0; uintptr_t address = 0;
            if (fscanf(f, "%u %zu %hd %hu %" SCNd32 " %" SCNu32 " %" SCNd64 " %" SCNu64 " %jd %ju %td %" SCNuPTR,
                       &flags, &count, &small_signed, &small_unsigned, &signed32, &unsigned32,
                       &signed64, &unsigned64, &signedmax, &unsignedmax, &difference, &address) != 12) return 33;
            if (flags != 16 || count != 7 || small_signed != -12 || small_unsigned != 65535 ||
                signed32 != -123 || unsigned32 != UINT32_MAX || signed64 != -1234567890123LL ||
                unsigned64 != 1234567890123ULL || signedmax != -42 || unsignedmax != 42 ||
                difference != -3 || address != 4) return 34;
            if (fclose(f)) return 35;
            if (!setlocale(LC_CTYPE, "C.UTF-8")) return 23;
            f = tmpfile();
            if (!f || fprintf(f, "%lc", (wint_t)0xa9) != 2) return 24;
            if (fprintf(f, "%lc", (wint_t)0) != 1) return 25;
            if (fseek(f, 0, SEEK_SET)) return 26;
            if (fread(buf, 1, 3, f) != 3 || (unsigned char)buf[0] != 0xc2 || (unsigned char)buf[1] != 0xa9 || buf[2] != 0) return 27;
            if (fprintf(f, "%lc", (wint_t)0xd800) != -1) return 28;
            if (!setlocale(LC_CTYPE, "C")) return 29;
            if (fprintf(f, "%lc", (wint_t)0xa9) != -1) return 30;
            if (!setlocale(LC_CTYPE, "C.UTF-8") || fclose(f)) return 31;
            if (snprintf(NULL, 0, "%d", 123) != 3) return 12;
            char out[5];
            if (snprintf(out, 5, "%s", "abcdef") != 6) return 8;
            if (out[3] != 'd' || out[4] != 0) return 9;
            if (snprintf(out, 5, "%o", 8) != 2 || out[0] != '1' || out[1] != '0') return 18;
            const char *fmt = "%s:%d\n";
            if (printf(fmt, "runtime", 3 + 4) != 10) return 10;
            if (printf("\xff%5.3s|%c\n", "\xc3\xa9\xf0\x9f\x91\xa8", 255) != 9) return 11;
            puts("puts");
            fwrite("write", 1, 5, stdout);
            printf("tail\n");
            printf("partial");
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
        "[package]\nname = \"cio-runtime\"\nversion = \"0.0.0\"\nedition = \"2024\"\n[dependencies]\nxj_cio = {path = \"/home/brk/xj-cio/xj_cio\", features = [\"cfmt\"]}\nxj_cfmt = {path = \"/home/brk/xj_cfmt\"}\nlibc = \"0.2\"\n").unwrap();
    fs::write(package.join("src/main.rs"), rust).unwrap();
    let output = Command::new("cargo")
        .env(
            "CARGO_TARGET_DIR",
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/cio-runtime-tests"),
        )
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
        b"runtime:7\n\xff  \xc3\xa9\xf0|\xff\nputs\nwritetail\npartial"
    );
}

#[test]
fn formatting_guided_zstr_uses_byte_views() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("guided.c");
    fs::write(&source, "#include <stdio.h>\nvoid demo(const char *s, char *out, size_t n) { printf(\"%5.3s\", s); snprintf(out, n, \"%s\", s); }").unwrap();
    let mut cfg = common::config(
        Edition2024,
        serde_json::json!({"vars_of_type": {
            "&ZStr": "demo:s", "&mut ZStr": "demo:out"
        }}),
    );
    cfg.output_dir = Some(dir.path().join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    let rust = fs::read_to_string(dir.path().join("translated/src/guided.rs")).unwrap();
    assert!(rust.contains("::xj_cio::cprintf!"), "{rust}");
    assert!(rust.contains("out.limit("), "{rust}");
    assert!(!rust.contains("to_string_lossy"), "{rust}");
}
