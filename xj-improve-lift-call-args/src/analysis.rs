//! Bridge layer between rust-analyzer (`ra_ap_*`) and the algorithm's
//! access/shape model.
//!
//! Responsibilities:
//! - Load a Cargo workspace into an `AnalysisHost`.
//! - For each Rust file, walk every `CallExpr` and `MethodCallExpr`.
//! - Translate AST + HIR semantics into `Access` records and per-
//!   subexpression `TyShape`s.
//!
//! All `ra_ap_*` API usage is concentrated here. If a version bump breaks
//! compilation, this is the only file to patch.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};

use ra_ap_base_db::SourceDatabase;
use ra_ap_hir::{Access as RaAccess, LangItem, Semantics, Trait, Type, attach_db};
use ra_ap_hir_def::lang_item; // unused but documents the lookup path
use ra_ap_ide::{AnalysisHost, RootDatabase};
use ra_ap_load_cargo::{LoadCargoConfig, ProcMacroServerChoice, load_workspace_at};
use ra_ap_paths::AbsPathBuf;
use ra_ap_project_model::{CargoConfig, RustLibSource};
use ra_ap_syntax::ast::{self, AstNode, HasArgList};
use ra_ap_syntax::{SyntaxKind, SyntaxNode, TextRange, WalkEvent};
use ra_ap_vfs::Vfs;

use crate::access::{Access, AccessKind, IndexKey, Place, PlaceRoot, Projection, SubexprId};
use crate::lift::{ExprForm, TyShape};

// Silence unused-import warning for the documenting use.
#[allow(dead_code)]
fn _ensure_lang_item_visible() {
    let _ = lang_item::LangItemTarget::TraitId;
}

/// One loaded workspace, ready for querying.
pub struct Workspace {
    pub host: AnalysisHost,
    pub vfs: Vfs,
}

impl Workspace {
    /// Load the workspace whose `Cargo.toml` (or workspace root) is at
    /// `manifest_dir`.
    pub fn load(manifest_dir: &Path) -> Result<Self> {
        let canon = manifest_dir
            .canonicalize()
            .with_context(|| format!("canonicalising {}", manifest_dir.display()))?;
        let _abs = AbsPathBuf::assert_utf8(canon.clone());

        // Sysroot discovery is required for `Type::is_copy` etc. to find
        // the Copy / Clone lang items; without it, every type comes back
        // as non-Copy.
        let cargo_config = CargoConfig {
            sysroot: Some(RustLibSource::Discover),
            ..CargoConfig::default()
        };
        let load_config = LoadCargoConfig {
            load_out_dirs_from_check: true,
            with_proc_macro_server: ProcMacroServerChoice::None,
            prefill_caches: false,
            num_worker_threads: 1,
            proc_macro_processes: 1,
        };

        let (db, vfs, _proc_macro) =
            load_workspace_at(&canon, &cargo_config, &load_config, &|_| {})
                .context("loading cargo workspace")?;

        let host = AnalysisHost::with_database(db);
        Ok(Workspace { host, vfs })
    }

    /// All Rust source files in the workspace's *local* source roots
    /// (i.e. the user's crates, excluding sysroot / dependencies).
    pub fn files(&self) -> Vec<(PathBuf, ra_ap_vfs::FileId)> {
        let db = self.host.raw_database();
        let mut out = Vec::new();
        for (file_id, path) in self.vfs.iter() {
            let Some(abs) = path.as_path() else { continue };
            let p: PathBuf = PathBuf::from(abs.as_str());
            if p.extension().and_then(|s| s.to_str()) != Some("rs") {
                continue;
            }
            let root_id = db.file_source_root(file_id).source_root_id(db);
            let source_root = db.source_root(root_id).source_root(db);
            if source_root.is_library {
                continue;
            }
            out.push((p, file_id));
        }
        out
    }
}

/// Per-call analysis result. The caller drives the algorithm with these.
pub struct CallAnalysis {
    pub call_span: TextRange,
    pub accesses: Vec<Access>,
    pub shapes: HashMap<SubexprId, TyShape>,
    /// Mirrors `shapes` but carries the source-text fragments and
    /// inner-place span the emitter needs.
    pub snippets: HashMap<SubexprId, SnippetData>,
}

