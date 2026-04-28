//! `xj-improve-synsub` -- a framework for rewriting Rust source code using `syn`.
//!
//! The [`Rewriter`] struct is the core engine.  Register expression and
//! statement rewrite functions, then call [`Rewriter::rewrite_crate`] to apply
//! them across every file in an input crate.
//!
//! Each rewrite is an ordinary function (or a method on [`Rewriter`] referenced
//! as `Rewriter::method_name`).  Rewrites receive the rewriter -- giving
//! access to [`Rewriter::add_dep`] -- together with the current
//! [`SymbolTable`] and the AST node to inspect.
//!
//! The symbol table is maintained by the traversal: global variables
//! (statics and consts) are collected in a pre-pass, then function
//! arguments and let-bindings are added as the visitor enters scopes.

pub mod rewrites;

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result};
use serde::Deserialize;
use syn::visit_mut::{self, VisitMut};

// ── Depth ────────────────────────────────────────────────────────────

/// Controls how deeply into the AST the rewriting process proceeds.
///
/// When a rewrite succeeds it returns a `Depth` that governs further
/// rewriting *within the replacement* node.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Depth {
    /// No limit on rewriting depth.
    Unlimited,
    /// Rewrite at most `n` more nesting levels of statements/expressions.
    Limited(u32),
}

impl Depth {
    /// Returns the depth for one nesting level deeper, or `None` if we
    /// have reached the limit.
    pub fn descend(self) -> Option<Depth> {
        match self {
            Depth::Unlimited => Some(Depth::Unlimited),
            Depth::Limited(0) => None,
            Depth::Limited(n) => Some(Depth::Limited(n - 1)),
        }
    }
}

// ── Symbol table ─────────────────────────────────────────────────────

/// A scope-aware table of symbols mapping names to their [`syn::Type`].
///
/// Maintained by the rewriting traversal: global variables (statics and
/// consts) are collected in a pre-pass, then function arguments and
/// let-binding statements are added as the visitor descends into scopes.
/// The table is provided as an immutable reference to each rewrite
/// function.
#[derive(Debug, Clone, Default)]
pub struct SymbolTable {
    /// Maps a symbol name to its type.
    pub items: HashMap<String, syn::Type>,
}

impl SymbolTable {
    pub fn new() -> Self {
        Self::default()
    }

    /// Returns `true` if the table contains a symbol with the given name.
    pub fn has(&self, name: &str) -> bool {
        self.items.contains_key(name)
    }

    /// Returns the type associated with `name`, if any.
    pub fn get(&self, name: &str) -> Option<&syn::Type> {
        self.items.get(name)
    }

    /// Insert a name -> type mapping.
    pub fn insert(&mut self, name: String, ty: syn::Type) {
        self.items.insert(name, ty);
    }

    /// Build a symbol table of global variables (statics and consts) by
    /// scanning every file.  Inline modules are descended recursively.
    pub fn from_files(files: &[(PathBuf, syn::File)]) -> Self {
        let mut table = Self::new();
        for (_, file) in files {
            table.collect_globals(&file.items);
        }
        table
    }

    /// Build a symbol table of global variables from a single file.
    pub fn from_file(file: &syn::File) -> Self {
        let mut table = Self::new();
        table.collect_globals(&file.items);
        table
    }

    fn collect_globals(&mut self, items: &[syn::Item]) {
        for item in items {
            match item {
                syn::Item::Const(i) => {
                    self.insert(i.ident.to_string(), (*i.ty).clone());
                }
                syn::Item::Static(i) => {
                    self.insert(i.ident.to_string(), (*i.ty).clone());
                }
                syn::Item::Mod(i) => {
                    if let Some((_, ref inner)) = i.content {
                        self.collect_globals(inner);
                    }
                }
                _ => {}
            }
        }
    }
}

// ── Rewrite function types ───────────────────────────────────────────

