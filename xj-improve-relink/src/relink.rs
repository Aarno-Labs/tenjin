//! Cross-module relinking.
//!
//! When one module imports a C symbol through an `extern "C" { fn foo(); }`
//! block and another module in the same workspace *exports* that same
//! symbol (a `#[no_mangle] pub extern "C" fn foo`), the call can be wired
//! directly to the Rust definition instead of going through the foreign
//! (and therefore `unsafe`) declaration.
//!
//! This module builds an index of exported symbols across the whole
//! workspace, then rewrites each importing module's calls to point at the
//! exporting module and removes the now-redundant `extern "C"` entries.

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;

use syn::visit_mut::{self, VisitMut};

use crate::collect::CrateUnit;

/// A request to add a cross-package dependency to a manifest.
#[derive(Debug, Clone)]
pub struct DepRequest {
    /// Directory of the importing package's `Cargo.toml`.
    pub importer_pkg_dir: PathBuf,
    /// Package name of the exporting crate (the `[dependencies]` key).
    pub dep_package: String,
    /// Directory of the exporting package (target of the `path = …`).
    pub dep_pkg_dir: PathBuf,
}

/// Where an exported symbol can be reached, as a module path within its
/// defining crate target.
#[derive(Debug, Clone)]
struct ExportTarget {
    /// Index into the workspace `units` slice of the defining crate target.
    unit_idx: usize,
    /// Module path from the crate root to the item, e.g. `["src", "lib"]`.
    module_segments: Vec<String>,
    /// Identifier of the callable function.
    fn_ident: String,
}

/// Index of exported C link-symbols across the workspace.
type ExportIndex = HashMap<String, ExportTarget>;

/// A function definition discovered while scanning a crate target,
/// recorded so that `_xj_ffi` passthrough wrappers can be resolved to the
/// underlying safe function.
#[derive(Default)]
struct FnDefs {
    /// `(module_segments, fn_ident)` of every `pub` free function.
    pub_fns: HashSet<(Vec<String>, String)>,
}

/// Run relinking over an already-collected workspace, mutating ASTs in
/// place. Returns the dependency edits that callers should apply to the
/// affected `Cargo.toml` manifests.
pub fn relink_workspace(units: &mut [CrateUnit]) -> Vec<DepRequest> {
    let index = build_export_index(units);

    // Pass A (read-only): plan every importer's rewrites.
    let plans: Vec<Plan> = (0..units.len())
        .map(|importer_idx| plan_unit(units, &index, importer_idx))
        .collect();

    // Pass B (mutating): apply each plan to its importing unit.
    let mut deps = Vec::new();
    for plan in plans {
        if plan.renames.is_empty() {
            continue;
        }
        apply_plan(&mut units[plan.importer_idx], &plan);
        deps.extend(plan.deps);
    }
    dedup_deps(deps)
}

// ── Export index ─────────────────────────────────────────────────────

fn build_export_index(units: &[CrateUnit]) -> ExportIndex {
    let mut index = ExportIndex::new();
    for (unit_idx, unit) in units.iter().enumerate() {
        let mut fn_defs = FnDefs::default();
        let mut exports: Vec<RawExport> = Vec::new();
        for file in &unit.files {
            scan_items_for_exports(&file.modpath, &file.ast.items, &mut fn_defs, &mut exports);
        }
        for raw in exports {
            let target = resolve_export(unit_idx, &raw, &fn_defs);
            // First definition of a symbol wins; duplicates would be a
            // link error in C anyway.
            index.entry(raw.link_name.clone()).or_insert(target);
        }
    }
    index
}

/// An exported `#[no_mangle] extern "C" fn` found during scanning, before
/// passthrough resolution.
struct RawExport {
    link_name: String,
    /// Module path of the `#[no_mangle]` wrapper item.
    wrapper_module: Vec<String>,
    /// Identifier of the `#[no_mangle]` wrapper.
    wrapper_ident: String,
    /// If the body is a trivial `super::NAME(..)` passthrough, the callee
    /// identifier `NAME`.
    passthrough: Option<String>,
}

