//! Applying cross-package dependency edits to `Cargo.toml` manifests.

use std::fs;
use std::path::{Component, Path, PathBuf};

use anyhow::{Context, Result, bail};
use toml_edit::{DocumentMut, Item, Table, value};

use crate::relink::DepRequest;

/// Apply every dependency request, adding `path` dependencies to the
/// relevant manifests. Existing dependency keys are left untouched.
pub fn apply_dep_requests(reqs: &[DepRequest], dry_run: bool) -> Result<()> {
    for req in reqs {
        let manifest_path = req.importer_pkg_dir.join("Cargo.toml");
        let rel = relative_path(&req.importer_pkg_dir, &req.dep_pkg_dir);
        let rel_str = rel.to_string_lossy().replace('\\', "/");
        add_path_dependency(&manifest_path, &req.dep_package, &rel_str, dry_run)?;
    }
    Ok(())
}

fn add_path_dependency(
    manifest_path: &Path,
    dep_name: &str,
    rel_path: &str,
    dry_run: bool,
) -> Result<()> {
    let mut manifest: DocumentMut = fs::read_to_string(manifest_path)
        .with_context(|| format!("reading {}", manifest_path.display()))?
        .parse()
        .with_context(|| format!("parsing {}", manifest_path.display()))?;

    let deps_item = manifest
        .entry("dependencies")
        .or_insert(Item::Table(Table::new()));
    let Some(deps_table) = deps_item.as_table_like_mut() else {
        bail!(
            "[dependencies] in {} is not a table",
            manifest_path.display()
        );
    };

    if deps_table.contains_key(dep_name) {
        return Ok(());
    }

    let mut dep = toml_edit::InlineTable::new();
    dep.insert("path", rel_path.into());
    deps_table.insert(dep_name, value(dep));

    if dry_run {
        eprintln!(
            "relink: would add dependency `{dep_name} = {{ path = \"{rel_path}\" }}` to {}",
            manifest_path.display()
        );
        return Ok(());
    }

    fs::write(manifest_path, manifest.to_string())
        .with_context(|| format!("writing {}", manifest_path.display()))?;
    eprintln!(
        "relink: added dependency `{dep_name} = {{ path = \"{rel_path}\" }}` to {}",
        manifest_path.display()
    );
    Ok(())
}

/// Compute a relative path from `from` (a directory) to `to`.
pub fn relative_path(from: &Path, to: &Path) -> PathBuf {
    let from: Vec<Component> = from.components().collect();
    let to: Vec<Component> = to.components().collect();

    let common = from.iter().zip(&to).take_while(|(a, b)| a == b).count();

    let mut rel = PathBuf::new();
    for _ in common..from.len() {
        rel.push("..");
    }
    for comp in &to[common..] {
        rel.push(comp.as_os_str());
    }
    if rel.as_os_str().is_empty() {
        rel.push(".");
    }
    rel
}
