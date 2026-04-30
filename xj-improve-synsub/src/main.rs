use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use clap::Parser;
use toml_edit::{DocumentMut, Item, Table, value};

use xj_improve_synsub::{Depth, Rewriter, collect_workspace_files};

#[derive(Parser)]
#[command(
    name = "xj-improve-synsub",
    about = "Rewrite Rust source code using `syn`-based substitution rules"
)]
struct Args {
    /// Root directory of the workspace to be rewritten
    workspace_root: PathBuf,

    /// Maximum rewriting depth (omit for unlimited).
    #[arg(long)]
    depth: Option<u32>,

    /// Write results back to the source files instead of stdout.
    #[arg(long)]
    modify_in_place: bool,
}

fn main() -> Result<()> {
    let args = Args::parse();

    let mut crates = collect_workspace_files(&args.workspace_root)?;
    let mut rw = Rewriter::new();

    rw.add_expr_rewrite(Rewriter::rewrite_getchar_variants);
    rw.add_expr_rewrite(Rewriter::rewrite_strstr);
    rw.add_expr_rewrite(Rewriter::rewrite_decayed_array_subscript);
    rw.add_expr_rewrite(Rewriter::rewrite_strlen_of_slice);
    rw.add_expr_rewrite(Rewriter::rewrite_scanf_and_fscanf);
    rw.add_expr_rewrite(Rewriter::rewrite_printf_with_lone_offset_fmt);
    rw.add_expr_rewrite(Rewriter::rewrite_usleep);
    rw.add_expr_rewrite(Rewriter::strip_as_c_float_of_int_literals);
    rw.add_expr_rewrite(Rewriter::rewrite_fgets_stdin_is_null);

    rw.add_stmt_rewrite(Rewriter::rewrite_stmt_outer_parens);
    rw.add_stmt_rewrite(Rewriter::rewrite_local);
    rw.add_stmt_rewrite(Rewriter::rewrite_string_pop_trailing_nul);

    let depth = match args.depth {
        Some(n) => Depth::Limited(n),
        None => Depth::Unlimited,
    };

    for (crate_root, files) in crates.iter_mut() {
        rw.rewrite_crate(files, depth);
        let deps = rw.take_deps();
        if deps.is_empty() {
            continue;
        }
        let manifest_path = find_manifest_path(&crate_root.src_path)?;
        add_deps_to_manifest(&manifest_path, &deps)?;
    }

    for (_crate_root, files) in crates {
        for (path, file) in files {
            let code = prettyplease::unparse(&file);
            if args.modify_in_place {
                fs::write(&path, &code)?;
                eprintln!("synsub overwrote {}", path.display());
            } else {
                println!("// {}", path.display());
                println!("{code}");
            }
        }
    }

    Ok(())
}

fn add_deps_to_manifest(
    manifest_path: &Path,
    deps: &std::collections::BTreeSet<String>,
) -> Result<()> {
    let mut manifest: DocumentMut = fs::read_to_string(manifest_path)
        .with_context(|| format!("reading {}", manifest_path.display()))?
        .parse()
        .with_context(|| format!("parsing {}", manifest_path.display()))?;

    let deps_item = manifest
        .entry("dependencies")
        .or_insert(Item::Table(Table::new()));
    let Some(deps_table) = deps_item.as_table_like_mut() else {
        bail!(
            "[dependencies] in {} is not a table",
            manifest_path.display()
        );
    };

    let mut changed = false;
    for dep in deps {
        if !deps_table.contains_key(dep) {
            deps_table.insert(dep, value(dep_version(dep)));
            changed = true;
        }
    }

    if changed {
        fs::write(manifest_path, manifest.to_string())
            .with_context(|| format!("writing {}", manifest_path.display()))?;
        eprintln!("updated {}", manifest_path.display());
    }

    Ok(())
}

fn dep_version(crate_name: &str) -> &'static str {
    match crate_name {
        "xj_cstr" => "0.1.4",
        "xj_scanf" => "0.2.4",
        _ => "*",
    }
}

fn find_manifest_path(crate_root: &Path) -> Result<PathBuf> {
    let start = crate_root
        .canonicalize()
        .with_context(|| format!("canonicalising {}", crate_root.display()))?;

    let mut dir = if start.is_file() {
        start.parent().map(Path::to_path_buf)
    } else {
        Some(start)
    };

    while let Some(path) = dir {
        let manifest = path.join("Cargo.toml");
        if manifest.is_file() {
            return Ok(manifest);
        }
        dir = path.parent().map(Path::to_path_buf);
    }

    bail!("could not find Cargo.toml for {}", crate_root.display())
}