/// Source-text + span information for one analyzed subexpression.
///
/// The `inner_place_span` is required by `emit::emit_edits` to compose
/// nested lifts: when an outer lift's RHS is the inner place of a `&place`
/// expression and another (smaller) lift falls inside that inner place,
/// we need its bounds in source coordinates to substitute correctly.
#[derive(Debug, Clone)]
pub struct SnippetData {
    /// Source span of the expression this snippet describes. Used by the
    /// lift planner to find the outer expression's span when the chosen
    /// access is an inner sub-expression of a nested call.
    pub span: TextRange,
    pub original_text: String,
    pub inner_place_text: String,
    pub inner_place_span: Option<TextRange>,
}

/// Walk a single file and produce one `CallAnalysis` per top-level call
/// site. Method calls and free-function calls are both included; nested
/// calls inside an argument are folded into the outer call's access set
/// per Step 1.3 of the spec.
pub fn analyze_file(host: &AnalysisHost, file_id: ra_ap_vfs::FileId) -> Result<Vec<CallAnalysis>> {
    let db = host.raw_database();
    // `hir::attach_db` makes the database available to the next-solver
    // interner; without it, type queries panic with "no db is attached".
    Ok(attach_db(db, || {
        let sema = Semantics::new(db);
        // Parse via Semantics so it registers the tree internally; without
        // this, `type_of_expr` panics with "Failed to lookup ... in this
        // Semantics".
        let source_file = sema.parse_guess_edition(file_id);
        let syntax = source_file.syntax().clone();

        let mut out = Vec::new();
        let mut id_gen = IdGen::default();

        // Per spec implementation note: "nested calls require recursion:
        // handle inner calls before outer". We analyze EVERY call site,
        // not only top-level ones. Each call gets its own access set and
        // its own lift plan, so a conflict that exists between two args
        // of an inner call is detected even when that inner call is
        // itself an argument to an outer call (where the merged accesses
        // would otherwise be silenced by the intra-arg-pair rule). Order
        // matters for `Step 5` purposes: process innermost-first so that
        // textual edits from inner lifts are sequenced before outer ones.
        for node in syntax.descendants() {
            if !matches!(
                node.kind(),
                SyntaxKind::CALL_EXPR | SyntaxKind::METHOD_CALL_EXPR
            ) {
                continue;
            }
            if let Some(analysis) = analyze_call(&sema, &node, &mut id_gen) {
                out.push(analysis);
            }
        }
        // Sort by nesting: innermost first (longest start offset, then
        // shortest range). This keeps edits ordered so that text changes
        // from an inner call land in the source before its enclosing call
        // is re-evaluated (which never actually re-runs in this pass —
        // but it keeps emit order deterministic and matches the spec's
        // "innermost first" guidance).
        out.sort_by_key(|a| (std::cmp::Reverse(a.call_span.start()), a.call_span.len()));
        out
    }))
}

#[derive(Default)]
struct IdGen {
    next: u32,
}

impl IdGen {
    fn fresh(&mut self) -> SubexprId {
        let id = SubexprId(self.next);
        self.next += 1;
        id
    }
}

fn analyze_call(
    sema: &Semantics<'_, RootDatabase>,
    call_node: &SyntaxNode,
    ids: &mut IdGen,
) -> Option<CallAnalysis> {
    let call_span = call_node.text_range();
    let mut accesses = Vec::new();
    let mut shapes: HashMap<SubexprId, TyShape> = HashMap::new();
    let mut snippets: HashMap<SubexprId, SnippetData> = HashMap::new();

    if let Some(method) = ast::MethodCallExpr::cast(call_node.clone()) {
        let receiver_kind = method_receiver_kind(sema, &method);
        let param_kinds: Vec<Option<AccessKind>> = sema
            .resolve_method_call(&method)
            .map(|func| {
                func.params_without_self(sema.db)
                    .iter()
                    .map(|p| param_access_kind(sema, p.ty()))
                    .map(Some)
                    .collect()
            })
            .unwrap_or_default();

        if let Some(receiver) = method.receiver() {
            walk_arg_expr(
                sema,
                receiver.syntax(),
                0,
                receiver_kind,
                None,
                ids,
                &mut accesses,
                &mut shapes,
                &mut snippets,
            );
        }
        if let Some(arg_list) = method.arg_list() {
            for (i, arg) in arg_list.args().enumerate() {
                let hint = param_kinds.get(i).copied().flatten();
                walk_arg_expr(
                    sema,
                    arg.syntax(),
                    i + 1,
                    hint,
                    None,
                    ids,
                    &mut accesses,
                    &mut shapes,
                    &mut snippets,
                );
            }
        }
    } else {
        let call = ast::CallExpr::cast(call_node.clone())?;
        // Resolve the callee to derive per-parameter access kinds. For
        // by-ref parameters we want SharedBorrow/MutBorrow even when the
        // argument is written as a bare place expression (rustc would
        // auto-ref / reborrow).
        let param_kinds: Vec<Option<AccessKind>> = call
            .expr()
            .as_ref()
            .and_then(|expr| sema.resolve_expr_as_callable(expr))
            .map(|callable| {
                callable
                    .params()
                    .iter()
                    .map(|p| param_access_kind(sema, p.ty()))
                    .map(Some)
                    .collect()
            })
            .unwrap_or_default();

        if let Some(callee) = call.expr() {
            walk_arg_expr(
                sema,
                callee.syntax(),
                0,
                None,
                None,
                ids,
                &mut accesses,
                &mut shapes,
                &mut snippets,
            );
        }
        if let Some(arg_list) = call.arg_list() {
            for (i, arg) in arg_list.args().enumerate() {
                let hint = param_kinds.get(i).copied().flatten();
                walk_arg_expr(
                    sema,
                    arg.syntax(),
                    i + 1,
                    hint,
                    None,
                    ids,
                    &mut accesses,
                    &mut shapes,
                    &mut snippets,
                );
            }
        }
    }

    Some(CallAnalysis {
        call_span,
        accesses,
        shapes,
        snippets,
    })
}