fn scan_items_for_exports(
    modpath: &[String],
    items: &[syn::Item],
    fn_defs: &mut FnDefs,
    exports: &mut Vec<RawExport>,
) {
    for item in items {
        match item {
            syn::Item::Fn(f) => {
                let is_pub = matches!(f.vis, syn::Visibility::Public(_));
                if is_pub {
                    fn_defs
                        .pub_fns
                        .insert((modpath.to_vec(), f.sig.ident.to_string()));
                }
                if is_pub
                    && is_extern_c(&f.sig.abi)
                    && let Some(link_name) = export_link_name(&f.attrs, &f.sig.ident.to_string())
                {
                    exports.push(RawExport {
                        link_name,
                        wrapper_module: modpath.to_vec(),
                        wrapper_ident: f.sig.ident.to_string(),
                        passthrough: passthrough_callee(&f.block),
                    });
                }
            }
            syn::Item::Mod(m) => {
                if let Some((_, ref inner)) = m.content {
                    let mut child = modpath.to_vec();
                    child.push(m.ident.to_string());
                    scan_items_for_exports(&child, inner, fn_defs, exports);
                }
            }
            _ => {}
        }
    }
}

fn resolve_export(unit_idx: usize, raw: &RawExport, fn_defs: &FnDefs) -> ExportTarget {
    // Prefer the underlying safe function when the export is a recognized
    // trivial `_xj_ffi` passthrough wrapper whose target is a `pub` fn in
    // the parent module.
    if let Some(callee) = &raw.passthrough
        && !raw.wrapper_module.is_empty()
    {
        let parent = raw.wrapper_module[..raw.wrapper_module.len() - 1].to_vec();
        if fn_defs.pub_fns.contains(&(parent.clone(), callee.clone())) {
            return ExportTarget {
                unit_idx,
                module_segments: parent,
                fn_ident: callee.clone(),
            };
        }
    }
    // Fallback: reference the `#[no_mangle]` export item directly.
    ExportTarget {
        unit_idx,
        module_segments: raw.wrapper_module.clone(),
        fn_ident: raw.wrapper_ident.clone(),
    }
}

// ── Planning ─────────────────────────────────────────────────────────

struct Plan {
    importer_idx: usize,
    /// symbol name -> replacement path expression target.
    renames: Vec<(String, syn::Path)>,
    /// names whose foreign declarations should be removed.
    matched_names: HashSet<String>,
    deps: Vec<DepRequest>,
}

fn plan_unit(units: &[CrateUnit], index: &ExportIndex, importer_idx: usize) -> Plan {
    let importer = &units[importer_idx];
    let mut renames = Vec::new();
    let mut matched_names = HashSet::new();
    let mut deps = Vec::new();

    // Collect the foreign symbols this unit imports.
    let mut imported = HashSet::new();
    for file in &importer.files {
        collect_foreign_fn_names(&file.ast.items, &mut imported);
    }

    for name in imported {
        let Some(target) = index.get(&name) else {
            continue;
        };
        let Some((path, dep)) = resolve_path(units, importer_idx, target) else {
            continue;
        };
        matched_names.insert(name.clone());
        renames.push((name, path));
        if let Some(dep) = dep {
            deps.push(dep);
        }
    }

    Plan {
        importer_idx,
        renames,
        matched_names,
        deps,
    }
}

/// Build the call path for `target` as seen from `importer_idx`, plus any
/// cross-package dependency that path requires. Returns `None` if the
/// export is not reachable (e.g. it lives in a non-library target of a
/// different package).
fn resolve_path(
    units: &[CrateUnit],
    importer_idx: usize,
    target: &ExportTarget,
) -> Option<(syn::Path, Option<DepRequest>)> {
    let importer = &units[importer_idx];
    let exporter = &units[target.unit_idx];

    let mut segments: Vec<String> = Vec::new();
    let mut dep = None;

    if exporter.target.src_path == importer.target.src_path {
        // Same crate target.
        segments.push("crate".to_string());
    } else {
        // Different target: reference the exporter by its lib crate name.
        if !exporter.target.is_linkable_lib() {
            return None;
        }
        segments.push(exporter.target.extern_crate_name());
        if exporter.target.package_name != importer.target.package_name {
            dep = Some(DepRequest {
                importer_pkg_dir: importer.target.package_dir.clone(),
                dep_package: exporter.target.package_name.clone(),
                dep_pkg_dir: exporter.target.package_dir.clone(),
            });
        }
    }

    segments.extend(target.module_segments.iter().cloned());
    segments.push(target.fn_ident.clone());

    let joined = segments.join("::");
    let path = syn::parse_str::<syn::Path>(&joined).ok()?;
    Some((path, dep))
}