/// Signature for expression rewrite functions.
///
/// Receives the [`Rewriter`] (for [`Rewriter::add_dep`]), the current
/// [`SymbolTable`], and the expression to inspect.  Returns
/// `Some((replacement, depth))` if the rewrite applies, where `depth`
/// limits further rewriting within the replacement.  Returns `None` if
/// the rewrite does not apply.
pub type ExprRewrite = fn(&Rewriter, &SymbolTable, &syn::Expr) -> Option<(syn::Expr, Depth)>;

/// Signature for statement rewrite functions.
///
/// Same contract as [`ExprRewrite`] but for statements.
pub type StmtRewrite = fn(&Rewriter, &SymbolTable, &syn::Stmt) -> Option<(syn::Stmt, Depth)>;

#[derive(Debug, Default)]
struct UseItems(BTreeMap<(bool, Vec<String>), BTreeSet<String>>);

impl UseItems {
    fn add_use(&mut self, is_absolute: bool, path: Vec<String>, ident: &str) {
        self.0
            .entry((is_absolute, path))
            .or_default()
            .insert(ident.to_string());
    }

    fn into_items(self) -> Result<Vec<syn::Item>> {
        let mut items = Vec::with_capacity(self.0.len());
        for ((is_absolute, path), idents) in self.0 {
            if idents.is_empty() {
                continue;
            }

            let prefix = if is_absolute { "::" } else { "" };
            let path_str = path.join("::");
            let item_src = if idents.len() == 1 {
                let ident = idents.into_iter().next().expect("singleton import missing");
                format!("use {prefix}{path_str}::{ident};")
            } else {
                let leaves = idents.into_iter().collect::<Vec<_>>().join(", ");
                format!("use {prefix}{path_str}::{{{leaves}}};")
            };
            items.push(
                syn::parse_str(&item_src)
                    .with_context(|| format!("parsing generated use item `{item_src}`"))?,
            );
        }
        Ok(items)
    }
}

#[derive(Debug, Default)]
pub struct ItemStore {
    uses: UseItems,
}

impl ItemStore {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn add_use(&mut self, is_absolute: bool, path: Vec<String>, ident: &str) {
        self.uses.add_use(is_absolute, path, ident);
    }

    fn into_items(self) -> Result<Vec<syn::Item>> {
        self.uses.into_items()
    }
}

// ── Rewriter ─────────────────────────────────────────────────────────

/// The core rewriting engine.
///
/// # Usage
///
/// ```ignore
/// let mut rw = Rewriter::new();
/// rw.add_expr_rewrite(Rewriter::some_expr_rewrite);
/// rw.rewrite_crate(&mut files, Depth::Unlimited);
/// ```
///
/// Specific rewrites are methods on `Rewriter` and are registered via
/// [`add_expr_rewrite`](Rewriter::add_expr_rewrite) or
/// [`add_stmt_rewrite`](Rewriter::add_stmt_rewrite) using
/// `Rewriter::method_name` as the function pointer.
///
/// The symbol table is built and maintained automatically during
/// traversal: globals are collected in a pre-pass, then function
/// arguments and let-bindings are added as the visitor enters scopes.
pub struct Rewriter {
    deps: RefCell<BTreeSet<String>>,
    expr_rewrites: Vec<ExprRewrite>,
    stmt_rewrites: Vec<StmtRewrite>,
    item_stores: RefCell<HashMap<PathBuf, ItemStore>>,
    root_file: RefCell<Option<PathBuf>>,
    cur_file: RefCell<Option<PathBuf>>,
}

impl Default for Rewriter {
    fn default() -> Self {
        Self::new()
    }
}

impl Rewriter {
    /// Create a new rewriter with no registered rewrites.
    pub fn new() -> Self {
        Self {
            deps: RefCell::new(BTreeSet::new()),
            expr_rewrites: Vec::new(),
            stmt_rewrites: Vec::new(),
            item_stores: RefCell::new(HashMap::new()),
            root_file: RefCell::new(None),
            cur_file: RefCell::new(None),
        }
    }

    /// Register an expression rewrite.
    pub fn add_expr_rewrite(&mut self, rewrite: ExprRewrite) {
        self.expr_rewrites.push(rewrite);
    }

    /// Register a statement rewrite.
    pub fn add_stmt_rewrite(&mut self, rewrite: StmtRewrite) {
        self.stmt_rewrites.push(rewrite);
    }

