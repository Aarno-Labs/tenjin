//! A Rustc plugin that provides several tools for improving Rust code.

#![feature(rustc_private)]

extern crate rustc_driver;
extern crate rustc_hir;
extern crate rustc_interface;
extern crate rustc_middle;
extern crate rustc_session;
extern crate rustc_span;

use std::{borrow::Cow, env, process::Command};

use std::collections::HashSet;

use clap::{Parser, ValueEnum};
use rustc_hir::def_id::DefId;
use rustc_middle::ty::TyCtxt;
use rustc_plugin::{CrateFilter, RustcPlugin, RustcPluginArgs, Utf8Path};
use serde::{Deserialize, Serialize};

mod extract_graph;
mod span_eraser;

use extract_graph::{GEdge, GNode};
use span_eraser::SpanEraser;

pub struct XjImproveMultitoolPlugin;

#[derive(Parser, Serialize, Deserialize)]
pub struct XjImproveMultitoolPluginArgs {
    #[arg(long)]
    modify_in_place: bool,

    #[arg(long)]
    print_to_stdout: bool,

    #[arg(long, value_enum)]
    tool: Option<SelectedMultitool>,

    #[clap(last = true)]
    cargo_args: Vec<String>,
}

impl RustcPlugin for XjImproveMultitoolPlugin {
    type Args = XjImproveMultitoolPluginArgs;

    fn version(&self) -> Cow<'static, str> {
        env!("CARGO_PKG_VERSION").into()
    }

    fn driver_name(&self) -> Cow<'static, str> {
        "xj-improve-multitool-driver".into()
    }

    fn args(&self, _target_dir: &Utf8Path) -> RustcPluginArgs<Self::Args> {
        let args = XjImproveMultitoolPluginArgs::parse_from(env::args().skip(1));
        let filter = CrateFilter::OnlyWorkspace;
        RustcPluginArgs { args, filter }
    }

    // Pass Cargo arguments (like --feature) from the top-level CLI to Cargo.
    fn modify_cargo(&self, cargo: &mut Command, args: &Self::Args) {
        cargo.args(&args.cargo_args);
    }

    fn run(
        self,
        compiler_args: Vec<String>,
        plugin_args: Self::Args,
    ) -> rustc_interface::interface::Result<()> {
        let mut callbacks = XjImproveMultitoolCallbacks {
            args: Some(plugin_args),
        };
        rustc_driver::run_compiler(&compiler_args, &mut callbacks);
        Ok(())
    }
}

struct XjImproveMultitoolCallbacks {
    args: Option<XjImproveMultitoolPluginArgs>,
}

impl rustc_driver::Callbacks for XjImproveMultitoolCallbacks {
    fn after_analysis(
        &mut self,
        _compiler: &rustc_interface::interface::Compiler,
        tcx: TyCtxt<'_>,
    ) -> rustc_driver::Compilation {
        let args = self.args.take().expect("Plugin args should be set");
        match determine_multitool_mode(&args) {
            MultitoolMode::NoToolSelected => {
                eprintln!("No tool selected. Use --help to see available options.");
                rustc_driver::Compilation::Stop
            }
            MultitoolMode::ArgConflict(note) => {
                eprintln!(
                    "Argument conflict detected for xj-improve-multitool: {}",
                    note
                );
                rustc_driver::Compilation::Stop
            }
            MultitoolMode::Selected(multitool) => match multitool {
                SelectedMultitool::ExtractUCGSCCs => {
                    eprintln!(
                        "Extracting Under-approximated Call Graph SCCs is not implemented yet."
                    );
                    rustc_driver::Compilation::Stop
                }
                SelectedMultitool::TrimDeadItems => {
                    trim_dead_items(tcx, args);

                    rustc_driver::Compilation::Continue
                }
            },
        }
    }
}

#[derive(Clone, Copy, Debug, ValueEnum, Deserialize, Serialize)]
enum SelectedMultitool {
    /// Trims dead items from the crate.
    #[value(name = "TrimDeadItems")]
    TrimDeadItems,
    /// Extracts Under-approximated Call Graph SCCs
    #[value(name = "ExtractUCGSCCs")]
    ExtractUCGSCCs,
}

enum MultitoolMode {
    NoToolSelected,
    ArgConflict(&'static str),
    Selected(SelectedMultitool),
}

fn determine_multitool_mode(args: &XjImproveMultitoolPluginArgs) -> MultitoolMode {
    match args.tool {
        Some(SelectedMultitool::TrimDeadItems) => {
            if !args.modify_in_place && !args.print_to_stdout {
                return MultitoolMode::ArgConflict(
                    "TrimDeadItems should specify either --modify-in-place or --print-to-stdout.",
                );
            }
            MultitoolMode::Selected(SelectedMultitool::TrimDeadItems)
        }
        Some(SelectedMultitool::ExtractUCGSCCs) => {
            if args.modify_in_place {
                return MultitoolMode::ArgConflict(
                    "The --modify-in-place argument is not supported for ExtractUCGSCCs.",
                );
            }
            MultitoolMode::Selected(SelectedMultitool::ExtractUCGSCCs)
        }
        None => MultitoolMode::NoToolSelected,
    }
}

fn trim_dead_items(tcx: TyCtxt, args: XjImproveMultitoolPluginArgs) {
    let extracted = extract_graph::extract_def_graph(tcx);
    let mut graf = extracted.graf;
    let defspans = extracted.defspans;

    // For executable crates, the entry function will be the only root.
    // For library crates, extraction will have added virtual roots for public items.
    if let Some((main_def_id, _)) = tcx.entry_fn(()) {
        graf.update_edge(GNode::VirtualRoot, GNode::Def(main_def_id), GEdge::Mentions);
    }

    // Run DFS, marking all reachable crate-local definitions.
    let mut dpo = petgraph::visit::DfsPostOrder::new(
        &graf.graf,
        *graf
            .grafnodes
            .get(&GNode::VirtualRoot)
            .expect("Virtual root should exist"),
    );
    let mut live: HashSet<DefId> = HashSet::new();
    while let Some(nx) = dpo.next(&graf.graf) {
        let node = graf.graf[nx];
        match node {
            GNode::Def(def_id) => {
                live.insert(def_id);
            }
            GNode::VirtualRoot => {}
        }
    }

    // Collect unreachable crate-local definitions.
    let mut crate_dead = HashSet::new();
    for node in graf.grafnodes.keys() {
        match node {
            GNode::Def(def_id) => {
                let def = *def_id;
                if def.is_local() && !live.contains(&def) {
                    crate_dead.insert(def);
                }
            }
            GNode::VirtualRoot => (),
        };
    }

    // Now that we have the set of dead crate-local definitions,
    // we can erase their spans from the source code.
    let mut span_eraser = SpanEraser::new();

    for def in crate_dead {
        if let Some(def_span) = defspans.get(&def) {
            span_eraser.add_span(*def_span);

            if extracted.top_item_defs.contains(&def) {
                span_eraser.mark_top_item_span_for_attribute_erasure(*def_span);
            }
        }
    }

    span_eraser.erase_spans(tcx);
    span_eraser.print_rewritten_source(
        tcx.sess.source_map(),
        args.modify_in_place,
        args.print_to_stdout,
    );
}