// ── Applying a plan ──────────────────────────────────────────────────

fn apply_plan(unit: &mut CrateUnit, plan: &Plan) {
    let map: HashMap<String, syn::Path> = plan.renames.iter().cloned().collect();
    for file in &mut unit.files {
        let mut rewriter = CallRewriter { map: &map };
        rewriter.visit_file_mut(&mut file.ast);
        remove_foreign_fns(&mut file.ast.items, &plan.matched_names);
    }
}

/// Replaces single-segment path expressions whose identifier names a
/// relinked symbol with the resolved multi-segment path. This covers both
/// direct calls (`foo(..)` — the callee is itself a path expression) and
/// bare function-pointer references (`foo`).
struct CallRewriter<'a> {
    map: &'a HashMap<String, syn::Path>,
}

impl VisitMut for CallRewriter<'_> {
    fn visit_expr_mut(&mut self, expr: &mut syn::Expr) {
        if let syn::Expr::Path(ep) = expr
            && ep.qself.is_none()
            && ep.path.leading_colon.is_none()
            && ep.path.segments.len() == 1
        {
            let seg = &ep.path.segments[0];
            if seg.arguments.is_none() {
                let ident = seg.ident.to_string();
                if let Some(target) = self.map.get(&ident) {
                    ep.path = target.clone();
                    return;
                }
            }
        }
        visit_mut::visit_expr_mut(self, expr);
    }

    fn visit_macro_mut(&mut self, mac: &mut syn::Macro) {
        // `syn` exposes macro arguments as an opaque token stream, so the
        // expression visitor never sees calls written inside a macro (e.g.
        // `println!("{}", foo())`). Rewrite the bare identifiers directly in
        // the token stream instead.
        mac.tokens = rewrite_tokens(std::mem::take(&mut mac.tokens), self.map);
    }
}

/// Replace any bare identifier token naming a relinked symbol with the
/// resolved path, recursing through delimiter groups. Identifiers directly
/// preceded by `.` (method/field access) are left alone.
fn rewrite_tokens(
    tokens: proc_macro2::TokenStream,
    map: &HashMap<String, syn::Path>,
) -> proc_macro2::TokenStream {
    use proc_macro2::{Group, TokenStream, TokenTree};
    use quote::ToTokens;

    let mut out = TokenStream::new();
    let mut prev_was_dot = false;
    for tt in tokens {
        match tt {
            TokenTree::Group(g) => {
                let inner = rewrite_tokens(g.stream(), map);
                let mut new_group = Group::new(g.delimiter(), inner);
                new_group.set_span(g.span());
                out.extend(std::iter::once(TokenTree::Group(new_group)));
                prev_was_dot = false;
            }
            TokenTree::Ident(id) => {
                if !prev_was_dot && let Some(target) = map.get(&id.to_string()) {
                    target.to_tokens(&mut out);
                } else {
                    out.extend(std::iter::once(TokenTree::Ident(id)));
                }
                prev_was_dot = false;
            }
            TokenTree::Punct(p) => {
                prev_was_dot = p.as_char() == '.';
                out.extend(std::iter::once(TokenTree::Punct(p)));
            }
            TokenTree::Literal(l) => {
                out.extend(std::iter::once(TokenTree::Literal(l)));
                prev_was_dot = false;
            }
        }
    }
    out
}