    /// Record a crate dependency required by rewritten code.
    ///
    /// Rewrite functions call this when they emit code that depends on an
    /// external crate.  After rewriting, retrieve the full set with
    /// [`Rewriter::deps`].
    pub fn add_dep(&self, crate_name: impl Into<String>) {
        self.deps.borrow_mut().insert(crate_name.into());
    }

    /// Returns the set of crate dependencies accumulated during
    /// rewriting.
    pub fn deps(&self) -> BTreeSet<String> {
        self.deps.borrow().clone()
    }

    pub fn take_deps(&self) -> BTreeSet<String> {
        self.deps.replace(BTreeSet::new())
    }

    /// Apply all registered rewrites to every file.
    ///
    /// A symbol table of globals is collected from all files before
    /// rewriting begins.  Function arguments and let-bindings are added
    /// to the table as the visitor descends into each scope.
    pub fn rewrite_crate(&self, files: &mut [(PathBuf, syn::File)], depth: Depth) {
        let globals = SymbolTable::from_files(files);
        let root_path = files.first().map(|(path, _)| path.clone());
        *self.root_file.borrow_mut() = root_path;
        for (path, file) in files.iter_mut() {
            *self.cur_file.borrow_mut() = Some(path.clone());
            let mut visitor = RewriteVisitor {
                rewriter: self,
                depth,
                symbols: globals.clone(),
            };
            visitor.visit_file_mut(file);
            self.finalize_current_file_items(file)
                .expect("failed to insert generated items into rewritten file");
        }
        self.cur_file.borrow_mut().take();
        self.root_file.borrow_mut().take();
    }

    /// Apply all registered rewrites to a single [`syn::File`].
    ///
    /// Globals are collected from this file only.
    pub fn rewrite_file(&self, file: &mut syn::File, depth: Depth) {
        let globals = SymbolTable::from_file(file);
        self.root_file.borrow_mut().take();
        self.cur_file.borrow_mut().take();
        let mut visitor = RewriteVisitor {
            rewriter: self,
            depth,
            symbols: globals,
        };
        visitor.visit_file_mut(file);
        self.finalize_current_file_items(file)
            .expect("failed to insert generated items into rewritten file");
    }

    fn cur_file_path(&self) -> PathBuf {
        self.cur_file
            .borrow()
            .clone()
            .or_else(|| self.root_file.borrow().clone())
            .unwrap_or_default()
    }

    fn with_cur_file_item_store<F, T>(&self, f: F) -> T
    where
        F: FnOnce(&mut ItemStore) -> T,
    {
        let cur_file = self.cur_file_path();
        let mut item_stores = self.item_stores.borrow_mut();
        let item_store = item_stores.entry(cur_file).or_default();
        f(item_store)
    }

    fn finalize_current_file_items(&self, file: &mut syn::File) -> Result<()> {
        let cur_file = self.cur_file_path();
        let Some(item_store) = self.item_stores.borrow_mut().remove(&cur_file) else {
            return Ok(());
        };

        let mut generated_items = item_store.into_items()?;
        if generated_items.is_empty() {
            return Ok(());
        }

        generated_items.append(&mut file.items);
        file.items = generated_items;
        Ok(())
    }

    fn try_rewrite_expr(
        &self,
        symbols: &SymbolTable,
        expr: &syn::Expr,
    ) -> Option<(syn::Expr, Depth)> {
        self.expr_rewrites
            .iter()
            .find_map(|rw| rw(self, symbols, expr))
    }

    fn try_rewrite_stmt(
        &self,
        symbols: &SymbolTable,
        stmt: &syn::Stmt,
    ) -> Option<(syn::Stmt, Depth)> {
        self.stmt_rewrites
            .iter()
            .find_map(|rw| rw(self, symbols, stmt))
    }
}

// ── AST visitor ──────────────────────────────────────────────────────

struct RewriteVisitor<'a> {
    rewriter: &'a Rewriter,
    depth: Depth,
    symbols: SymbolTable,
}