#[allow(clippy::too_many_arguments)]
fn walk_arg_expr(
    sema: &Semantics<'_, RootDatabase>,
    node: &SyntaxNode,
    arg_index: usize,
    outer_hint: Option<AccessKind>,
    // `None` ⇒ this *is* the outer expression of an argument; we use the
    // freshly-allocated id below as the outer. `Some(id)` ⇒ this is a
    // sub-expression of some larger argument expression and we propagate
    // that argument's outer id downward.
    outer_subexpr_id: Option<SubexprId>,
    ids: &mut IdGen,
    accesses: &mut Vec<Access>,
    shapes: &mut HashMap<SubexprId, TyShape>,
    snippets: &mut HashMap<SubexprId, SnippetData>,
) {
    let id = ids.fresh();
    let outer = outer_subexpr_id.unwrap_or(id);
    let span = node.text_range();
    let original_text = node.text().to_string();

    let expr = match ast::Expr::cast(node.clone()) {
        Some(e) => e,
        None => return,
    };

    let (form, place_opt, inner_place_text, inner_place_span) = classify_expr(&expr);

    // Trust the type-derived classification over the parameter-derived
    // hint when the hint disagrees on Copy-ness with the actual expression
    // type: ra_ap's Type::is_copy on a fresh `Param::ty()` sometimes loses
    // the lang-item resolution context, while `type_of_expr().is_copy()`
    // queries against the inferred type with full env. Reconcile by always
    // computing the by-value kind from the expression itself for non-ref
    // hints; reference hints (the only thing the hint adds value for) we
    // keep.
    let kind = match form {
        ExprForm::BorrowOf => AccessKind::SharedBorrow,
        ExprForm::MutBorrowOf => AccessKind::MutBorrow,
        ExprForm::NestedCall => AccessKind::Call,
        _ => match outer_hint {
            Some(k @ (AccessKind::SharedBorrow | AccessKind::MutBorrow)) => k,
            _ => derive_by_value_kind(sema, &expr),
        },
    };

    let shape = derive_shape(sema, &expr, form);

    snippets.insert(
        id,
        SnippetData {
            span,
            original_text,
            inner_place_text,
            inner_place_span,
        },
    );
    shapes.insert(id, shape);

    if let Some(place) = place_opt {
        accesses.push(Access {
            arg_index,
            place,
            kind,
            span,
            subexpr_id: id,
            outer_subexpr: outer,
        });
    } else if matches!(kind, AccessKind::Call) {
        // For nested calls, only synthesize a Call access when we can
        // identify a concrete place the call might borrow from (the join
        // of its place-typed arguments). Falling back to `Place::unknown`
        // here would conflict with every other access in the outer call,
        // spuriously lifting the callee and unrelated args whenever a
        // nested-call argument's chain has no place-typed arguments and
        // the join collapses to nothing. The recursive sub-accesses from
        // the chain's inner expressions still catch any real borrows.
        if let Some(place) = join_place_of_call(&expr) {
            accesses.push(Access {
                arg_index,
                place,
                kind: AccessKind::Call,
                span,
                subexpr_id: id,
                outer_subexpr: outer,
            });
        }
    }

    // When the argument is itself a call, walk each of its inner args (and
    // its receiver, for method calls) as full subexpressions. Without
    // this, the inner args of THIS call (e.g. `&mut x`, `(*p).f`) get no
    // access records — `recurse_children` only fires for *deeper*
    // CALL_EXPRs nested inside this one. We skip the generic
    // `recurse_children` walk in this branch because `walk_nested_call_
    // args` already covers every direct child and recurses through
    // further nested calls.
    if matches!(form, ExprForm::NestedCall) {
        walk_nested_call_args(
            sema, &expr, arg_index, outer, ids, accesses, shapes, snippets,
        );
    } else {
        recurse_children(
            sema, node, arg_index, outer, ids, accesses, shapes, snippets,
        );
    }
}

