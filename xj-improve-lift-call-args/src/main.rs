//! `xj-improve-lift-call-args` CLI entrypoint.
//!
//! For every Rust source file in the given Cargo workspace, walks each
//! function/method call and applies the lift-subexpressions algorithm.
//! Either prints the rewritten files to stdout or rewrites them in place.

use std::collections::{BTreeMap, HashMap};
use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::Parser;
use ra_ap_base_db::EditionedFileId;
use ra_ap_ide::AnalysisHost;
use ra_ap_syntax::ast::AstNode;

use xj_improve_lift_call_args::access::SubexprId;
use xj_improve_lift_call_args::text_edit::TextEdit;
use xj_improve_lift_call_args::analysis::{
    CallAnalysis, Workspace, analyze_file, file_path, file_source,
    statement_insertion_point,
};
use xj_improve_lift_call_args::emit::{EmitInput, InsertSite, LiftSnippet, emit_edits};
use xj_improve_lift_call_args::lift::{Diagnostic, PlannedLift, plan_lifts};

#[derive(Parser)]
#[command(
    name = "xj-improve-lift-call-args",
    about = "Lift call-argument subexpressions to satisfy Rust borrowck \
             (uses rust-analyzer crates for type/trait info)."
)]
struct Args {
    /// Root directory of the workspace to be rewritten (path to a directory
    /// containing `Cargo.toml`).
    workspace_root: PathBuf,

    /// Write results back to the source files instead of stdout.
    #[arg(long)]
    modify_in_place: bool,

    /// Print one diagnostic per call site we declined to rewrite.
    #[arg(long, default_value_t = false)]
    show_skipped: bool,
}

fn main() -> Result<()> {
    let _ = tracing_subscriber::fmt::try_init();
    let args = Args::parse();

    let workspace = Workspace::load(&args.workspace_root)
        .with_context(|| format!("loading workspace at {}", args.workspace_root.display()))?;

    let mut counter = NameCounter::default();
    let mut diagnostics: Vec<(PathBuf, Diagnostic)> = Vec::new();
    let mut per_file_edits: HashMap<PathBuf, TextEdit> = HashMap::new();

    for (path, file_id) in workspace.files() {
        let analyses = analyze_file(&workspace.host, file_id)?;
        if analyses.is_empty() {
            continue;
        }

        let source = file_source(&workspace.host, file_id)?;
        let db = workspace.host.raw_database();
        let editioned = EditionedFileId::current_edition(db, file_id);
        let parsed = editioned.parse(db);
        let syntax = parsed.tree();

        let mut acc_edit: Option<TextEdit> = None;
        for CallAnalysis {
            call_span,
            accesses,
            shapes,
            snippets,
        } in analyses
        {
            // Step 1-5 in one shot.
            let shapes_map = shapes;
            let lift_result = plan_lifts(call_span, &accesses, |sid| shapes_map.get(&sid).cloned());

            let plan = match lift_result {
                Ok(plan) if plan.lifts.is_empty() => continue,
                Ok(plan) => plan,
                Err(diag) => {
                    diagnostics.push((path.clone(), diag));
                    continue;
                }
            };

            // Look up the original call node for insertion-point logic.
            let call_node = match syntax
                .syntax()
                .descendants()
                .find(|n| n.text_range() == call_span)
            {
                Some(n) => n,
                None => continue,
            };

            let insert = match statement_insertion_point(&call_node) {
                Some(pos) => InsertSite::BeforeStmt { pos },
                None => InsertSite::WrapInBlock { call_span },
            };

            // Build the snippet map keyed by SubexprId for the emitter.
            let snippets_for_emit: BTreeMap<SubexprId, LiftSnippet> = snippets
                .into_iter()
                .map(|(k, (orig, inner))| {
                    (
                        k,
                        LiftSnippet {
                            original_text: orig,
                            inner_place_text: inner,
                        },
                    )
                })
                .collect();

            let mut name_for = |lift: &PlannedLift| -> String { counter.fresh(lift) };

            let input = EmitInput {
                source: &source,
                call_span,
                insert,
                plan: &plan,
                snippets: snippets_for_emit,
                name_for: &mut name_for,
            };

            if let Some(edit) = emit_edits(input) {
                match acc_edit.as_mut() {
                    None => acc_edit = Some(edit),
                    Some(existing) => {
                        if existing.union(edit.clone()).is_err() {
                            // Overlapping edits within one file — keep the
                            // earliest, drop the rest. This only happens
                            // when two call sites share text, which the
                            // algorithm should not produce, but defend
                            // anyway.
                            eprintln!(
                                "xj-improve-lift-call-args: overlapping edits in {}; \
                                 dropping later edits at {:?}",
                                path.display(),
                                call_span
                            );
                        }
                    }
                }
            }
        }

        if let Some(edit) = acc_edit {
            per_file_edits.insert(path.clone(), edit);
        }
    }

    // Apply edits.
    for (path, edit) in per_file_edits {
        let mut text = file_source_for_path(&workspace.host, &workspace.vfs, &path)?;
        edit.apply(&mut text);
        if args.modify_in_place {
            std::fs::write(&path, &text)
                .with_context(|| format!("writing {}", path.display()))?;
            eprintln!("xj-improve-lift-call-args rewrote {}", path.display());
        } else {
            println!("// {}", path.display());
            println!("{}", text);
        }
    }

    if args.show_skipped {
        for (path, diag) in diagnostics {
            eprintln!(
                "xj-improve-lift-call-args: skipped {} @ {:?}",
                path.display(),
                diag.call_span
            );
            eprintln!("  reason: {}", diag.reason);
            let (a, b) = diag.conflicting;
            eprintln!(
                "  conflict: {:?}@{:?}  vs  {:?}@{:?}",
                a.kind, a.span, b.kind, b.span
            );
            if let Some(s) = diag.suggestion {
                eprintln!("  suggest: {}", s);
            }
        }
    }

    Ok(())
}

fn file_source_for_path(
    host: &AnalysisHost,
    vfs: &ra_ap_vfs::Vfs,
    path: &PathBuf,
) -> Result<String> {
    // Find the file id corresponding to `path` and read its source.
    for (file_id, vfs_path) in vfs.iter() {
        if let Some(p) = file_path(vfs, file_id) {
            if p == *path {
                return file_source(host, file_id);
            }
        }
        let _ = vfs_path;
    }
    std::fs::read_to_string(path)
        .with_context(|| format!("reading {}", path.display()))
}

#[derive(Default)]
struct NameCounter {
    next: u32,
}

impl NameCounter {
    fn fresh(&mut self, lift: &PlannedLift) -> String {
        let n = self.next;
        self.next += 1;
        let line_hint: u32 = u32::from(lift.span.start());
        format!("__lift_{}_{}_{}", lift.arg_index, line_hint, n)
    }
}