impl RewriteVisitor<'_> {
    /// Add typed function/method arguments to the symbol table.
    fn add_fn_args(&mut self, sig: &syn::Signature) {
        for arg in &sig.inputs {
            if let Some((name, ty)) = binding_from_fn_arg(arg) {
                self.symbols.insert(name, ty);
            }
        }
    }

    /// Add bindings introduced by a local `let` statement.
    fn add_local_bindings(&mut self, local: &syn::Local) {
        collect_pat_bindings(&local.pat, &mut self.symbols);
    }
}

impl VisitMut for RewriteVisitor<'_> {
    fn visit_expr_mut(&mut self, expr: &mut syn::Expr) {
        // Try every registered expression rewrite on this node.
        if let Some((new_expr, inner_depth)) = self.rewriter.try_rewrite_expr(&self.symbols, expr) {
            *expr = new_expr;
            // Recurse into the replacement with the depth returned by the
            // rewrite, not the ambient depth.
            let mut inner = RewriteVisitor {
                rewriter: self.rewriter,
                depth: inner_depth,
                symbols: self.symbols.clone(),
            };
            visit_mut::visit_expr_mut(&mut inner, expr);
            return;
        }

        // No rewrite matched -- descend into children if depth allows.
        if let Some(deeper) = self.depth.descend() {
            let prev = std::mem::replace(&mut self.depth, deeper);
            visit_mut::visit_expr_mut(self, expr);
            self.depth = prev;
        }
    }

    fn visit_stmt_mut(&mut self, stmt: &mut syn::Stmt) {
        // Try every registered statement rewrite on this node.
        if let Some((new_stmt, inner_depth)) = self.rewriter.try_rewrite_stmt(&self.symbols, stmt) {
            *stmt = new_stmt;
            let mut inner = RewriteVisitor {
                rewriter: self.rewriter,
                depth: inner_depth,
                symbols: self.symbols.clone(),
            };
            visit_mut::visit_stmt_mut(&mut inner, stmt);
            return;
        }

        // No rewrite matched -- descend into children if depth allows.
        if let Some(deeper) = self.depth.descend() {
            let prev = std::mem::replace(&mut self.depth, deeper);
            visit_mut::visit_stmt_mut(self, stmt);
            self.depth = prev;
        }
    }

    /// Process statements in a block sequentially so that each
    /// let-binding becomes visible to subsequent statements.  The
    /// symbol table is restored when leaving the block.
    fn visit_block_mut(&mut self, block: &mut syn::Block) {
        let saved = self.symbols.clone();
        for stmt in &mut block.stmts {
            self.visit_stmt_mut(stmt);
            // After the statement has been (potentially) rewritten,
            // record any let-bindings it introduces.
            if let syn::Stmt::Local(ref local) = *stmt {
                self.add_local_bindings(local);
            }
        }
        self.symbols = saved;
    }

    /// Add function arguments to the symbol table before visiting the
    /// body.
    fn visit_item_fn_mut(&mut self, item: &mut syn::ItemFn) {
        let saved = self.symbols.clone();
        self.add_fn_args(&item.sig);
        visit_mut::visit_item_fn_mut(self, item);
        self.symbols = saved;
    }

    /// Add method arguments to the symbol table before visiting the
    /// body.
    fn visit_impl_item_fn_mut(&mut self, item: &mut syn::ImplItemFn) {
        let saved = self.symbols.clone();
        self.add_fn_args(&item.sig);
        visit_mut::visit_impl_item_fn_mut(self, item);
        self.symbols = saved;
    }

    /// Add trait method arguments to the symbol table before visiting
    /// the default body, if any.
    fn visit_trait_item_fn_mut(&mut self, item: &mut syn::TraitItemFn) {
        let saved = self.symbols.clone();
        self.add_fn_args(&item.sig);
        visit_mut::visit_trait_item_fn_mut(self, item);
        self.symbols = saved;
    }
}

// ── Binding extraction helpers ───────────────────────────────────────