#[allow(clippy::too_many_arguments)]
fn walk_nested_call_args(
    sema: &Semantics<'_, RootDatabase>,
    expr: &ast::Expr,
    arg_index: usize,
    outer: SubexprId,
    ids: &mut IdGen,
    accesses: &mut Vec<Access>,
    shapes: &mut HashMap<SubexprId, TyShape>,
    snippets: &mut HashMap<SubexprId, SnippetData>,
) {
    match expr {
        ast::Expr::CallExpr(call) => {
            // Derive parameter hints from the inner callable so each inner
            // arg gets its borrow-kind right (auto-ref / reborrow).
            let param_kinds: Vec<Option<AccessKind>> = call
                .expr()
                .as_ref()
                .and_then(|e| sema.resolve_expr_as_callable(e))
                .map(|callable| {
                    callable
                        .params()
                        .iter()
                        .map(|p| Some(param_access_kind(sema, p.ty())))
                        .collect()
                })
                .unwrap_or_default();
            if let Some(callee) = call.expr() {
                walk_arg_expr(
                    sema,
                    callee.syntax(),
                    arg_index,
                    None,
                    Some(outer),
                    ids,
                    accesses,
                    shapes,
                    snippets,
                );
            }
            if let Some(arg_list) = call.arg_list() {
                for (i, a) in arg_list.args().enumerate() {
                    let hint = param_kinds.get(i).copied().flatten();
                    walk_arg_expr(
                        sema,
                        a.syntax(),
                        arg_index,
                        hint,
                        Some(outer),
                        ids,
                        accesses,
                        shapes,
                        snippets,
                    );
                }
            }
        }
        ast::Expr::MethodCallExpr(m) => {
            let receiver_kind = method_receiver_kind(sema, m);
            let param_kinds: Vec<Option<AccessKind>> = sema
                .resolve_method_call(m)
                .map(|func| {
                    func.params_without_self(sema.db)
                        .iter()
                        .map(|p| Some(param_access_kind(sema, p.ty())))
                        .collect()
                })
                .unwrap_or_default();
            if let Some(recv) = m.receiver() {
                walk_arg_expr(
                    sema,
                    recv.syntax(),
                    arg_index,
                    receiver_kind,
                    Some(outer),
                    ids,
                    accesses,
                    shapes,
                    snippets,
                );
            }
            if let Some(arg_list) = m.arg_list() {
                for (i, a) in arg_list.args().enumerate() {
                    let hint = param_kinds.get(i).copied().flatten();
                    walk_arg_expr(
                        sema,
                        a.syntax(),
                        arg_index,
                        hint,
                        Some(outer),
                        ids,
                        accesses,
                        shapes,
                        snippets,
                    );
                }
            }
        }
        _ => {}
    }
}

#[allow(clippy::too_many_arguments)]
fn recurse_children(
    sema: &Semantics<'_, RootDatabase>,
    node: &SyntaxNode,
    arg_index: usize,
    outer: SubexprId,
    ids: &mut IdGen,
    accesses: &mut Vec<Access>,
    shapes: &mut HashMap<SubexprId, TyShape>,
    snippets: &mut HashMap<SubexprId, SnippetData>,
) {
    for event in node.preorder() {
        if let WalkEvent::Enter(n) = event {
            if n == *node {
                continue;
            }
            match n.kind() {
                SyntaxKind::INDEX_EXPR => {
                    if let Some(ix) = ast::IndexExpr::cast(n.clone())
                        && let Some(idx_expr) = ix.index()
                    {
                        walk_arg_expr(
                            sema,
                            idx_expr.syntax(),
                            arg_index,
                            None,
                            Some(outer),
                            ids,
                            accesses,
                            shapes,
                            snippets,
                        );
                    }
                }
                SyntaxKind::CALL_EXPR | SyntaxKind::METHOD_CALL_EXPR => {
                    if let Some(sub) = analyze_call(sema, &n, ids) {
                        for mut acc in sub.accesses {
                            acc.arg_index = arg_index;
                            acc.outer_subexpr = outer;
                            accesses.push(acc);
                        }
                        for (k, v) in sub.shapes {
                            shapes.insert(k, v);
                        }
                        for (k, v) in sub.snippets {
                            snippets.insert(k, v);
                        }
                    }
                }
                _ => {}
            }
        }
    }
}