/// Remove `extern "C"` foreign function declarations whose name is in
/// `names`, dropping any `extern` block left empty, recursing into inline
/// modules.
fn remove_foreign_fns(items: &mut Vec<syn::Item>, names: &HashSet<String>) {
    items.retain_mut(|item| match item {
        syn::Item::ForeignMod(fm) => {
            fm.items.retain(|fi| match fi {
                syn::ForeignItem::Fn(f) => !names.contains(&f.sig.ident.to_string()),
                _ => true,
            });
            !fm.items.is_empty()
        }
        syn::Item::Mod(m) => {
            if let Some((_, ref mut inner)) = m.content {
                remove_foreign_fns(inner, names);
            }
            true
        }
        _ => true,
    });
}

fn collect_foreign_fn_names(items: &[syn::Item], out: &mut HashSet<String>) {
    for item in items {
        match item {
            syn::Item::ForeignMod(fm) if is_extern_c(&Some(fm.abi.clone())) => {
                for fi in &fm.items {
                    if let syn::ForeignItem::Fn(f) = fi {
                        out.insert(f.sig.ident.to_string());
                    }
                }
            }
            syn::Item::Mod(m) => {
                if let Some((_, ref inner)) = m.content {
                    collect_foreign_fn_names(inner, out);
                }
            }
            _ => {}
        }
    }
}

// ── Attribute / signature helpers ────────────────────────────────────

/// `true` if the ABI is `extern "C"` (or a bare `extern`, which defaults
/// to the C ABI).
fn is_extern_c(abi: &Option<syn::Abi>) -> bool {
    match abi {
        Some(abi) => match &abi.name {
            Some(name) => name.value() == "C",
            None => true,
        },
        None => false,
    }
}

/// Returns the exported link name for a function if it carries a
/// `no_mangle` / `export_name` marker (including the `unsafe(..)` forms),
/// otherwise `None`.
fn export_link_name(attrs: &[syn::Attribute], fn_ident: &str) -> Option<String> {
    let mut exported = false;
    let mut explicit: Option<String> = None;
    for attr in attrs {
        scan_export_meta(&attr.meta, &mut exported, &mut explicit);
    }
    if explicit.is_some() {
        explicit
    } else if exported {
        Some(fn_ident.to_string())
    } else {
        None
    }
}

fn scan_export_meta(meta: &syn::Meta, exported: &mut bool, explicit: &mut Option<String>) {
    match meta {
        syn::Meta::Path(p) => {
            if p.is_ident("no_mangle") {
                *exported = true;
            }
        }
        syn::Meta::NameValue(nv) => {
            if nv.path.is_ident("export_name")
                && let syn::Expr::Lit(syn::ExprLit {
                    lit: syn::Lit::Str(s),
                    ..
                }) = &nv.value
            {
                *explicit = Some(s.value());
            }
        }
        syn::Meta::List(list) => {
            // Edition 2024 spelling: `#[unsafe(no_mangle)]` /
            // `#[unsafe(export_name = "..")]`.
            if list.path.is_ident("unsafe")
                && let Ok(inner) = list.parse_args::<syn::Meta>()
            {
                scan_export_meta(&inner, exported, explicit);
            }
        }
    }
}

/// If `block` is a trivial passthrough of the form `super::NAME(..)`
/// (optionally `return super::NAME(..);`), returns `NAME`.
fn passthrough_callee(block: &syn::Block) -> Option<String> {
    if block.stmts.len() != 1 {
        return None;
    }
    let call_expr = match &block.stmts[0] {
        syn::Stmt::Expr(expr, _) => match expr {
            syn::Expr::Return(ret) => ret.expr.as_deref()?,
            other => other,
        },
        _ => return None,
    };
    let syn::Expr::Call(call) = call_expr else {
        return None;
    };
    let syn::Expr::Path(ep) = call.func.as_ref() else {
        return None;
    };
    let segs: Vec<String> = ep
        .path
        .segments
        .iter()
        .map(|s| s.ident.to_string())
        .collect();
    if segs.len() == 2 && segs[0] == "super" {
        Some(segs[1].clone())
    } else {
        None
    }
}

fn dedup_deps(deps: Vec<DepRequest>) -> Vec<DepRequest> {
    let mut seen = HashSet::new();
    deps.into_iter()
        .filter(|d| {
            seen.insert((
                d.importer_pkg_dir.clone(),
                d.dep_package.clone(),
                d.dep_pkg_dir.clone(),
            ))
        })
        .collect()
}