/// Extract a (name, type) pair from a function argument.
fn binding_from_fn_arg(arg: &syn::FnArg) -> Option<(String, syn::Type)> {
    match arg {
        syn::FnArg::Typed(pt) => {
            if let syn::Pat::Ident(ref pi) = *pt.pat {
                Some((pi.ident.to_string(), (*pt.ty).clone()))
            } else {
                None
            }
        }
        syn::FnArg::Receiver(_) => None,
    }
}

/// Collect typed bindings from a pattern into the symbol table.
///
/// Handles the common `pat: Type` form (a [`syn::Pat::Type`] wrapping a
/// [`syn::Pat::Ident`]).
fn collect_pat_bindings(pat: &syn::Pat, table: &mut SymbolTable) {
    if let syn::Pat::Type(ref pt) = *pat {
        if let syn::Pat::Ident(ref pi) = *pt.pat {
            table.insert(pi.ident.to_string(), (*pt.ty).clone());
        }
    }
}

// ── File collection ──────────────────────────────────────────────────

/// Parse a crate starting from its root file (e.g. `src/lib.rs` or
/// `src/main.rs`) and collect every source file reachable through `mod`
/// declarations.
pub fn collect_crate_files(root: &Path) -> Result<Vec<(PathBuf, syn::File)>> {
    let root = root
        .canonicalize()
        .with_context(|| format!("canonicalising crate root: {}", root.display()))?;
    let mut files = Vec::new();
    let mut seen = HashSet::new();
    collect_file(&root, &mut files, &mut seen)?;
    Ok(files)
}

fn collect_file(
    path: &Path,
    files: &mut Vec<(PathBuf, syn::File)>,
    seen: &mut HashSet<PathBuf>,
) -> Result<()> {
    if !seen.insert(path.to_path_buf()) {
        return Ok(());
    }
    let src = fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
    let ast = syn::parse_file(&src).with_context(|| format!("parsing {}", path.display()))?;

    // Compute the base directory for resolving child `mod` declarations.
    // Directory-owning files (lib.rs, main.rs, mod.rs) resolve children
    // in their own directory; other files resolve in a sibling directory
    // named after themselves (e.g. foo.rs -> foo/).
    let mod_dir = mod_dir_for_file(path);

    // Resolve external `mod name;` declarations, including those nested
    // inside inline modules.
    resolve_mods_in_items(path, &mod_dir, &ast.items, files, seen)?;

    files.push((path.to_path_buf(), ast));
    Ok(())
}

/// Compute the directory in which a file's child `mod` declarations are
/// resolved.
fn mod_dir_for_file(path: &Path) -> PathBuf {
    let stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
    if matches!(stem, "lib" | "main" | "mod") {
        path.parent().unwrap_or(path).to_path_buf()
    } else {
        path.with_extension("")
    }
}

/// Recursively walk items looking for `mod` declarations.
///
/// *  `mod name;` (no body) — resolve to a file and [`collect_file`] it.
/// *  `mod name { ... }` (inline) — descend into its items, advancing
///    `mod_dir` by the inline module's name so that nested `mod child;`
///    declarations resolve in the correct subdirectory.
///
/// `file_path` is the on-disk file (used for `#[path]` resolution).
/// `mod_dir` is the directory in which standard child modules are looked
/// up; it shifts deeper with each inline module.
fn resolve_mods_in_items(
    file_path: &Path,
    mod_dir: &Path,
    items: &[syn::Item],
    files: &mut Vec<(PathBuf, syn::File)>,
    seen: &mut HashSet<PathBuf>,
) -> Result<()> {
    for item in items {
        if let syn::Item::Mod(m) = item {
            match m.content {
                None => {
                    // External file module.
                    if let Some(child) =
                        resolve_mod(file_path, mod_dir, &m.ident.to_string(), &m.attrs)
                    {
                        collect_file(&child, files, seen)?;
                    }
                }
                Some((_, ref inner_items)) => {
                    // Inline module — advance the module directory so
                    // that `mod bar;` inside `mod foo { }` resolves to
                    // `<mod_dir>/foo/bar.rs`.
                    let child_dir = mod_dir.join(m.ident.to_string());
                    resolve_mods_in_items(file_path, &child_dir, inner_items, files, seen)?;
                }
            }
        }
    }
    Ok(())
}