fn classify_expr(expr: &ast::Expr) -> (ExprForm, Option<Place>, String, Option<TextRange>) {
    match expr {
        ast::Expr::PathExpr(p) => {
            let place = path_to_place(p);
            (ExprForm::BarePlace, place, String::new(), None)
        }
        ast::Expr::FieldExpr(_) | ast::Expr::IndexExpr(_) => {
            let place = place_for_expr(expr);
            (ExprForm::BarePlace, place, String::new(), None)
        }
        ast::Expr::PrefixExpr(p) if p.op_kind() == Some(ast::UnaryOp::Deref) => {
            let place = place_for_expr(expr);
            (ExprForm::BarePlace, place, String::new(), None)
        }
        ast::Expr::RefExpr(r) => {
            let inner = r.expr();
            let inner_text = inner
                .as_ref()
                .map(|e| e.syntax().text().to_string())
                .unwrap_or_default();
            let inner_span = inner.as_ref().map(|e| e.syntax().text_range());
            // The place being borrowed is the inner place — without this,
            // `func(&mut x, x)` produces no access for `&mut x` and the
            // conflict is missed. The borrow-kind itself comes from
            // `form` in walk_arg_expr.
            let inner_place = inner.as_ref().and_then(place_for_expr);
            let form = if r.mut_token().is_some() {
                ExprForm::MutBorrowOf
            } else {
                ExprForm::BorrowOf
            };
            (form, inner_place, inner_text, inner_span)
        }
        ast::Expr::CallExpr(_) | ast::Expr::MethodCallExpr(_) => {
            (ExprForm::NestedCall, None, String::new(), None)
        }
        ast::Expr::CastExpr(cast) => {
            // For `&raw mut place as *mut T` and similar reference-to-raw-pointer
            // casts, the cast result is Copy (no borrow lifetime escapes), but the
            // inner `&`/`&mut`/`&raw mut` still imposes a borrow constraint on the
            // place. Look through the cast to record that place so conflicts are
            // detected. We keep form=Other so classify_lift chooses BindValue —
            // binding the whole cast expression before the call is always safe.
            if let Some(inner) = cast.expr()
                && let ast::Expr::RefExpr(_) = &inner
            {
                let (_, place_opt, _, _) = classify_expr(&inner);
                return (ExprForm::Other, place_opt, String::new(), None);
            }
            (ExprForm::Other, None, String::new(), None)
        }
        _ => (ExprForm::Other, None, String::new(), None),
    }
}

fn path_to_place(p: &ast::PathExpr) -> Option<Place> {
    let path = p.path()?;
    let segs: Vec<_> = path.segments().collect();
    if segs.len() != 1 {
        return None;
    }
    let name = segs[0].name_ref()?.text().to_string();
    Some(Place::named(name))
}

fn place_for_expr(expr: &ast::Expr) -> Option<Place> {
    // We build the projection chain outer-first while walking down the
    // AST, then reverse it. Splittability of a Field projection depends
    // on whether anything between it and the ROOT is non-splittable
    // (Deref/Index/method) — i.e., anything earlier in the *reversed*
    // chain, which is anything we encounter *later* in the walk. So we
    // can't decide splittability during the walk; we fix it up after the
    // reverse below.
    let mut chain: Vec<Projection> = Vec::new();
    let mut cur = expr.clone();
    loop {
        match cur {
            ast::Expr::FieldExpr(f) => {
                let proj = match f.field_access()? {
                    ast::FieldKind::Name(nr) => Projection::Field {
                        name: nr.text().to_string(),
                        // Fixed up below.
                        splittable: true,
                    },
                    ast::FieldKind::Index(tok) => {
                        let idx = tok.text().parse::<u32>().ok()?;
                        Projection::TupleField {
                            idx,
                            splittable: true,
                        }
                    }
                };
                chain.push(proj);
                cur = f.expr()?;
            }
            ast::Expr::IndexExpr(ix) => {
                let key = match ix.index() {
                    Some(ast::Expr::Literal(lit)) => match lit.kind() {
                        ast::LiteralKind::IntNumber(n) => n
                            .value()
                            .ok()
                            .map(|v| IndexKey::Literal(v as i128))
                            .unwrap_or(IndexKey::Opaque(0)),
                        _ => IndexKey::Opaque(0),
                    },
                    _ => IndexKey::Opaque(0),
                };
                chain.push(Projection::Index { key });
                cur = ix.base()?;
            }
            ast::Expr::PrefixExpr(p) if p.op_kind() == Some(ast::UnaryOp::Deref) => {
                chain.push(Projection::Deref);
                cur = p.expr()?;
            }
            ast::Expr::ParenExpr(pe) => {
                // Parentheses are transparent for place semantics:
                // `(*p).f` denotes the same place as `*p . f`.
                cur = pe.expr()?;
            }
            ast::Expr::PathExpr(p) => {
                let mut place = path_to_place(&p)?;
                chain.reverse();
                // Fix up Field/TupleField splittability: a field is
                // splittable iff every step from the root up to (but not
                // including) it is also splittable. Walk root-to-leaf
                // tracking whether we've seen any Deref/Index — once we
                // have, all subsequent fields are non-splittable.
                let mut after_unsplittable = false;
                for proj in chain.iter_mut() {
                    match proj {
                        Projection::Deref | Projection::Index { .. } => {
                            after_unsplittable = true;
                        }
                        Projection::Field { splittable, .. }
                        | Projection::TupleField { splittable, .. } => {
                            if after_unsplittable {
                                *splittable = false;
                            }
                        }
                    }
                }
                place.projections = chain;
                return Some(place);
            }
            _ => return None,
        }
    }
}

