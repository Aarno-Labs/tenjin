mod common;

use c2rust_rust_tools::RustEdition::Edition2024;
use std::fs;
use std::process::Command;

#[test]
fn stdio_and_formatting_use_one_byte_exact_backend() {
    // This larger differential input exercises the translator's recursive AST
    // traversal beyond the default 2 MiB test-thread stack.
    std::thread::Builder::new()
        .stack_size(8 * 1024 * 1024)
        .spawn(stdio_and_formatting_check)
        .unwrap()
        .join()
        .unwrap();
}

fn stdio_and_formatting_check() {
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
        int destination_calls;
        int runtime(FILE *f, const char *fmt, ...) {
            va_list ap; va_start(ap, fmt);
            int n = vfprintf(f, fmt, ap); va_end(ap); return n;
        }
        int runtime_buffer(char *xj_cio_plan, size_t n, const char *xj_cio_fmt, ...) {
            va_list ap; va_start(ap, xj_cio_fmt);
            int xj_cio_args = vsnprintf((destination_calls++, xj_cio_plan), n, xj_cio_fmt, ap); va_end(ap); return xj_cio_args;
        }
        int runtime_unbounded(char *out, const char *fmt, ...) {
            va_list ap; va_start(ap, fmt);
            int count = vsprintf(out, fmt, ap); va_end(ap); return count;
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
            char literal[128], dynamic[128];
            const char *integer_fmt = "%hhd/%hd/%ld/%lld/%jd/%zd/%td";
            int literal_n = sprintf(literal, "%hhd/%hd/%ld/%lld/%jd/%zd/%td",
                255, 65535, (long)-7, (long long)-8, (intmax_t)-9, (ssize_t)-10, (ptrdiff_t)-11);
            int dynamic_n = runtime_unbounded(dynamic, integer_fmt,
                255, 65535, (long)-7, (long long)-8, (intmax_t)-9, (ssize_t)-10, (ptrdiff_t)-11);
            if (literal_n != dynamic_n) return 36;
            for (int j = 0; j <= literal_n; ++j) if (literal[j] != dynamic[j]) return 37;
            const char *star_fmt = "%*.*s/%%/%#08.4x";
            literal_n = snprintf(literal, 7, "%*.*s/%%/%#08.4x", -5, 2, "abcdef", 23u);
            dynamic_n = runtime_buffer(dynamic, 7, star_fmt, -5, 2, "abcdef", 23u);
            if (literal_n != dynamic_n || literal[6] != 0 || dynamic[6] != 0) return 38;
            for (int j = 0; j < 7; ++j) if (literal[j] != dynamic[j]) return 39;
            signed char n1 = -1; short n2 = -1; long n3 = -1; long long n4 = -1;
            intmax_t n5 = -1; ssize_t n6 = -1; ptrdiff_t n7 = -1;
            const char *count_fmt = "abc%hhn%hn%ln%lln%jn%zn%tn";
            if (runtime_buffer(dynamic, sizeof dynamic, count_fmt, &n1, &n2, &n3, &n4, &n5, &n6, &n7) != 3) return 40;
            if (n1 != 3 || n2 != 3 || n3 != 3 || n4 != 3 || n5 != 3 || n6 != 3 || n7 != 3) return 41;
            if (runtime_buffer(dynamic, 1, "%") != -1 || destination_calls != 3) return 42;
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
    let original = dir.path().join("original");
    assert!(Command::new("clang")
        .args(["-fno-builtin", "-funsigned-char"])
        .arg(&source)
        .arg("-o")
        .arg(&original)
        .status()
        .unwrap()
        .success());
    let reference = Command::new(&original).output().unwrap();
    assert!(reference.status.success());
    let mut cfg = common::config(Edition2024, serde_json::json!({"use_libc": false}));
    cfg.output_dir = Some(dir.path().join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    let rust = fs::read_to_string(dir.path().join("translated/src/io.rs")).unwrap();
    assert!(rust.contains("::xj_cio::FILE_ptr"), "{rust}");
    assert!(rust.contains("xj_cio::fprintf_raw!"), "{rust}");
    assert!(rust.contains("xj_cio::snprintf_raw!"), "{rust}");
    assert!(!rust.contains("pub struct _IO_FILE"), "{rust}");
    assert!(!rust.contains("to_str().unwrap()"), "{rust}");

    let package = dir.path().join("runtime");
    fs::create_dir_all(package.join("src")).unwrap();
    fs::write(package.join("Cargo.toml"),
        "[package]\nname = \"cio-runtime\"\nversion = \"0.0.0\"\nedition = \"2024\"\n[dependencies]\nxj_cio = {version = \"0.1.0\", features = [\"c_variadic\"]}\nxj_cfmt = \"0.1.0\"\nlibc = \"0.2\"\n").unwrap();
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
    assert_eq!(output.stdout, reference.stdout);
    assert_eq!(
        output.stdout,
        b"runtime:7\n\xff  \xc3\xa9\xf0|\xff\nputs\nwritetail\npartial"
    );
}

#[test]
fn formatting_guided_zstr_uses_byte_views() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("guided.c");
    fs::write(&source, "#include <stdio.h>\n#include <stdarg.h>\nvoid demo(const char *s, char *out, size_t n) { printf(\"%5.3s\", s); snprintf(out, n, \"%s\", s); }\nvoid demo_v(char *out, size_t n, const char *fmt, va_list ap) { vsnprintf(out, n, fmt, ap); }").unwrap();
    let mut cfg = common::config(
        Edition2024,
        serde_json::json!({"vars_of_type": {
            "&ZStr": ["demo:s", "demo_v:fmt"], "&mut ZStr": ["demo:out", "demo_v:out"]
        }}),
    );
    cfg.output_dir = Some(dir.path().join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    let rust = fs::read_to_string(dir.path().join("translated/src/guided.rs")).unwrap();
    assert!(rust.contains("::xj_cio::printf!"), "{rust}");
    assert!(rust.contains("::xj_cio::snprintf!(out,"), "{rust}");
    assert!(rust.contains("::xj_cio::snprintf_plan_("), "{rust}");
    assert!(!rust.contains("to_string_lossy"), "{rust}");
    fs::write(dir.path().join("translated/Cargo.toml"),
        "[package]\nname = \"cio-guided-runtime\"\nversion = \"0.0.0\"\nedition = \"2024\"\n[lib]\npath = \"src/guided.rs\"\n[dependencies]\nxj_cio = \"0.1.0\"\nxj_zstr = {version = \"0.1.0\", features = [\"cfmt\"]}\nxj_cfmt = \"0.1.0\"\nlibc = \"0.2\"\n").unwrap();
    let output = Command::new("cargo")
        .env(
            "CARGO_TARGET_DIR",
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/cio-runtime-tests"),
        )
        .current_dir(dir.path().join("translated"))
        .args([Edition2024.toolchain(), "check", "--offline", "--quiet"])
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

fn translated_text(dir: &std::path::Path, source: &str, guidance: serde_json::Value) -> String {
    let path = dir.join("case.c");
    fs::write(&path, source).unwrap();
    let mut cfg = common::config(Edition2024, guidance);
    cfg.output_dir = Some(dir.join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[path]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    fs::read_to_string(dir.join("translated/src/case.rs")).unwrap()
}

#[test]
fn project_stdio_definitions_keep_their_identity() {
    let dir = tempfile::tempdir().unwrap();
    let rust = translated_text(
        dir.path(),
        r#"
        #include <stdio.h>
        int printf(const char *fmt, ...) { return 101; }
        int fprintf(FILE *f, const char *fmt, ...) { return 102; }
        int sprintf(char *out, const char *fmt, ...) { return 103; }
        int snprintf(char *out, size_t n, const char *fmt, ...) { return 104; }
        int puts(const char *s) { return 105; }
        int main(void) {
            char out[10]; const char *fmt = "%d";
            if (printf("%d", 1) != 101 || printf(fmt, 1) != 101) return 1;
            if (fprintf(stdout, "%d", 1) != 102 || fprintf(stdout, fmt, 1) != 102) return 2;
            if (sprintf(out, "%d", 1) != 103 || sprintf(out, fmt, 1) != 103) return 3;
            if (snprintf(out, 10, "%d", 1) != 104 || snprintf(out, 10, fmt, 1) != 104) return 4;
            if (puts("s") != 105) return 5;
            return 0;
        }
    "#,
        serde_json::json!({}),
    );
    for name in ["printf", "fprintf", "sprintf", "snprintf", "puts"] {
        assert!(rust.contains(&format!("fn {name}(")), "{rust}");
        assert!(!rust.contains(&format!("xj_cio::{name}!")), "{rust}");
        assert!(!rust.contains(&format!("xj_cio::{name}_safe!")), "{rust}");
    }
    // Other stdio declarations remain on the established stream backend.
    assert!(rust.contains("xj_cio::stdout()"), "{rust}");
}

#[test]
fn alternate_headers_require_explicit_trust_for_both_format_kinds() {
    let dir = tempfile::tempdir().unwrap();
    let header = dir.path().join("stdio.h");
    fs::write(&header, "int printf(const char *, ...);\n").unwrap();
    let source =
        "#include \"stdio.h\"\nvoid demo(const char *f) { printf(\"%d\", 42); printf(f, 42); }";
    // Failure occurs before declarations or output are replaced.
    let error = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        translated_text(dir.path(), source, serde_json::json!({"use_libc": false}));
    }))
    .unwrap_err();
    let message = error
        .downcast_ref::<String>()
        .map(String::as_str)
        .or_else(|| error.downcast_ref::<&str>().copied())
        .unwrap();
    assert!(message.contains("stdio_header_roots"), "{message}");
    assert!(!dir.path().join("translated/src/case.rs").exists());
    let rust = translated_text(
        dir.path(),
        source,
        serde_json::json!({"stdio_header_roots": [dir.path().to_str().unwrap()]}),
    );
    assert!(rust.contains("xj_cio::printf!(b\"%d\", 42"), "{rust}");
    assert!(rust.contains("xj_cio::printf_raw!("), "{rust}");
    assert!(!rust.contains("extern \"C\" {\n    fn printf"), "{rust}");
}

#[test]
fn literal_formats_preserve_bytes_and_extra_argument_evaluation() {
    let dir = tempfile::tempdir().unwrap();
    let rust = translated_text(
        dir.path(),
        r#"
        #include <stdio.h>
        int tick(void) { return 7; }
        void demo(char *out, const char *path) {
            sprintf(out, "%s/00Tree.html", path);
            printf("%d\0ignored %q", tick(), tick());
            printf("%d", (long)123);
        }
    "#,
        serde_json::json!({}),
    );
    assert!(rust.contains("xj_cio::sprintf_raw!"), "{rust}");
    assert!(rust.contains("b\"%s/00Tree.html\""), "{rust}");
    assert!(!rust.contains("xj_cio_ptr_arg"), "{rust}");
    assert!(!rust.contains("ignored"), "{rust}");
    assert!(rust.contains("tick(), tick()"), "{rust}");
    assert!(rust.contains("as ::core::ffi::c_int"), "{rust}");
}

#[test]
fn printing_keeps_unsafe_operations_visible_in_function_bodies() {
    let dir = tempfile::tempdir().unwrap();
    let rust = translated_text(
        dir.path(),
        r#"
        #include <stdio.h>
        #include <stdarg.h>
        void direct(FILE *f, char *out, const char *s, const char *fmt, int *n) {
            printf("%s", s);
            fprintf(f, "%s", s);
            sprintf(out, "%s", s);
            snprintf(out, 8, "%s", s);
            printf(fmt, s);
            printf("%n", n);
        }
        void variadic(FILE *f, char *out, const char *fmt, va_list ap) {
            vprintf(fmt, ap);
            vfprintf(f, fmt, ap);
            vsprintf(out, fmt, ap);
            vsnprintf(out, 8, fmt, ap);
        }
        "#,
        serde_json::json!({}),
    );
    struct UnsafeBlocks(usize);
    impl syn::visit_mut::VisitMut for UnsafeBlocks {
        fn visit_expr_unsafe_mut(&mut self, _: &mut syn::ExprUnsafe) {
            self.0 += 1;
        }
    }
    let mut file = syn::parse_file(&rust).unwrap();
    let mut blocks = UnsafeBlocks(0);
    syn::visit_mut::VisitMut::visit_file_mut(&mut blocks, &mut file);
    assert_eq!(blocks.0, 0, "{rust}");
    for name in ["direct", "variadic"] {
        let function = file
            .items
            .iter()
            .find_map(|item| match item {
                syn::Item::Fn(function) if function.sig.ident == name => Some(function),
                _ => None,
            })
            .unwrap();
        assert!(function.sig.unsafety.is_some(), "{rust}");
    }
}

#[test]
fn project_stdio_bindings_apply_across_translation_units() {
    let dir = tempfile::tempdir().unwrap();
    let implementation = dir.path().join("implementation.c");
    let caller = dir.path().join("caller.c");
    fs::write(
        &implementation,
        "#include <stdio.h>\nint printf(const char *f, ...) { return 17; }\n",
    )
    .unwrap();
    fs::write(&caller, "#include <stdio.h>\nint main(void) { const char *f = \"%d\"; return printf(\"%d\", 1) + printf(f, 1) - 34; }\n").unwrap();
    let mut cfg = common::config(Edition2024, serde_json::json!({}));
    cfg.output_dir = Some(dir.path().join("translated"));
    let (_commands_dir, commands) =
        c2rust_transpile::create_temp_compile_commands(&[implementation, caller]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    let caller = fs::read_to_string(dir.path().join("translated/src/caller.rs")).unwrap();
    assert!(!caller.contains("xj_cio::printf"), "{caller}");
    assert!(caller.contains("printf("), "{caller}");
    let implementation =
        fs::read_to_string(dir.path().join("translated/src/implementation.rs")).unwrap();
    assert!(implementation.contains("fn printf("), "{implementation}");
}

/// Literal and runtime scanf formats, `hh`, `%n`, unsigned short and
/// pointer-sized destinations, and the `va_list` path agree with native C.
#[test]
fn scanf_family_matches_native() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("scan.c");
    fs::write(
        &source,
        r#"
        #include <stdio.h>
        #include <stdarg.h>
        #include <stdint.h>
        #include <stddef.h>
        #include <inttypes.h>
        #include <sys/types.h>
        static int vs(const char *in, const char *fmt, ...) {
            va_list ap; va_start(ap, fmt);
            int n = vsscanf(in, fmt, ap); va_end(ap); return n;
        }
        struct guarded { unsigned short before; unsigned short v; unsigned short after[3]; };
        int main(void) {
            char c = 0; signed char sc = 0; unsigned char uc = 0; int8_t i8 = 0; uint8_t u8 = 0;
            int n = -1; short sn = -1; long ln = -1; size_t zn = 0; char hn = 0;
            int r = sscanf("-7 200 x -3 250 tail", "%hhd %hhu %c %" SCNd8 " %" SCNu8 "%n %hn%ln%zn%hhn",
                           &sc, &uc, &c, &i8, &u8, &n, &sn, &ln, &zn, &hn);
            printf("%d %d %d %d %d %d %d %d %ld %zu %d\n", r, sc, uc, c, i8, u8, n, sn, ln, zn, hn);
            unsigned short us = 0; short ss = 0; uint16_t u16 = 0; ssize_t sz = 0; ptrdiff_t pd = 0;
            intptr_t ip = 0; uintptr_t up = 0;
            r = sscanf("65535 -2 65534 -5 -6 -7 8", "%hu %hd %" SCNu16 " %zd %td %" SCNdPTR " %" SCNuPTR,
                       &us, &ss, &u16, &sz, &pd, &ip, &up);
            printf("%d %u %d %u %zd %td %" PRIdPTR " %" PRIuPTR "\n", r, us, ss, u16, sz, pd, ip, up);
            char word[8] = {0}, rest[16] = {0};
            r = sscanf("hello world", "%7s %15s", word, rest);
            printf("%d %s %s\n", r, word, rest);
            const char *fmt = "%d,%d";
            int a = 0, b = 0;
            r = sscanf("3,4", fmt, &a, &b);
            printf("%d %d %d\n", r, a, b);
            struct guarded g = {0xa5a5, 0, {0xa5a5, 0xa5a5, 0xa5a5}};
            signed char h = 0; int d = 0;
            r = vs("9 -1 5", "%hu %hhd %d", &g.v, &h, &d);
            printf("%d %u %d %d %x %x %x %x\n", r, g.v, h, d, g.before, g.after[0], g.after[1], g.after[2]);
            FILE *f = tmpfile();
            if (!f) return 1;
            fputs("abc]def 12 0x1f 2.5", f);
            rewind(f);
            char set[8] = {0}, three[4] = {0}; int k = 0; unsigned x = 0; double dd = 0;
            r = fscanf(f, "%7[a-c]%*c%3c %d %x %lf", set, three, &k, &x, &dd);
            printf("%d %s %s %d %u %g %d\n", r, set, three, k, x, dd, feof(f));
            r = fscanf(f, "%d", &k);
            printf("%d\n", r);
            fclose(f);
            return 0;
        }
    "#,
    )
    .unwrap();
    let native = dir.path().join("native");
    assert!(Command::new("clang")
        .args(["-fno-builtin", "-w"])
        .arg(&source)
        .arg("-o")
        .arg(&native)
        .status()
        .unwrap()
        .success());
    let reference = Command::new(&native).output().unwrap();
    assert!(reference.status.success());
    let mut cfg = common::config(Edition2024, serde_json::json!({"use_libc": false}));
    cfg.output_dir = Some(dir.path().join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(cfg, &commands, &[]);
    let rust = fs::read_to_string(dir.path().join("translated/src/scan.rs")).unwrap();
    for expected in [
        "::xj_cio::sscanf_raw(",
        "::xj_cio::fscanf_raw(",
        "::xj_cio::vsscanf(",
        "OutArg::SChar(",
        "OutArg::UChar(",
        "OutArg::UShort(",
        "OutArg::Size(",
        "OutArg::ISize(",
    ] {
        assert!(rust.contains(expected), "{expected}\n{rust}");
    }
    assert!(!rust.contains("xj_cio_vscanargs"), "{rust}");
    assert!(!rust.contains("size_of::<usize>()"), "{rust}");

    let package = dir.path().join("runtime");
    fs::create_dir_all(package.join("src")).unwrap();
    fs::write(package.join("Cargo.toml"),
        "[package]\nname = \"cio-scan-runtime\"\nversion = \"0.0.0\"\nedition = \"2024\"\n[dependencies]\nxj_cio = {version = \"0.1.0\", features = [\"c_variadic\"]}\nxj_cfmt = \"0.1.0\"\nlibc = \"0.2\"\n").unwrap();
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
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&reference.stdout)
    );
}

/// A literal directive that conflicts with the C pointee type keeps the
/// pointee's width; the engine rejects it at run time.
#[test]
fn scanf_conflicting_literal_keeps_the_destination_width() {
    let dir = tempfile::tempdir().unwrap();
    let rust = translated_text(
        dir.path(),
        "#include <stdio.h>\nint demo(void) { short s = 0; char word[4]; return sscanf(\"5 x\", \"%d %s\", &s, word); }\n",
        serde_json::json!({"use_libc": false}),
    );
    assert!(rust.contains("OutArg::Short("), "{rust}");
    assert!(!rust.contains("OutArg::Int("), "{rust}");
    assert!(rust.contains("OutArg::Str("), "{rust}");
}
