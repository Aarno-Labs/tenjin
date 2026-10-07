mod common;

use c2rust_rust_tools::RustEdition::Edition2024;
use std::fs;
use std::path::Path;
use std::process::Command;

fn translate(dir: &Path, sources: &[(&str, &str)], guidance: serde_json::Value) -> Vec<String> {
    translate_with_args(dir, sources, guidance, &[])
}

fn translate_with_args(
    dir: &Path,
    sources: &[(&str, &str)],
    guidance: serde_json::Value,
    args: &[&str],
) -> Vec<String> {
    let paths = sources
        .iter()
        .map(|(name, source)| {
            let path = dir.join(name);
            fs::write(&path, source).unwrap();
            path
        })
        .collect::<Vec<_>>();
    let mut config = common::config(Edition2024, guidance);
    config.output_dir = Some(dir.join("translated"));
    config.emit_build_files = true;
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&paths);
    c2rust_transpile::transpile(config, &commands, args);
    sources
        .iter()
        .map(|(name, _)| {
            fs::read_to_string(dir.join("translated/src").join(name.replace(".c", ".rs"))).unwrap()
        })
        .collect()
}

fn successful(command: &mut Command) {
    let output = command.output().unwrap();
    assert!(
        output.status.success(),
        "{command:?}\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn compile_and_run(dir: &Path, source: &str) {
    // A separate Cargo target avoids locking the target running these tests.
    // Use the real libc crate, including its private fields, rather than a
    // stand-in that could accidentally accept an invalid exhaustive literal.
    let package = dir.join("runtime");
    fs::create_dir_all(package.join("src")).unwrap();
    fs::write(package.join("Cargo.toml"),
        "[package]\nname = \"stat-runtime\"\nversion = \"0.0.0\"\nedition = \"2024\"\n[dependencies]\nlibc = \"0.2\"\n").unwrap();
    fs::write(package.join("src/main.rs"), source).unwrap();
    successful(
        Command::new("cargo")
            .args(["run", "--offline", "--quiet"])
            .arg("--manifest-path")
            .arg(package.join("Cargo.toml")),
    );
}

#[test]
#[cfg(all(target_arch = "x86_64", target_os = "linux", target_env = "gnu"))]
fn system_stat_uses_real_libc_and_matches_c_behavior() {
    let dir = tempfile::tempdir().unwrap();
    let c = r#"
        #include <sys/stat.h>
        #include <fcntl.h>
        #include <unistd.h>
        #include <errno.h>
        typedef struct stat status;
        struct holder { status s; };
        struct stat global = {.st_size = 21, .st_mtim = {5, 6}};
        int kind(int mode) {
            switch (mode) { case S_IFBLK: return 1; case S_IFCHR: return 2; default: return 0; }
        }
        int main(void) {
            status s = {.st_size = 1, .st_mtim = {3, 4}};
            status array[2] = {{0}, {.st_mode = S_IFREG | S_IRWXU}};
            struct holder h = {.s = {.st_size = 42}};
            *(&s.st_mtim.tv_nsec) += 2;
            s.st_atime = 17;
            s.st_ctim.tv_sec = 19;
            if (sizeof(status) != 144 || global.st_size != 21 ||
                global.st_mtim.tv_nsec != 6 || s.st_mtim.tv_nsec != 6 ||
                s.st_mtime != 3 || s.st_atime != 17 || s.st_ctime != 19 ||
                array[0].st_size != 0 || h.s.st_size != 42) return 1;
            if ((array[1].st_mode & S_IFMT) != S_IFREG ||
                (S_IFBLK | S_IRGRP) != 060040 || S_IRWXG != 070 ||
                S_IRWXO != 007 || ~S_IFMT >= 0) return 2;
            if (kind(S_IFBLK) != 1 || kind(S_IFCHR) != 2) return 2;
            int (*stat_pointer)(const char *, struct stat *) = stat;
            if (stat_pointer("/dev/null", &s) || (s.st_mode & S_IFMT) != S_IFCHR) return 3;
            status output;
            if (stat("/dev/null", &output) || (output.st_mode & S_IFMT) != S_IFCHR) return 3;
            int fd = open("/dev/null", O_RDONLY);
            if (fd < 0 || fstat(fd, &s) || close(fd)) return 4;
            if (fstatat(AT_FDCWD, "/dev/null", &s, 0)) return 5;
            if (lstat("/proc/self/exe", &s) || (s.st_mode & S_IFMT) != S_IFLNK) return 6;
            if (stat("/this-xj-stat-test-path-does-not-exist", &s) != -1 || errno != ENOENT) return 7;
            int (*p_chmod)(const char *, mode_t) = chmod;
            int (*p_fchmod)(int, mode_t) = fchmod;
            int (*p_fchmodat)(int, const char *, mode_t, int) = fchmodat;
            int (*p_mkdir)(const char *, mode_t) = mkdir;
            int (*p_mkdirat)(int, const char *, mode_t) = mkdirat;
            int (*p_mkfifo)(const char *, mode_t) = mkfifo;
            int (*p_mkfifoat)(int, const char *, mode_t) = mkfifoat;
            int (*p_mknod)(const char *, mode_t, dev_t) = mknod;
            int (*p_mknodat)(int, const char *, mode_t, dev_t) = mknodat;
            mode_t (*p_umask)(mode_t) = umask;
            return !p_chmod || !p_fchmod || !p_fchmodat || !p_mkdir || !p_mkdirat ||
                !p_mkfifo || !p_mkfifoat || !p_mknod || !p_mknodat || !p_umask;
        }
    "#;
    let rust = translate(dir.path(), &[("stat.c", c)], serde_json::json!({})).remove(0);
    assert!(rust.contains("::libc::stat"), "{rust}");
    assert!(!rust.contains("pub struct stat"), "{rust}");
    assert!(
        rust.contains("MaybeUninit::<::libc::stat>::zeroed()"),
        "{rust}"
    );
    for constant in [
        "S_IFMT", "S_IFBLK", "S_IRWXU", "S_IRWXG", "S_IRWXO", "S_IRGRP",
    ] {
        assert!(
            rust.contains(&format!("::libc::{constant}")),
            "missing {constant}:\n{rust}"
        );
    }
    for function in [
        "stat", "lstat", "fstat", "fstatat", "chmod", "fchmod", "fchmodat", "mkdir", "mkdirat",
        "mkfifo", "mkfifoat", "mknod", "mknodat", "umask",
    ] {
        assert!(
            rust.contains(&format!("::libc::{function}(")),
            "missing {function}:\n{rust}"
        );
    }
    let manifest = fs::read_to_string(dir.path().join("translated/Cargo.toml")).unwrap();
    assert_eq!(manifest.matches("libc =").count(), 1, "{manifest}");
    let cli = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("cli/10j");
    let binary = dir.path().join("c-stat");
    successful(
        Command::new(cli)
            .arg("clang")
            .arg(dir.path().join("stat.c"))
            .arg("-o")
            .arg(&binary),
    );
    successful(&mut Command::new(binary));
    compile_and_run(dir.path(), &rust);
}

#[test]
fn project_declarations_and_redefined_macros_remain_local() {
    let dir = tempfile::tempdir().unwrap();
    let c = "#define S_IFMT 123\nstruct stat { long st_size; };\nint stat(void) { return S_IFMT; }\nint main(void) { struct stat s = {4}; return stat() == 123 && s.st_size == 4 ? 0 : 1; }";
    let rust = translate(dir.path(), &[("local.c", c)], serde_json::json!({})).remove(0);
    assert!(rust.contains("pub struct stat"), "{rust}");
    assert!(!rust.contains("::libc::"), "{rust}");
    compile_and_run(dir.path(), &rust);

    let dir = tempfile::tempdir().unwrap();
    let rust = translate(dir.path(), &[("redefined.c", "#include <sys/stat.h>\n#undef S_IFMT\n#define S_IFMT 123\nint main(void) { return S_IFMT == 123 ? 0 : 1; }")], serde_json::json!({})).remove(0);
    assert!(!rust.contains("::libc::S_IFMT"), "{rust}");
    compile_and_run(dir.path(), &rust);
}

#[test]
#[cfg(all(target_arch = "x86_64", target_os = "linux", target_env = "gnu"))]
fn unsupported_stat_uses_and_explicit_disable_keep_generated_record() {
    for (code, guidance) in [
        (
            "long inspect(struct stat *s) { return s->__glibc_reserved[0]; }",
            serde_json::json!({}),
        ),
        (
            "struct timespec *inspect(struct stat *s) { return &s->st_mtim; }",
            serde_json::json!({}),
        ),
        ("struct stat value = {.__pad0 = 1};", serde_json::json!({})),
        (
            "long inspect(struct stat *s) { return s->st_size; }",
            serde_json::json!({"use_libc": false}),
        ),
    ] {
        let dir = tempfile::tempdir().unwrap();
        let c = format!("#include <sys/stat.h>\n{code}");
        let rust = translate(dir.path(), &[("fallback.c", &c)], guidance).remove(0);
        assert!(rust.contains("pub struct stat"), "{code}:\n{rust}");
        assert!(!rust.contains("::libc::stat"), "{code}:\n{rust}");
    }
}

#[test]
#[cfg(all(target_arch = "x86_64", target_os = "linux", target_env = "gnu"))]
fn connected_translation_units_choose_the_same_stat_representation() {
    let dir = tempfile::tempdir().unwrap();
    let source = [
        ("producer.c", "#include <sys/stat.h>\nvoid fill(struct stat *s) { s->st_size = 9; }"),
        ("consumer.c", "#include <sys/stat.h>\nvoid fill(struct stat *s);\nlong inspect(void) { struct stat s = {0}; fill(&s); return s.__glibc_reserved[0] + s.st_size; }"),
    ];
    let rust = translate(dir.path(), &source, serde_json::json!({}));
    for source in &rust {
        assert!(source.contains("pub struct stat"), "{source}");
        assert!(!source.contains("::libc::stat"), "{source}");
    }
    compile_and_run(dir.path(), &format!("mod producer {{ {} }}\nmod consumer {{ {} }}\nfn main() {{ unsafe {{ assert_eq!(consumer::inspect(), 9); }} }}", rust[0], rust[1]));

    let dir = tempfile::tempdir().unwrap();
    let source = [
        ("producer.c", source[0].1),
        ("consumer.c", "#include <sys/stat.h>\nvoid fill(struct stat *s);\nlong inspect(void) { struct stat s = {0}; fill(&s); return s.st_size; }"),
    ];
    let rust = translate(dir.path(), &source, serde_json::json!({}));
    for source in &rust {
        assert!(source.contains("::libc::stat"), "{source}");
        assert!(!source.contains("pub struct stat"), "{source}");
    }
    compile_and_run(dir.path(), &format!("mod producer {{ {} }}\nmod consumer {{ {} }}\nfn main() {{ unsafe {{ assert_eq!(consumer::inspect(), 9); }} }}", rust[0], rust[1]));
}

#[test]
#[cfg(all(target_arch = "x86_64", target_os = "linux", target_env = "gnu"))]
fn guided_stat_parameters_preserve_owned_and_borrowed_member_access() {
    for (ty, argument) in [
        ("libc::stat", "s"),
        ("&libc::stat", "&s"),
        ("&mut libc::stat", "&mut s"),
    ] {
        let dir = tempfile::tempdir().unwrap();
        let rust = translate(dir.path(), &[("guided.c", "#include <sys/stat.h>\nlong inspect(struct stat *s) { return s->st_size + s->st_mtim.tv_sec; }")], serde_json::json!({"vars_of_type": {ty: "inspect:s"}})).remove(0);
        assert!(!rust.contains("pub struct stat"), "{rust}");
        compile_and_run(dir.path(), &format!("{rust}\nfn main() {{ let mut s: libc::stat = unsafe {{ core::mem::zeroed() }}; s.st_size = 4; s.st_mtime = 5; unsafe {{ assert_eq!(inspect({argument}), 9); }} }}"));
    }
}

#[test]
fn unsupported_c_targets_and_old_timestamp_layout_fall_back() {
    for args in [
        &["--target=x86_64-unknown-linux-musl"][..],
        &["-std=c99"][..],
    ] {
        let dir = tempfile::tempdir().unwrap();
        let rust = translate_with_args(
            dir.path(),
            &[(
                "target.c",
                "#include <sys/stat.h>\nlong inspect(struct stat *s) { return s->st_size; }",
            )],
            serde_json::json!({}),
            args,
        )
        .remove(0);
        assert!(rust.contains("pub struct stat"), "{args:?}:\n{rust}");
        assert!(!rust.contains("::libc::stat"), "{args:?}:\n{rust}");
    }
}

#[test]
#[cfg(all(target_arch = "x86_64", target_os = "linux", target_env = "gnu"))]
fn same_name_typedef_uses_the_replacement_and_explicit_field_guidance_falls_back() {
    let dir = tempfile::tempdir().unwrap();
    let rust = translate(dir.path(), &[("alias.c", "#include <fcntl.h>\ntypedef struct stat stat;\nlong inspect(stat *s) { return s->st_size; }")], serde_json::json!({})).remove(0);
    assert!(rust.contains("::libc::stat"), "{rust}");
    compile_and_run(dir.path(), &format!("{rust}\nfn main() {{ let mut s: libc::stat = unsafe {{ core::mem::zeroed() }}; s.st_size = 9; unsafe {{ assert_eq!(inspect(&mut s), 9); }} }}"));

    let dir = tempfile::tempdir().unwrap();
    let rust = translate(
        dir.path(),
        &[(
            "fields.c",
            "#include <sys/stat.h>\nlong inspect(struct stat *s) { return s->st_size; }",
        )],
        serde_json::json!({"vars_of_type": {"i64": "stat:st_size"}}),
    )
    .remove(0);
    assert!(rust.contains("pub struct stat"), "{rust}");
    assert!(!rust.contains("::libc::stat"), "{rust}");
}

#[test]
#[cfg(all(target_arch = "x86_64", target_os = "linux", target_env = "gnu"))]
fn renamed_system_function_keeps_its_assembler_symbol() {
    let dir = tempfile::tempdir().unwrap();
    let c = r#"
        #include <sys/stat.h>
        int stat(const char *, struct stat *) __asm__("custom_stat");
        int custom_stat(const char *path, struct stat *output) { output->st_size = path[5]; return 73; }
        int main(void) {
            struct stat output;
            return stat("/dev/null", &output) == 73 && output.st_size == 'n' ? 0 : 1;
        }
    "#;
    let rust = translate(dir.path(), &[("renamed.c", c)], serde_json::json!({})).remove(0);
    assert!(rust.contains("link_name = \"custom_stat\""), "{rust}");
    assert!(rust.contains("::libc::stat"), "{rust}");
    assert!(!rust.contains("::libc::stat("), "{rust}");
    let cli = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("cli/10j");
    let binary = dir.path().join("c-renamed");
    successful(
        Command::new(cli)
            .arg("clang")
            .arg(dir.path().join("renamed.c"))
            .arg("-o")
            .arg(&binary),
    );
    successful(&mut Command::new(binary));
    compile_and_run(dir.path(), &rust);
}