fn derive_by_value_kind(sema: &Semantics<'_, RootDatabase>, expr: &ast::Expr) -> AccessKind {
    if let Some(ty) = sema.type_of_expr(expr)
        && ty.original.is_copy(sema.db)
    {
        return AccessKind::Read;
    }
    AccessKind::Move
}

fn derive_shape(sema: &Semantics<'_, RootDatabase>, expr: &ast::Expr, form: ExprForm) -> TyShape {
    let ty_info = sema.type_of_expr(expr);
    let (is_copy, is_clone, is_str_or_slice_ref) = match &ty_info {
        Some(info) => {
            let t = &info.original;
            let is_copy = t.is_copy(sema.db);
            let is_clone = type_impls_clone(sema, t);
            let is_str_or_slice_ref = is_ref_to_str_or_slice(t);
            (is_copy, is_clone, is_str_or_slice_ref)
        }
        None => (false, false, false),
    };

    let (inner_is_copy, inner_is_clone) = match form {
        ExprForm::BorrowOf | ExprForm::MutBorrowOf => {
            if let ast::Expr::RefExpr(r) = expr {
                if let Some(inner) = r.expr() {
                    if let Some(ty) = sema.type_of_expr(&inner) {
                        let c = ty.original.is_copy(sema.db);
                        let cl = type_impls_clone(sema, &ty.original);
                        (c, cl)
                    } else {
                        (false, false)
                    }
                } else {
                    (false, false)
                }
            } else {
                (false, false)
            }
        }
        _ => (false, false),
    };

    let call_return_is_owned = matches!(form, ExprForm::NestedCall)
        && ty_info
            .as_ref()
            .map(|info| !info.original.is_reference())
            .unwrap_or(false);

    TyShape {
        is_copy,
        is_clone,
        is_str_or_slice_ref,
        form,
        inner_place_is_copy: inner_is_copy,
        inner_place_is_clone: inner_is_clone,
        call_return_is_owned,
    }
}

fn type_impls_clone(sema: &Semantics<'_, RootDatabase>, ty: &Type<'_>) -> bool {
    if ty.is_copy(sema.db) {
        return true;
    }
    let Some(adt) = ty.as_adt() else {
        // For non-ADT types (references, tuples, primitives) we can't query
        // impls_trait directly here. Reference types are Copy, so the early
        // return handled them. Default to false for everything else;
        // refusing to lift via .clone() is safer than calling .clone() on a
        // type that doesn't have one.
        return false;
    };
    let krate = adt.module(sema.db).krate(sema.db);
    let Some(clone_trait): Option<Trait> = Trait::lang(sema.db, krate, LangItem::Clone) else {
        return false;
    };
    ty.impls_trait(sema.db, clone_trait, &[])
}

fn is_ref_to_str_or_slice(ty: &Type<'_>) -> bool {
    if !ty.is_reference() {
        return false;
    }
    // No stable cross-version "is &str / &[T]" predicate; use the Debug
    // print as a narrow textual probe. Acceptable here because the only
    // consequence is choosing `.to_owned()` over `.clone()`.
    let s = format!("{:?}", ty);
    s.contains("str") || s.contains("Slice") || s.contains("&[")
}

