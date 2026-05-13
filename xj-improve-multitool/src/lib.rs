//! A Rustc plugin that provides several tools for improving Rust code.

#![feature(rustc_private)]

extern crate rustc_driver;
extern crate rustc_hir;
extern crate rustc_interface;
extern crate rustc_middle;
extern crate rustc_session;
extern crate rustc_span;

use std::{borrow::Cow, env, process::Command};

use std::collections::HashMap;
use std::collections::HashSet;

use clap::{Parser, ValueEnum};
use petgraph::graph::DiGraph;
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

    #[arg(long)]
    cacg_json_outdir: Option<String>,

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
                SelectedMultitool::ExtractCACG => {
                    let outdirstr = args.cacg_json_outdir.expect("cacg_json_outdir must be set");
                    let outdir = Utf8Path::new(&outdirstr);
                    if !outdir.is_dir() {
                        eprintln!("Output directory for CACG does not exist: {}", outdir);
                        return rustc_driver::Compilation::Stop;
                    }
                    match extract_condensed_approximated_call_graph(tcx, outdir) {
                        Ok(()) => rustc_driver::Compilation::Continue,
                        Err(e) => {
                            eprintln!("Failed to extract CACG: {}", e);
                            rustc_driver::Compilation::Stop
                        }
                    }
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
    /// Extracts Condensed Approximated Call Graph
    #[value(name = "ExtractCACG")]
    ExtractCACG,
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
        Some(SelectedMultitool::ExtractCACG) => {
            if args.modify_in_place {
                return MultitoolMode::ArgConflict(
                    "The --modify-in-place argument is not supported for ExtractCACG.",
                );
            }
            // if args.acgsccs_json_out.is_none() {
            //     return MultitoolMode::ArgConflict(
            //         "The --acgsccs-json-out argument is required for ExtractCACG.",
            //     );
            // }
            MultitoolMode::Selected(SelectedMultitool::ExtractCACG)
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
            span_eraser.mark_item_span_for_attribute_erasure(*def_span);
        }
    }

    span_eraser.erase_spans(tcx);
    span_eraser.print_rewritten_source(
        tcx.sess.source_map(),
        args.modify_in_place,
        args.print_to_stdout,
    );
}

// XREF:CONDENSED_SPAN_GRAPH_REPRESENTATION
#[derive(Debug, Serialize, Deserialize)]
struct ExplicitSpan {
    fileid: u32, // index into `files`
    lo: u32,
    hi: u32,
}

#[derive(Debug, Serialize, Deserialize)]
struct CondensedSpanGraph {
    files: Vec<std::path::PathBuf>,
    elts: Vec<ExplicitSpan>,
    nodes: Vec<Vec<usize>>,     // indices into `elts`
    edges: Vec<(usize, usize)>, // indices into `nodes`
}

