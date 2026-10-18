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
fn aligned_storage_preserves_c_values_addresses_and_sizes() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("aligned.c");
    fs::write(&source, r#"
        typedef __UINTPTR_TYPE__ word;
        extern int redeclared __attribute__((aligned(128)));
        int redeclared = 7;
        _Alignas(64) int standard = 9;
        static int default_aligned __attribute__((aligned)) = 11;
        static double natural __attribute__((aligned(1))) = 1.5;
        static const word table[3] __attribute__((aligned(64))) = {
            (word)&table[0], (word)&table[1], 42
        };
        static int cleaned;
        static void cleanup(int *n) { cleaned = *n; }
        static void with_cleanup(void) {
            int n __attribute__((aligned(512), cleanup(cleanup))) = 13;
            n += 2;
        }
        static int local_statics(void) {
            static int counter __attribute__((aligned(256))) = 4;
            static word addr __attribute__((aligned(128))) = (word)&counter;
            static int ordinary __attribute__((aligned(64))) = 6;
            if (addr != (word)&counter || addr % 256 || (word)&addr % 128 || (word)&ordinary % 64) return 1;
            return ++counter != 5 || ++ordinary != 7;
        }
        struct node { struct node *self; int value; };
        int check(void) {
            int C2Rust_Aligned512 = 19;
            int collision __attribute__((aligned(512))) = C2Rust_Aligned512;
            if (collision != 19 || (word)&collision % 512) return 12;
            int local __attribute__((aligned(256))) = 2;
            int array[3] __attribute__((__aligned__(128))) = {1, 2, 3};
            struct node node __attribute__((aligned(64))) = {&node, 17};
            if ((word)&local % 256 || (word)array % 128 || (word)&node % 64) return 1;
            int *p = &local;
            *p += 3;
            array[1] += local;
            if (local != 5 || array[1] != 7 || node.self != &node || node.value != 17) return 2;
            if (sizeof(local) != sizeof(int) || sizeof(array) != 3 * sizeof(int)) return 3;
            if (__alignof__(local) != 256 || __alignof__(array) != 128 || __alignof__(node) != 64) return 4;
            if ((word)&redeclared % 128 || (word)&standard % 64 || (word)table % 64) return 5;
            if (__alignof__(redeclared) != 128 || __alignof__(standard) != 64 || __alignof__(table) != 64) return 6;
            if (redeclared != 7 || standard != 9 || table[0] != (word)&table[0] || table[1] != (word)&table[1] || table[2] != 42) return 7;
            if (sizeof(table) != 3 * sizeof(word) || natural != 1.5 || default_aligned != 11) return 8;
            if ((word)&default_aligned % __alignof__(default_aligned) || (word)&natural % __alignof__(double)) return 9;
            with_cleanup();
            if (cleaned != 15 || local_statics()) return 10;
            goto use;
            int skipped __attribute__((aligned(64))) = 100;
        use:
            skipped = 23;
            if ((word)&skipped % 64 || skipped != 23) return 11;
            return 0;
        }
    "#).unwrap();
    let native = dir.path().join("native.c");
    fs::write(
        &native,
        "#include \"aligned.c\"\nint main(void) { return check(); }",
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
    fs::write(&runner, "#[path = \"translated/src/aligned.rs\"] mod translated; fn main() { assert_eq!(unsafe { translated::check() }, 0); }").unwrap();
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

#[test]
fn aligned_externs_and_thread_locals_keep_their_linkage() {
    let dir = tempfile::tempdir().unwrap();
    let external = dir.path().join("external.c");
    fs::write(&external, "_Alignas(128) int external[3] = {5, 6, 7};").unwrap();
    let object = dir.path().join("external.o");
    successful(
        Command::new("cc")
            .arg("-c")
            .arg(&external)
            .arg("-o")
            .arg(&object),
    );
    let source = dir.path().join("aligned.c");
    fs::write(&source, r#"
        typedef __UINTPTR_TYPE__ word;
        extern int external[3] __attribute__((aligned(128)));
        _Thread_local _Alignas(64) int per_thread = 3;
        int check(void) {
            if ((word)external % 128 || __alignof__(external) != 128 || sizeof(external) != 3 * sizeof(int)) return 1;
            if (external[0] != 5 || external[2] != 7) return 2;
            external[1] = 11;
            if ((word)&per_thread % 64 || __alignof__(per_thread) != 64 || per_thread != 3) return 3;
            per_thread += external[1];
            return per_thread != 14;
        }
    "#).unwrap();
    let mut config = common::config(Edition2024, serde_json::json!({}));
    config.output_dir = Some(dir.path().join("translated"));
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(config, &commands, &["-std=gnu11"]);
    let runner = dir.path().join("runner.rs");
    fs::write(&runner, "#![feature(thread_local)]\n#[path = \"translated/src/aligned.rs\"] mod translated; fn main() { assert_eq!(unsafe { translated::check() }, 0); }").unwrap();
    let executable = dir.path().join("runner");
    successful(
        Command::new("rustc")
            .env("RUSTC_BOOTSTRAP", "1")
            .args([
                "--edition=2024",
                "-Awarnings",
                "-C",
                &format!("link-arg={}", object.display()),
            ])
            .arg(runner)
            .arg("-o")
            .arg(&executable),
    );
    successful(&mut Command::new(executable));
}