/// Resolve an external `mod name;` to a file path, respecting `#[path]`
/// attributes and the standard module layout.
///
/// `file_path` is the on-disk file that contains the `mod` declaration
/// (used for `#[path = "..."]` resolution).  `mod_dir` is the directory
/// to use for standard `name.rs` / `name/mod.rs` lookup and already
/// accounts for any enclosing inline modules.
fn resolve_mod(
    file_path: &Path,
    mod_dir: &Path,
    name: &str,
    attrs: &[syn::Attribute],
) -> Option<PathBuf> {
    // Check for an explicit #[path = "..."] attribute.
    // The path is relative to the directory of the *file* on disk.
    for attr in attrs {
        if attr.path().is_ident("path") {
            if let syn::Meta::NameValue(ref nv) = attr.meta {
                if let syn::Expr::Lit(syn::ExprLit {
                    lit: syn::Lit::Str(ref s),
                    ..
                }) = nv.value
                {
                    let p = file_path.parent()?.join(s.value());
                    if p.exists() {
                        return Some(p);
                    }
                }
            }
        }
    }

    // Standard resolution: look for `<mod_dir>/name.rs` or
    // `<mod_dir>/name/mod.rs`.
    let as_file = mod_dir.join(format!("{name}.rs"));
    if as_file.exists() {
        return Some(as_file);
    }

    let as_dir = mod_dir.join(name).join("mod.rs");
    if as_dir.exists() {
        return Some(as_dir);
    }

    None
}

// ── Cargo metadata ───────────────────────────────────────────────────

/// A single Cargo target (lib, bin, example, …) with its root source file.
#[derive(Debug, Clone)]
pub struct CrateRoot {
    /// Package name from `Cargo.toml`.
    pub package_name: String,
    /// Target name (often equal to the package name for `lib` targets).
    pub target_name: String,
    /// Target kinds, e.g. `["lib"]`, `["bin"]`, `["example"]`.
    pub kinds: Vec<String>,
    /// Absolute path to the root source file for this target.
    pub src_path: PathBuf,
}

// Subset of the `cargo metadata` JSON we care about.
#[derive(Deserialize)]
struct CargoMetadata {
    packages: Vec<MetadataPackage>,
}

#[derive(Deserialize)]
struct MetadataPackage {
    name: String,
    targets: Vec<MetadataTarget>,
}

#[derive(Deserialize)]
struct MetadataTarget {
    name: String,
    kind: Vec<String>,
    src_path: PathBuf,
}

/// Discover the root source files of every crate in a Cargo workspace by
/// running `cargo metadata --offline --no-deps --format-version 1`.
///
/// `manifest_dir` should be any directory inside the workspace (typically
/// the workspace root).  The function returns one [`CrateRoot`] per
/// target (lib, bin, example, …) listed in the metadata.
pub fn discover_crate_roots(manifest_dir: &Path) -> Result<Vec<CrateRoot>> {
    let output = Command::new("cargo")
        .args([
            "metadata",
            "--offline",
            "--no-deps",
            "--format-version",
            "1",
        ])
        .current_dir(manifest_dir)
        .output()
        .context("failed to run `cargo metadata`")?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        anyhow::bail!(
            "`cargo metadata` exited with {}: {}",
            output.status,
            stderr.trim()
        );
    }

    let meta: CargoMetadata =
        serde_json::from_slice(&output.stdout).context("parsing `cargo metadata` output")?;

    let mut roots = Vec::new();
    for pkg in meta.packages {
        for tgt in pkg.targets {
            roots.push(CrateRoot {
                package_name: pkg.name.clone(),
                target_name: tgt.name,
                kinds: tgt.kind,
                src_path: tgt.src_path,
            });
        }
    }
    Ok(roots)
}

type CrateFiles = Vec<(PathBuf, syn::File)>;

