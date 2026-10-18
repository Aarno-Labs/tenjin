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
fn floating_literals_round_in_their_original_type_before_conversion() {
    let dir = tempfile::tempdir().unwrap();
    let source = dir.path().join("float_literal_casts.c");
    fs::write(
        &source,
        r#"
        typedef float Float;
        static const double table[] = {
            1.5849625007211563f, 0.1f, -0.1f, 1e-50f, -1e-50f
        };
        double ergonomic[] = {
            2.0f, 0.5f, 16777216.0f, 33554432.0f,
            0x1p-149f, 0x1.fffffep127f, 1.0000000000000000000001f
        };
        double negative_zero = -0.0f;
        float ergonomic_narrowed = 0.1;
        static double returned(void) { return 0.1f; }
        static double identity(double x) { return x; }
        static double conditional(int c) { return c ? 0.1f : 0.0; }
        static float narrowed(void) { return 1.0000000596046448; }
        int check(void) {
            if (table[0] != 1.58496248722076416015625) return 1;
            if (table[1] != 0.100000001490116119384765625) return 2;
            if (table[2] != -0.100000001490116119384765625) return 3;
            if (table[3] != 0.0 || table[4] != 0.0) return 4;
            if (!(1.0 / table[4] < 0.0)) return 5;
            if (returned() != table[1]) return 6;
            if (identity(0.1f) != table[1]) return 7;
            if (conditional(1) != table[1]) return 8;
            if (0.1f + 0.0 != table[1]) return 9;
            Float aliased = 0.1f;
            if (aliased != table[1]) return 10;
            /* This decimal rounds to the f32 midpoint as a double, then to
               1.0f. Parsing it directly as f32 would round upward instead. */
            if (narrowed() != 1.0f) return 11;
            if (ergonomic[0] != 2.0 || ergonomic[1] != 0.5 ||
                ergonomic[2] != 16777216.0 || ergonomic[3] != 33554432.0 ||
                ergonomic[4] != (double)(Float)0x1p-149 ||
                ergonomic[5] != (double)(Float)0x1.fffffep127 ||
                ergonomic[6] != 1.0 || !(1.0 / negative_zero < 0.0)) return 12;
            if (ergonomic_narrowed != 0.1f) return 13;
            return 0;
        }
        "#,
    )
    .unwrap();

    let native = dir.path().join("native.c");
    fs::write(
        &native,
        "#include \"float_literal_casts.c\"\nint main(void) { return check(); }",
    )
    .unwrap();
    let c_runner = dir.path().join("c_runner");
    successful(Command::new("cc").arg(&native).arg("-o").arg(&c_runner));
    successful(&mut Command::new(c_runner));

    let mut config = common::config(Edition2024, serde_json::json!({}));
    config.output_dir = Some(dir.path().join("translated"));
    config.reduce_type_annotations = true;
    let (_commands_dir, commands) = c2rust_transpile::create_temp_compile_commands(&[source]);
    c2rust_transpile::transpile(config, &commands, &[]);

    let translated = syn::parse_file(
        &fs::read_to_string(dir.path().join("translated/src/float_literal_casts.rs")).unwrap(),
    )
    .unwrap();
    let initializer = |name: &str| {
        translated
            .items
            .iter()
            .find_map(|item| match item {
                syn::Item::Static(item) if item.ident == name => Some(&*item.expr),
                _ => None,
            })
            .unwrap()
    };
    let syn::Expr::Array(array) = initializer("ergonomic") else {
        panic!("expected array")
    };
    assert_eq!(array.elems.len(), 7);
    for expr in &array.elems {
        let expr = match expr {
            syn::Expr::Unary(neg) => &*neg.expr,
            expr => expr,
        };
        assert!(
            matches!(expr, syn::Expr::Lit(syn::ExprLit { lit: syn::Lit::Float(lit), .. }) if lit.suffix() == "f64"),
            "{expr:?}"
        );
    }
    assert!(
        matches!(initializer("ergonomic_narrowed"), syn::Expr::Lit(syn::ExprLit { lit: syn::Lit::Float(lit), .. }) if lit.suffix() == "f32")
    );

    let runner = dir.path().join("runner.rs");
    fs::write(
        &runner,
        "#[path = \"translated/src/float_literal_casts.rs\"] mod translated;\nfn main() { assert_eq!(unsafe { translated::check() }, 0); }",
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
