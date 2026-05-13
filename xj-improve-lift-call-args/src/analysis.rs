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

        for node in syntax.descendants() {
            if !matches!(
                node.kind(),
                SyntaxKind::CALL_EXPR | SyntaxKind::METHOD_CALL_EXPR
            ) {
                continue;
            }
            if has_enclosing_call(&node) {
                continue;
            }
            if let Some(analysis) = analyze_call(&sema, &node, &mut id_gen) {
                out.push(analysis);
            }
        }
        out
    }))
}

fn has_enclosing_call(node: &SyntaxNode) -> bool {
    let mut cur = node.parent();
    while let Some(p) = cur {
        if matches!(p.kind(), SyntaxKind::CALL_EXPR | SyntaxKind::METHOD_CALL_EXPR) {
            return true;
        }
        if matches!(
            p.kind(),
            SyntaxKind::BLOCK_EXPR | SyntaxKind::FN | SyntaxKind::EXPR_STMT
        ) {
            return false;
        }
        cur = p.parent();
    }
    false
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
                    ids,
                    &mut accesses,
                    &mut shapes,
                    &mut snippets,
                );
            }
        }
    } else if let Some(call) = ast::CallExpr::cast(call_node.clone()) {
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
                    ids,
                    &mut accesses,
                    &mut shapes,
                    &mut snippets,
                );
            }
        }
    } else {
        return None;
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
    ids: &mut IdGen,
    accesses: &mut Vec<Access>,
    shapes: &mut HashMap<SubexprId, TyShape>,
    snippets: &mut HashMap<SubexprId, SnippetData>,
) {
    let id = ids.fresh();
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
            });
        }
    }

    recurse_children(sema, node, arg_index, ids, accesses, shapes, snippets);
}

#[allow(clippy::too_many_arguments)]
fn recurse_children(
    sema: &Semantics<'_, RootDatabase>,
    node: &SyntaxNode,
    arg_index: usize,
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
                    if let Some(ix) = ast::IndexExpr::cast(n.clone()) {
                        if let Some(idx_expr) = ix.index() {
                            walk_arg_expr(
                                sema,
                                idx_expr.syntax(),
                                arg_index,
                                None,
                                ids,
                                accesses,
                                shapes,
                                snippets,
                            );
                        }
                    }
                }
                SyntaxKind::CALL_EXPR | SyntaxKind::METHOD_CALL_EXPR => {
                    if let Some(sub) = analyze_call(sema, &n, ids) {
                        for mut acc in sub.accesses {
                            acc.arg_index = arg_index;
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
    let mut chain: Vec<Projection> = Vec::new();
    let mut cur = expr.clone();
    let mut saw_deref_or_index = false;
    loop {
        match cur {
            ast::Expr::FieldExpr(f) => {
                let splittable = !saw_deref_or_index;
                let proj = match f.field_access()? {
                    ast::FieldKind::Name(nr) => Projection::Field {
                        name: nr.text().to_string(),
                        splittable,
                    },
                    ast::FieldKind::Index(tok) => {
                        let idx = tok.text().parse::<u32>().ok()?;
                        Projection::TupleField { idx, splittable }
                    }
                };
                chain.push(proj);
                cur = f.expr()?;
            }
            ast::Expr::IndexExpr(ix) => {
                saw_deref_or_index = true;
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
                saw_deref_or_index = true;
                chain.push(Projection::Deref);
                cur = p.expr()?;
            }
            ast::Expr::PathExpr(p) => {
                let mut place = path_to_place(&p)?;
                chain.reverse();
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
pub fn statement_insertion_point(call_node: &SyntaxNode) -> Option<ra_ap_syntax::TextSize> {
    let mut cur = call_node.parent();
    while let Some(p) = cur {
        match p.kind() {
            SyntaxKind::EXPR_STMT | SyntaxKind::LET_STMT => {
                return Some(p.text_range().start());
            }
            SyntaxKind::BLOCK_EXPR => {
                return None;
            }
            _ => {}
        }
        cur = p.parent();
    }
    None
}