/// Convenience wrapper: discover every crate root in the workspace and
/// then [`collect_crate_files`] for each one, returning all files keyed
/// by [`CrateRoot`].
pub fn collect_workspace_files(manifest_dir: &Path) -> Result<Vec<(CrateRoot, CrateFiles)>> {
    let roots = discover_crate_roots(manifest_dir)?;
    let mut result = Vec::with_capacity(roots.len());
    for root in roots {
        let files = collect_crate_files(&root.src_path)?;
        result.push((root, files));
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rewrite_file_strips_redundant_outer_statement_parens() {
        let mut rw = Rewriter::new();
        rw.add_stmt_rewrite(Rewriter::rewrite_stmt_outer_parens);

        let mut file = syn::parse_file("fn demo() -> i32 { ((foo())); (((value))) }").unwrap();

        rw.rewrite_file(&mut file, Depth::Unlimited);

        let rewritten = prettyplease::unparse(&file);
        assert!(rewritten.contains("foo();"));
        assert!(rewritten.contains("value"));
        assert!(!rewritten.contains("((foo()));"));
        assert!(!rewritten.contains("(((value)))"));
    }

    #[test]
    fn rewrite_file_rewrites_trailing_string_nul_assignment() {
        let mut rw = Rewriter::new();
        rw.add_stmt_rewrite(Rewriter::rewrite_string_pop_trailing_nul);

        let mut file = syn::parse_file(
            "fn demo(s1: String) { *s1.offset(s1.len().wrapping_sub(1 as size_t) as isize) = '\\0' as ::core::ffi::c_char; }",
        )
        .unwrap();

        rw.rewrite_file(&mut file, Depth::Unlimited);

        let rewritten = prettyplease::unparse(&file);
        assert!(rewritten.contains("s1.pop();"));
        assert!(!rewritten.contains("offset"));
    }

    #[test]
    fn rewrite_file_keeps_trailing_nul_assignment_for_non_strings() {
        let mut rw = Rewriter::new();
        rw.add_stmt_rewrite(Rewriter::rewrite_string_pop_trailing_nul);

        let mut file = syn::parse_file(
            "fn demo(s1: &[u8]) { *s1.offset(s1.len().wrapping_sub(1 as size_t) as isize) = '\\0' as ::core::ffi::c_char; }",
        )
        .unwrap();

        rw.rewrite_file(&mut file, Depth::Unlimited);

        let rewritten = prettyplease::unparse(&file);
        assert!(rewritten.contains("offset"));
        assert!(!rewritten.contains("s1.pop();"));
    }

    #[test]
    fn rewrite_crate_inserts_use_only_in_current_file() {
        let mut rw = Rewriter::new();
        rw.add_expr_rewrite(Rewriter::rewrite_strlen_of_slice);

        let mut files = vec![
            (
                PathBuf::from("src/uses_trait.rs"),
                syn::parse_file(
                    "fn demo(buf: &[u16]) { let _ = strlen(buf.as_mut_ptr()); let _ = strlen(buf.as_mut_ptr()); }",
                )
                .unwrap(),
            ),
            (
                PathBuf::from("src/untouched.rs"),
                syn::parse_file("fn untouched(buf: &[u16]) { let _ = buf.len(); }").unwrap(),
            ),
        ];

        rw.rewrite_crate(&mut files, Depth::Unlimited);

        let rewritten = prettyplease::unparse(&files[0].1);
        assert_eq!(rewritten.matches("use ::xj_cstr::ByteSlice;").count(), 1);
        assert!(rewritten.contains(".as_u8_slice()"));

        let untouched = prettyplease::unparse(&files[1].1);
        assert!(!untouched.contains("ByteSlice"));
    }

    #[test]
    fn rewrite_file_inserts_use_without_explicit_file_path() {
        let mut rw = Rewriter::new();
        rw.add_expr_rewrite(Rewriter::rewrite_strlen_of_slice);

        let mut file =
            syn::parse_file("fn demo(buf: &[u16]) { let _ = strlen(buf.as_mut_ptr()); }").unwrap();

        rw.rewrite_file(&mut file, Depth::Unlimited);

        let rewritten = prettyplease::unparse(&file);
        assert!(rewritten.contains("use ::xj_cstr::ByteSlice;"));
        assert!(rewritten.contains(".as_u8_slice()"));
    }
}