/// Derive the access kind imposed on an argument by a parameter of the
/// given type. References classify as the corresponding borrow; by-value
/// Copy is `Read`, by-value non-Copy is `Move`.
fn param_access_kind(sema: &Semantics<'_, RootDatabase>, ty: &Type<'_>) -> AccessKind {
    if ty.is_mutable_reference() {
        AccessKind::MutBorrow
    } else if ty.is_reference() {
        AccessKind::SharedBorrow
    } else if ty.is_copy(sema.db) {
        AccessKind::Read
    } else {
        AccessKind::Move
    }
}

fn method_receiver_kind(
    sema: &Semantics<'_, RootDatabase>,
    method: &ast::MethodCallExpr,
) -> Option<AccessKind> {
    let func = sema.resolve_method_call(method)?;
    let self_param = func.self_param(sema.db)?;
    Some(match self_param.access(sema.db) {
        RaAccess::Exclusive => AccessKind::MutBorrow,
        RaAccess::Shared => AccessKind::SharedBorrow,
        RaAccess::Owned => AccessKind::Move,
    })
}

fn join_place_of_call(expr: &ast::Expr) -> Option<Place> {
    let args: Vec<ast::Expr> = match expr {
        ast::Expr::CallExpr(c) => c.arg_list()?.args().collect(),
        ast::Expr::MethodCallExpr(m) => {
            let mut v: Vec<ast::Expr> = Vec::new();
            if let Some(recv) = m.receiver() {
                v.push(recv);
            }
            if let Some(al) = m.arg_list() {
                v.extend(al.args());
            }
            v
        }
        _ => return None,
    };
    let mut places: Vec<Place> = args.iter().filter_map(place_for_expr).collect();
    let first = places.pop()?;
    let mut joined = first;
    for p in places {
        let n = joined.common_prefix_len(&p).unwrap_or(0);
        joined.projections.truncate(n);
        if !matches!(joined.root, PlaceRoot::Named(_)) {
            // Defensive: place_for_expr only produces Named-rooted places,
            // so this branch is unreachable today. If it ever fires,
            // return None so the caller treats it as "no resolvable place"
            // rather than "conflicts with everything".
            return None;
        }
    }
    Some(joined)
}

/// Resolve a `ra_ap_vfs::FileId` to an absolute path on disk, if it has
/// one (some files are virtual and have no on-disk location).
pub fn file_path(vfs: &Vfs, file_id: ra_ap_vfs::FileId) -> Option<PathBuf> {
    let vp = vfs.file_path(file_id);
    vp.as_path().map(|abs| PathBuf::from(abs.as_str()))
}

/// Read the on-disk source for a file via the rust-analyzer database.
pub fn file_source(host: &AnalysisHost, file_id: ra_ap_vfs::FileId) -> Result<String> {
    let db = host.raw_database();
    let text = db.file_text(file_id);
    Ok(text.text(db).to_string())
}

/// Find the statement-level insertion point for `let`-bindings inserted
/// before `call_node`. Returns `None` when the call is in an expression-
/// only context that requires `InsertSite::WrapInBlock`.
///
/// Hoisting a `let tmp = <subexpr>;` to before the enclosing statement is
/// only sound when nothing is evaluated between the start of that statement
/// and the call: otherwise we move the lifted subexpression's evaluation
/// *ahead of* a sibling's side effects (or out of a short-circuited /
/// conditional branch), which changes program semantics. Concretely, a call
/// sitting in the right operand of `||` runs only after — and conditionally
/// on — the left operand; hoisting a read of a place that the left operand
/// mutates would capture the stale, pre-mutation value.
///
/// So we walk from the call up to the statement and only keep going past a
/// parent when the call occupies that parent's *leading, unconditional*
/// evaluation position. The moment we hit an ordering/conditionality
/// boundary we return `None`, signalling the caller to wrap the call in a
/// block (`{ let tmp = ...; <call> }`) at its original site, which preserves
/// both evaluation order and conditionality.
pub fn statement_insertion_point(call_node: &SyntaxNode) -> Option<ra_ap_syntax::TextSize> {
    let mut child = call_node.clone();
    while let Some(p) = child.parent() {
        match p.kind() {
            SyntaxKind::EXPR_STMT | SyntaxKind::LET_STMT => {
                return Some(p.text_range().start());
            }
            SyntaxKind::BLOCK_EXPR => {
                return None;
            }
            _ => {
                if !child_is_leading_unconditional(&p, &child) {
                    return None;
                }
            }
        }
        child = p;
    }
    None
}

