use std::fs;
use std::path::PathBuf;

use anyhow::Result;
use clap::Parser;

use xj_improve_relink::collect::collect_workspace;
use xj_improve_relink::manifest::apply_dep_requests;
use xj_improve_relink::relink::relink_workspace;

#[derive(Parser)]
#[command(
    name = "xj-improve-relink",
    about = "Rewire `extern \"C\"` calls to the Rust modules that export them"
)]
struct Args {
    /// Root directory of the workspace to be rewritten.
    workspace_root: PathBuf,

    /// Write results back to the source files instead of stdout.
    #[arg(long)]
    modify_in_place: bool,
}

fn main() -> Result<()> {
    let args = Args::parse();

    let mut units = collect_workspace(&args.workspace_root)?;
    let deps = relink_workspace(&mut units);

    apply_dep_requests(&deps, !args.modify_in_place)?;

    for unit in &units {
        for file in &unit.files {
            let code = prettyplease::unparse(&file.ast);
            if args.modify_in_place {
                fs::write(&file.path, &code)?;
                eprintln!("relink overwrote {}", file.path.display());
            } else {
                println!("// {}", file.path.display());
                println!("{code}");
            }
        }
    }

    Ok(())
}
