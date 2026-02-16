use std::fs;
use std::path::PathBuf;

use anyhow::Result;
use clap::Parser;

use xj_improve_synsub::{collect_crate_files, Depth, Rewriter};

#[derive(Parser)]
#[command(
    name = "xj-improve-synsub",
    about = "Rewrite Rust source code using syn-based substitution rules"
)]
struct Args {
    /// Root source file of the crate to rewrite (e.g. src/lib.rs).
    crate_root: PathBuf,

    /// Maximum rewriting depth (omit for unlimited).
    #[arg(long)]
    depth: Option<u32>,

    /// Write results back to the source files instead of stdout.
    #[arg(long)]
    modify_in_place: bool,
}

fn main() -> Result<()> {
    let args = Args::parse();

    let mut files = collect_crate_files(&args.crate_root)?;
    let mut rw = Rewriter::new();

    rw.add_expr_rewrite(Rewriter::rewrite_strstr);

    let depth = match args.depth {
        Some(n) => Depth::Limited(n),
        None => Depth::Unlimited,
    };

    rw.rewrite_crate(&mut files, depth);

    // Report any new crate dependencies introduced by rewrites.
    let deps = rw.deps();
    if !deps.is_empty() {
        eprintln!("new crate dependencies:");
        for dep in &deps {
            eprintln!("  {dep}");
        }
    }

    for (path, file) in &files {
        let code = prettyplease::unparse(file);
        if args.in_place {
            fs::write(path, &code)?;
            eprintln!("wrote {}", path.display());
        } else {
            println!("// {}", path.display());
            println!("{code}");
        }
    }

    Ok(())
}