/// Whether `child` occupies the position within `parent` that is evaluated
/// first and unconditionally — i.e. nothing in `parent` runs before `child`,
/// and `child` always runs whenever `parent` does. Unrecognized parents are
/// treated conservatively as `false` (forcing a block wrap), since
/// over-wrapping is always semantics-preserving while over-hoisting is not.
fn child_is_leading_unconditional(parent: &SyntaxNode, child: &SyntaxNode) -> bool {
    let is =
        |e: Option<ast::Expr>| e.is_some_and(|e| e.syntax().text_range() == child.text_range());
    match parent.kind() {
        // Transparent wrappers: their sole operand evaluates first.
        SyntaxKind::PAREN_EXPR => is(ast::ParenExpr::cast(parent.clone()).and_then(|e| e.expr())),
        SyntaxKind::CAST_EXPR => is(ast::CastExpr::cast(parent.clone()).and_then(|e| e.expr())),
        SyntaxKind::REF_EXPR => is(ast::RefExpr::cast(parent.clone()).and_then(|e| e.expr())),
        SyntaxKind::PREFIX_EXPR => is(ast::PrefixExpr::cast(parent.clone()).and_then(|e| e.expr())),
        // `a OP b` (including `&&` / `||`): only the left operand is leading
        // and unconditional; the right runs after it and, for the logical
        // operators, only conditionally.
        SyntaxKind::BIN_EXPR => is(ast::BinExpr::cast(parent.clone()).and_then(|e| e.lhs())),
        // `base[index]`: the base is evaluated before the index.
        SyntaxKind::INDEX_EXPR => is(ast::IndexExpr::cast(parent.clone()).and_then(|e| e.base())),
        // `recv.field`: the receiver evaluates first.
        SyntaxKind::FIELD_EXPR => is(ast::FieldExpr::cast(parent.clone()).and_then(|e| e.expr())),
        // `if <cond> { .. }`: the condition is evaluated first and
        // unconditionally; the branches are conditional.
        SyntaxKind::IF_EXPR => is(ast::IfExpr::cast(parent.clone()).and_then(|e| e.condition())),
        _ => false,
    }
}

#[cfg(test)]
mod insertion_point_tests {
    use super::*;
    use ra_ap_syntax::{Edition, SourceFile};

    /// Parse `src`, find the call to `git__add` whose argument list contains
    /// `needle`, and return the statement insertion point for it.
    fn insertion_for_call_containing(src: &str, needle: &str) -> Option<ra_ap_syntax::TextSize> {
        let parse = SourceFile::parse(src, Edition::CURRENT);
        let file = parse.tree();
        let call = file
            .syntax()
            .descendants()
            .filter(|n| n.kind() == SyntaxKind::CALL_EXPR)
            .find(|n| {
                let t = n.text().to_string();
                t.contains("git__add") && t.contains(needle)
            })
            .expect("call not found");
        statement_insertion_point(&call)
    }

    #[test]
    fn hoist_past_short_circuit_and_side_effect_wraps_in_block() {
        // The xilca reproducer: the second `git__add` call reads `alloc_size`
        // in the RHS of `||`, after the first call has written it via
        // `&raw mut alloc_size`. Hoisting a `let` before the whole `if`
        // would capture the stale value, so we must NOT return a statement
        // insertion point — the caller wraps the call in a block instead.
        let src = r#"
fn f(alloc_size: usize, name_len: usize) {
    if git__add(&raw mut alloc_size, 16, name_len) != 0
        || git__add(&raw mut alloc_size, alloc_size, 1) != 0
    {
        return;
    }
}
"#;
        assert_eq!(
            insertion_for_call_containing(src, "alloc_size, 1"),
            None,
            "call in the RHS of `||` must force a block wrap, not a hoist",
        );
    }

    #[test]
    fn call_that_is_the_statement_hoists() {
        // A call sitting directly in statement position has nothing evaluated
        // before it, so hoisting a `let` ahead of it is sound.
        let src = r#"
fn f(p: usize) {
    git__add(&raw mut p, p, 1);
}
"#;
        assert!(
            insertion_for_call_containing(src, "p, 1").is_some(),
            "a call in plain statement position should hoist",
        );
    }

    #[test]
    fn call_through_leading_cast_hoists() {
        // `call as T;` in statement position: the call is the cast's operand,
        // evaluated first and unconditionally, so hoisting remains sound.
        let src = r#"
fn f(p: usize) {
    git__add(&raw mut p, p, 1) as i32;
}
"#;
        assert!(
            insertion_for_call_containing(src, "p, 1").is_some(),
            "a call under a leading cast should still hoist",
        );
    }
}
