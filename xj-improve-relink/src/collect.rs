//! Workspace discovery and module-path-aware source collection.
//!
//! [`collect_workspace`] runs `cargo metadata` to enumerate every crate
//! target (lib, bin, …) in a workspace, then parses each target's source
//! tree, recording the **module path** of every file so that callers can
//! build fully-qualified item paths such as `crate::src::lib::foo`.

use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result, bail};
use serde::Deserialize;

/// A single Cargo target (lib, bin, …) together with the package that owns
/// it.
#[derive(Debug, Clone)]
pub struct CrateTarget {
    /// Package name from `Cargo.toml`.
    pub package_name: String,
    /// Directory containing the package's `Cargo.toml`.
    pub package_dir: PathBuf,
    /// Target name. For a `lib` target this (with `-` mapped to `_`) is the
    /// name other crates use to reference it.
    pub target_name: String,
    /// Target kinds, e.g. `["lib"]`, `["bin"]`.
    pub kinds: Vec<String>,
    /// Absolute path to the root source file for this target.
    pub src_path: PathBuf,
}

impl CrateTarget {
    /// `true` if this target is a Rust library other crates can link
    /// against (and therefore reference by name in a path).
    pub fn is_linkable_lib(&self) -> bool {
        self.kinds
            .iter()
            .any(|k| matches!(k.as_str(), "lib" | "rlib"))
    }

    /// The identifier other crates use to name this lib in a path
    /// (Cargo replaces `-` with `_`).
    pub fn extern_crate_name(&self) -> String {
        self.target_name.replace('-', "_")
    }
}

/// A single parsed source file together with its module path within the
/// crate (e.g. `["src", "lib"]` for `src/lib.rs` reached via
/// `mod src { mod lib; }`). The crate root file has an empty module path.
#[derive(Debug)]
pub struct FileUnit {
    pub path: PathBuf,
    pub modpath: Vec<String>,
    pub ast: syn::File,
}

/// All files belonging to one crate target.
#[derive(Debug)]
pub struct CrateUnit {
    pub target: CrateTarget,
    pub files: Vec<FileUnit>,
}

// ── cargo metadata ───────────────────────────────────────────────────

#[derive(Deserialize)]
struct CargoMetadata {
    packages: Vec<MetadataPackage>,
    workspace_members: Vec<String>,
}

#[derive(Deserialize)]
struct MetadataPackage {
    id: String,
    name: String,
    manifest_path: PathBuf,
    targets: Vec<MetadataTarget>,
}

#[derive(Deserialize)]
struct MetadataTarget {
    name: String,
    kind: Vec<String>,
    src_path: PathBuf,
}

/// Enumerate every crate target belonging to a workspace member by
/// invoking `cargo metadata`.
pub fn discover_targets(manifest_dir: &Path) -> Result<Vec<CrateTarget>> {
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
        bail!(
            "`cargo metadata` exited with {}: {}",
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }

    let meta: CargoMetadata =
        serde_json::from_slice(&output.stdout).context("parsing `cargo metadata` output")?;

    let members: HashSet<&str> = meta.workspace_members.iter().map(String::as_str).collect();

    let mut targets = Vec::new();
    for pkg in &meta.packages {
        if !members.contains(pkg.id.as_str()) {
            continue;
        }
        let package_dir = pkg
            .manifest_path
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_else(|| pkg.manifest_path.clone());
        for tgt in &pkg.targets {
            targets.push(CrateTarget {
                package_name: pkg.name.clone(),
                package_dir: package_dir.clone(),
                target_name: tgt.name.clone(),
                kinds: tgt.kind.clone(),
                src_path: tgt.src_path.clone(),
            });
        }
    }
    Ok(targets)
}

// ── source collection ────────────────────────────────────────────────

/// Discover every workspace target and parse its source tree.
pub fn collect_workspace(manifest_dir: &Path) -> Result<Vec<CrateUnit>> {
    let mut units = Vec::new();
    for target in discover_targets(manifest_dir)? {
        // Only Rust source we can parse and rewrite. Build scripts share
        // the package but are not interesting here.
        if target.kinds.iter().any(|k| k == "custom-build") {
            continue;
        }
        let files = collect_target_files(&target.src_path)?;
        units.push(CrateUnit { target, files });
    }
    Ok(units)
}

/// Parse a crate target starting from its root source file, following
/// `mod` declarations and recording each file's module path.
fn collect_target_files(root: &Path) -> Result<Vec<FileUnit>> {
    let root = root
        .canonicalize()
        .with_context(|| format!("canonicalising crate root: {}", root.display()))?;
    let mut files = Vec::new();
    let mut seen = HashSet::new();
    collect_file(&root, Vec::new(), &mut files, &mut seen)?;
    Ok(files)
}

fn collect_file(
    path: &Path,
    modpath: Vec<String>,
    files: &mut Vec<FileUnit>,
    seen: &mut HashSet<PathBuf>,
) -> Result<()> {
    if !seen.insert(path.to_path_buf()) {
        return Ok(());
    }
    let src = fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
    let ast = syn::parse_file(&src).with_context(|| format!("parsing {}", path.display()))?;

    let mod_dir = mod_dir_for_file(path);
    resolve_mods_in_items(path, &mod_dir, &modpath, &ast.items, files, seen)?;

    files.push(FileUnit {
        path: path.to_path_buf(),
        modpath,
        ast,
    });
    Ok(())
}

/// Directory in which a file's child `mod name;` declarations resolve.
/// `lib.rs`/`main.rs`/`mod.rs` own their directory; `foo.rs` owns `foo/`.
fn mod_dir_for_file(path: &Path) -> PathBuf {
    let stem = path.file_stem().and_then(|s| s.to_str()).unwrap_or("");
    if matches!(stem, "lib" | "main" | "mod") {
        path.parent().unwrap_or(path).to_path_buf()
    } else {
        path.with_extension("")
    }
}

/// Walk items for `mod` declarations, threading the current module path.
fn resolve_mods_in_items(
    file_path: &Path,
    mod_dir: &Path,
    modpath: &[String],
    items: &[syn::Item],
    files: &mut Vec<FileUnit>,
    seen: &mut HashSet<PathBuf>,
) -> Result<()> {
    for item in items {
        if let syn::Item::Mod(m) = item {
            let name = m.ident.to_string();
            let mut child_modpath = modpath.to_vec();
            child_modpath.push(name.clone());
            match m.content {
                None => {
                    if let Some(child) = resolve_mod(file_path, mod_dir, &name, &m.attrs) {
                        collect_file(&child, child_modpath, files, seen)?;
                    }
                }
                Some((_, ref inner_items)) => {
                    let child_dir = mod_dir.join(&name);
                    resolve_mods_in_items(
                        file_path,
                        &child_dir,
                        &child_modpath,
                        inner_items,
                        files,
                        seen,
                    )?;
                }
            }
        }
    }
    Ok(())
}

/// Resolve `mod name;` to a file, respecting `#[path = "..."]`.
fn resolve_mod(
    file_path: &Path,
    mod_dir: &Path,
    name: &str,
    attrs: &[syn::Attribute],
) -> Option<PathBuf> {
    for attr in attrs {
        if attr.path().is_ident("path")
            && let syn::Meta::NameValue(ref nv) = attr.meta
            && let syn::Expr::Lit(syn::ExprLit {
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