impl CondensedSpanGraph {
    fn from(
        call_graph: DiGraph<DefId, ()>,
        defspans: &HashMap<DefId, rustc_span::Span>,
        source_map: &rustc_span::source_map::SourceMap,
    ) -> Self {
        let mut file_map: HashMap<rustc_span::StableSourceFileId, usize> = HashMap::new();
        let mut def_to_file_idx: HashMap<DefId, usize> = HashMap::new();
        let mut def_to_elt_idx: HashMap<DefId, usize> = HashMap::new();

        let mut c = CondensedSpanGraph {
            files: Vec::new(),
            elts: Vec::new(),
            nodes: Vec::new(),
            edges: Vec::new(),
        };

        // Populate the list of files with local paths, and the elements (nodes in the un-condensed graph).
        for node in call_graph.node_indices() {
            let def_id = &call_graph[node];

            let span = match defspans.get(def_id) {
                Some(span) => span,
                None => {
                    // If DefId does not exist in defspans, it's likely a generated trait method
                    continue;
                }
            };

            let srcfile = source_map.lookup_source_file(span.lo());
            #[allow(clippy::map_entry)]
            if !file_map.contains_key(&srcfile.stable_id) {
                match &srcfile.name {
                    rustc_span::FileName::Real(real) if let Some(path) = real.local_path() => {
                        let file_index = c.files.len();
                        file_map.insert(srcfile.stable_id, file_index);
                        c.files.push(path.to_path_buf());
                        def_to_file_idx.insert(*def_id, file_index);
                    }
                    _ => {
                        println!(
                            "For def {:?} found non-local file: {:?}",
                            def_id, srcfile.name
                        );
                    }
                }
            } else {
                // If we have already seen this file, we can just reuse the index.
                let file_index = file_map
                    .get(&srcfile.stable_id)
                    .expect("File should have an index");
                def_to_file_idx.insert(*def_id, *file_index);
            }

            // Now, we can try to map the DefId to a file index. If we can't,
            // it's because the DefId is not a local definition, which ought to be impossible...
            let file_idx = def_to_file_idx
                .get(def_id)
                .expect("DefId should have a file index");
            let elt_idx = c.elts.len();
            def_to_elt_idx.insert(*def_id, elt_idx);
            c.elts.push(ExplicitSpan {
                fileid: *file_idx as u32,
                lo: srcfile.relative_position(span.lo()).0,
                hi: srcfile.relative_position(span.hi()).0,
            });
        }

        // Now, the nodes and elts come from the condensed graph.
        let gvec: DiGraph<Vec<DefId>, ()> = petgraph::algo::condensation(call_graph, true);
        c.nodes = gvec
            .node_indices()
            .map(|n| {
                let mut node_idxs: Vec<usize> = Vec::new();
                for def_id in gvec[n].iter() {
                    match def_to_elt_idx.get(def_id) {
                        Some(elt_idx) => node_idxs.push(*elt_idx),
                        None => continue, // presumably a definition we don't care about, e.g. a trait method
                    };
                }
                node_idxs
            })
            .collect();

        c.edges = gvec
            .edge_indices()
            .map(|e| {
                let (a, b) = gvec.edge_endpoints(e).expect("Edge should have endpoints");
                (a.index(), b.index())
            })
            .collect();

        c
    }
}

/// Call graphs are approximated in two ways:
/// 1. They may be under-approximated, omitting some possible calls.
///    This occurs because we only consider explicitly referenced symbols,
///    so functions called via traits or dynamic dispatch may not be included.
/// 2. They may be over-approximated, including calls that are not
///    actually possible. This may happen due to higher order usages
///    which are not direct calls.
///
/// For some applications, such as wrangling the call graph for
/// top-level functions generated by c2rust, these caveats are acceptable.
fn extract_condensed_approximated_call_graph(
    tcx: TyCtxt,
    outdir: &Utf8Path,
) -> Result<(), std::io::Error> {
    let extracted = extract_graph::extract_def_graph(tcx);
    let graf = extracted.graf;

    // Construct our approximate call graph: the set of edges consisting of
    // GEdge::Mentions between function definitions.
    let call_graph: DiGraph<DefId, ()> = graf.graf.filter_map(
        |_, node| {
            if let GNode::Def(def_id) = node {
                if extracted.all_fn_defs.contains(def_id) {
                    Some(*def_id)
                } else {
                    None
                }
            } else {
                None
            }
        },
        |_, edge| {
            if let GEdge::Mentions = edge {
                Some(())
            } else {
                None
            }
        },
    );

    let condensed =
        CondensedSpanGraph::from(call_graph, &extracted.defspans, tcx.sess.source_map());

    let j = serde_json::to_string(&condensed)
        .expect("Failed to serialize condensed call graph to JSON");
    let cu64 = tcx.stable_crate_id(rustc_hir::def_id::LOCAL_CRATE).as_u64();
    let path = Utf8Path::new(&outdir).join(format!("{}.cacg.json", cu64));
    std::fs::write(&path, j)
}
