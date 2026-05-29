import copy
import glob
import json
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
from itertools import product
from pathlib import Path

import click

import toml

import translation
from tenj_types import ResolvedPath, UserFacingError


# ---------------------------------------------------------------------------
# CMakePresets helpers
# ---------------------------------------------------------------------------


def _cmake_cache_var_str(raw_val: object) -> str:
    """Extract the string value from a cacheVariable entry.

    Handles both the simple string form ("fast") and the object form
    ({"type": "STRING", "value": "fast"}).
    """
    if isinstance(raw_val, dict):
        return str(raw_val.get("value", ""))
    return str(raw_val)


def _is_boolean_var(values: list) -> bool:
    return bool(values) and all(isinstance(v, bool) for v in values)


def load_cmake_presets(presets_path: Path) -> list[dict]:
    """Parse CMakePresets.json.

    Returns a list of non-hidden configurePresets, each as
    {"name": str, "cacheVariables": {param: str_value, ...}} with
    inheritance fully resolved.
    """
    with open(presets_path) as f:
        data = json.load(f)

    configure_presets: list[dict] = data.get("configurePresets", [])
    preset_by_name: dict[str, dict] = {p["name"]: p for p in configure_presets}

    def resolve_vars(preset: dict, seen: frozenset[str]) -> dict[str, str]:
        name: str = preset["name"]
        if name in seen:
            raise UserFacingError(
                f"Cyclic inheritance in CMakePresets.json at preset '{name}'"
            )
        seen = seen | {name}

        merged: dict[str, str] = {}
        inherits = preset.get("inherits", [])
        if isinstance(inherits, str):
            inherits = [inherits]
        for parent_name in inherits:
            if parent_name not in preset_by_name:
                raise UserFacingError(
                    f"CMakePresets.json preset '{name}' inherits unknown preset '{parent_name}'"
                )
            merged.update(resolve_vars(preset_by_name[parent_name], seen))

        for k, v in preset.get("cacheVariables", {}).items():
            merged[k] = _cmake_cache_var_str(v)

        return merged

    result = []
    for preset in configure_presets:
        if preset.get("hidden", False):
            continue
        result.append({"name": preset["name"], "cacheVariables": resolve_vars(preset, frozenset())})
    return result


def preset_to_features(preset_vars: dict[str, str], variables: dict) -> list[str]:
    """Map a preset's resolved cacheVariables to the Rust feature names it enables.

    Only variables present in *variables* (the configurable_variables from the
    config file) are considered.  Boolean variables (all config values are Python
    bools) map to either the bare param name (when the preset says ON/true) or
    nothing (OFF/false).  Non-boolean variables map to ``{param}_{value}``.
    """
    features: list[str] = []
    for param, values in variables.items():
        if param not in preset_vars:
            continue
        cmake_val = preset_vars[param]
        if _is_boolean_var(values):
            if cmake_val.upper() in ("ON", "TRUE"):
                features.append(param)
        else:
            features.append(f"{param}_{cmake_val}")
    return features


def emit_preset_features(merged_dir: Path, variables: dict, presets: list[dict]) -> None:
    """Add one Cargo feature per preset to every member crate in the merged workspace.

    Each preset feature lists only the feature deps that the member crate
    actually defines, so members that don't use a particular configurable
    variable won't reference a non-existent feature.
    """
    # Full candidate feature list per preset (across all variables)
    preset_candidates: dict[str, list[str]] = {
        p["name"]: preset_to_features(p["cacheVariables"], variables) for p in presets
    }

    click.echo(f"\nEmitting {len(preset_candidates)} preset feature(s) into merged Cargo.tomls:")
    for name, feats in preset_candidates.items():
        click.echo(f"  {name} = {feats}")

    workspace_cargo = _load_cargo_toml(merged_dir / "Cargo.toml")
    members: list[str] = workspace_cargo.get("workspace", {}).get("members", [])

    for member in members:
        member_cargo_path = merged_dir / member / "Cargo.toml"
        if not member_cargo_path.exists():
            continue
        member_cargo = _load_cargo_toml(member_cargo_path)
        existing_features: set[str] = set(member_cargo.get("features", {}).keys())
        features: dict = member_cargo.setdefault("features", {})
        for preset_name, all_deps in preset_candidates.items():
            features[preset_name] = [d for d in all_deps if d in existing_features]
        write_toml(member_cargo_path, member_cargo)

    click.echo(f"Updated {len(members)} member Cargo.toml(s) with preset features.")


# ---------------------------------------------------------------------------
# Combo helpers
# ---------------------------------------------------------------------------


def load_combinations(config_path: Path) -> tuple[dict, list[dict]]:
    with open(config_path) as f:
        data = json.load(f)
    variables = data["configurable_variables"]
    names = list(variables.keys())
    value_lists = [variables[n] for n in names]
    combos = [dict(zip(names, values)) for values in product(*value_lists)]
    return variables, combos


def cmake_value(v) -> str:
    if isinstance(v, bool):
        return "ON" if v else "OFF"
    return str(v)


def combo_dirname(combo: dict) -> str:
    parts = [f"{k}_{cmake_value(v)}" for k, v in combo.items()]
    return "__".join(parts)


# ---------------------------------------------------------------------------
# TOML writer
# ---------------------------------------------------------------------------


def _toml_leaf(val) -> str:
    if isinstance(val, bool):
        return "true" if val else "false"
    if isinstance(val, int):
        return str(val)
    if isinstance(val, float):
        return str(val)
    if isinstance(val, str):
        return f'"{val}"'
    if isinstance(val, list):
        return "[" + ", ".join(_toml_leaf(v) for v in val) + "]"
    raise ValueError(f"Cannot write as TOML inline value: {type(val)!r}")


def _emit_table(lines: list[str], data: dict, prefix: str | None):
    regular: dict = {}
    subtables: dict = {}
    aot: dict = {}
    for key, val in data.items():
        if isinstance(val, dict):
            subtables[key] = val
        elif isinstance(val, list) and val and isinstance(val[0], dict):
            aot[key] = val
        else:
            regular[key] = val

    if prefix is not None and regular:
        lines.append(f"\n[{prefix}]")
    for key, val in regular.items():
        lines.append(f"{key} = {_toml_leaf(val)}")

    for key, val in subtables.items():
        full = f"{prefix}.{key}" if prefix else key
        _emit_table(lines, val, full)

    for key, val in aot.items():
        full = f"{prefix}.{key}" if prefix else key
        for item in val:
            lines.append(f"\n[[{full}]]")
            _emit_table(lines, item, None)


def write_toml(path: Path, data: dict):
    lines: list[str] = []
    _emit_table(lines, data, prefix=None)
    path.write_text("\n".join(lines).strip() + "\n")


# ---------------------------------------------------------------------------
# Cargo.toml normalization helpers
# ---------------------------------------------------------------------------

DEP_SECTIONS = ("dependencies", "dev-dependencies", "build-dependencies")


def _load_cargo_toml(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return toml.load(f)


def resolve_members(workspace_root: Path, patterns: list[str]) -> dict[str, Path]:
    resolved: dict[str, Path] = {}
    for pattern in patterns:
        if any(c in pattern for c in "*?["):
            for match in glob.glob(str(workspace_root / pattern)):
                p = Path(match)
                if (p / "Cargo.toml").exists():
                    rel = str(p.relative_to(workspace_root))
                    resolved[rel] = p
        else:
            p = (workspace_root / pattern).resolve()
            if (p / "Cargo.toml").exists():
                resolved[pattern] = p
    return resolved


def workspace_without_members(cargo: dict) -> dict:
    d = copy.deepcopy(cargo)
    d.get("workspace", {}).pop("members", None)
    return d


def intra_ws_dep_keys(cargo: dict, member_abs: Path, ws_abs_set: set[Path]) -> set[tuple[str, str]]:
    result: set[tuple[str, str]] = set()
    for section in DEP_SECTIONS:
        for dep_name, spec in cargo.get(section, {}).items():
            if isinstance(spec, dict) and "path" in spec:
                resolved = (member_abs / spec["path"]).resolve()
                if resolved in ws_abs_set:
                    result.add((section, dep_name))
    return result


def cargo_without_intra_ws(cargo: dict, intra_ws: set[tuple[str, str]]) -> dict:
    d = copy.deepcopy(cargo)
    for section, dep_name in intra_ws:
        d.get(section, {}).pop(dep_name, None)
    d.pop("features", None)
    return d


def union_dep_spec(specs: list) -> dict:
    result = dict(specs[0])
    all_features: set[str] = set(result.get("features", []))
    for spec in specs[1:]:
        if isinstance(spec, dict):
            all_features.update(spec.get("features", []))
    if all_features:
        result["features"] = sorted(all_features)
    elif "features" in result and not result["features"]:
        del result["features"]
    return result


def create_stub_crate(path: Path, reference_cargo: dict | None):
    path.mkdir(parents=True, exist_ok=True)
    pkg = (reference_cargo or {}).get("package", {})
    stub = {
        "package": {
            "name": pkg.get("name", path.name),
            "version": pkg.get("version", "0.0.0"),
            "edition": pkg.get("edition", "2021"),
        }
    }
    write_toml(path / "Cargo.toml", stub)
    (path / "src").mkdir(exist_ok=True)
    (path / "src" / "lib.rs").touch()


def normalize_member_cargo_tomls(
    member: str,
    inputs: list[tuple[dict, Path]],
    per_entry_ws_abs: list[set[Path]],
):
    cargo_tomls = [_load_cargo_toml(p / "Cargo.toml") for _, p in inputs]

    all_intra_ws: set[tuple[str, str]] = set()
    for (_, member_abs), cargo, ws_abs in zip(inputs, cargo_tomls, per_entry_ws_abs):
        all_intra_ws |= intra_ws_dep_keys(cargo, member_abs, ws_abs)

    if not all_intra_ws:
        return

    stripped = [cargo_without_intra_ws(c, all_intra_ws) for c in cargo_tomls]
    canonical = json.dumps(stripped[0], sort_keys=True)
    for i, s in enumerate(stripped[1:], 1):
        if json.dumps(s, sort_keys=True) != canonical:
            _, p0 = inputs[0]
            _, pi = inputs[i]
            click.echo(
                f"Warning: '{member}' Cargo.toml in {pi} differs from {p0} "
                f"outside of intra-workspace deps",
                err=True,
            )

    union_cargo = copy.deepcopy(stripped[0])

    for section, dep_name in all_intra_ws:
        specs = [
            c.get(section, {}).get(dep_name)
            for c in cargo_tomls
            if c.get(section, {}).get(dep_name) is not None
        ]
        if specs:
            union_cargo.setdefault(section, {})[dep_name] = union_dep_spec(specs)

    all_features: dict[str, list] = {}
    for cargo in cargo_tomls:
        for fname, fdeps in cargo.get("features", {}).items():
            merged = set(all_features.get(fname, []))
            merged.update(fdeps)
            all_features[fname] = sorted(merged)
    if all_features:
        union_cargo["features"] = all_features

    dep_info: dict[tuple[str, str], tuple[str, dict | None]] = {}
    for section, dep_name in all_intra_ws:
        for (_, member_abs), cargo in zip(inputs, cargo_tomls):
            spec = cargo.get(section, {}).get(dep_name)
            if isinstance(spec, dict) and "path" in spec:
                candidate = (member_abs / spec["path"]).resolve()
                ref = (
                    _load_cargo_toml(candidate / "Cargo.toml")
                    if (candidate / "Cargo.toml").exists()
                    else None
                )
                dep_info[(section, dep_name)] = (spec["path"], ref)
                break

    for _, member_abs in inputs:
        write_toml(member_abs / "Cargo.toml", union_cargo)
        for (section, dep_name), (rel_path, ref_cargo) in dep_info.items():
            target = (member_abs / rel_path).resolve()
            if not (target / "Cargo.toml").exists():
                click.echo(f"  Creating stub crate at {target}")
                create_stub_crate(target, ref_cargo)


# ---------------------------------------------------------------------------
# Per-combo translation
# ---------------------------------------------------------------------------


def translate_one_combo(
    combo: dict,
    root: Path,
    codebase: Path,
    base_resultsdir: Path,
    cratename: str,
    guidance_str: str,
    do_not_refactor: list[ResolvedPath],
    buildcmd: str | None,
    base_cmake_defines: list[str],
) -> tuple[str, bool, str]:
    name = combo_dirname(combo)
    combo_resultsdir = base_resultsdir / name
    combo_defines = [f"{k}={cmake_value(v)}" for k, v in combo.items()]
    all_defines = list(base_cmake_defines) + combo_defines
    try:
        translation.do_translate(
            root,
            codebase,
            combo_resultsdir,
            cratename,
            guidance_str,
            do_not_refactor,
            buildcmd,
            all_defines,
        )
        return name, True, ""
    except Exception as e:
        return name, False, str(e)


def run_all_combos(
    combos: list[dict],
    root: Path,
    codebase: Path,
    base_resultsdir: Path,
    cratename: str,
    guidance_str: str,
    do_not_refactor: list[ResolvedPath],
    buildcmd: str | None,
    base_cmake_defines: list[str],
    jobs: int,
) -> list[tuple[str, bool, str]]:
    results: list[tuple[str, bool, str]] = []
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(
                translate_one_combo,
                combo,
                root,
                codebase,
                base_resultsdir,
                cratename,
                guidance_str,
                do_not_refactor,
                buildcmd,
                base_cmake_defines,
            ): combo
            for combo in combos
        }
        for future in as_completed(futures):
            name, ok, err = future.result()
            results.append((name, ok, err))
            status = "OK" if ok else "FAILED"
            click.echo(f"  [{status}] {name}")
            if not ok and err:
                for line in err.splitlines()[:10]:
                    click.echo(f"    {line}", err=True)
    return results


# ---------------------------------------------------------------------------
# Staging and merge
# ---------------------------------------------------------------------------


def stage_finals(
    successful_combos: list[tuple[str, dict]],
    base_resultsdir: Path,
) -> tuple[Path, list[dict]]:
    staging_dir = base_resultsdir / "_staged_finals"
    staging_dir.mkdir(parents=True, exist_ok=True)

    inputs_entries = []
    for name, combo in successful_combos:
        src = base_resultsdir / name / "final"
        dst = staging_dir / name
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(src, dst)
        entry: dict = {"dir": str(dst)}
        for var, val in combo.items():
            entry[var] = val
        inputs_entries.append(entry)

    return staging_dir, inputs_entries


def run_merge(
    inputs_entries: list[dict],
    merged_dir: Path,
    crat_merge_bin: Path,
):
    abs_dirs = [Path(e["dir"]).resolve() for e in inputs_entries]

    cargo_datas: list[dict] = []
    for abs_dir in abs_dirs:
        cargo_path = abs_dir / "Cargo.toml"
        if not cargo_path.exists():
            raise UserFacingError(f"No Cargo.toml found in staged final: {cargo_path}")
        data = _load_cargo_toml(cargo_path)
        if "workspace" not in data or "members" not in data["workspace"]:
            raise UserFacingError(f"{cargo_path} has no [workspace] members")
        cargo_datas.append(data)

    stripped_ws = [workspace_without_members(d) for d in cargo_datas]
    canonical_ws = json.dumps(stripped_ws[0], sort_keys=True)
    for i, s in enumerate(stripped_ws[1:], 1):
        if json.dumps(s, sort_keys=True) != canonical_ws:
            raise UserFacingError(
                f"Cargo.toml in '{inputs_entries[i]['dir']}' differs from "
                f"'{inputs_entries[0]['dir']}' outside of [workspace].members"
            )

    member_map: dict[str, list[tuple[dict, Path]]] = {}
    per_entry_ws_abs: list[set[Path]] = []
    for entry, abs_dir, cargo in zip(inputs_entries, abs_dirs, cargo_datas):
        resolved = resolve_members(abs_dir, cargo["workspace"]["members"])
        per_entry_ws_abs.append(set(resolved.values()))
        for member_key, member_abs in resolved.items():
            member_map.setdefault(member_key, []).append((entry, member_abs))

    entry_to_ws_abs: dict[int, set[Path]] = {
        id(entry): ws_abs for entry, ws_abs in zip(inputs_entries, per_entry_ws_abs)
    }

    all_members = sorted(member_map)
    click.echo(f"Workspace members ({len(all_members)}): {', '.join(all_members)}")

    merged_dir.mkdir(parents=True, exist_ok=True)

    for member in all_members:
        member_inputs = member_map[member]
        member_ws_abs = [entry_to_ws_abs[id(entry)] for entry, _ in member_inputs]
        normalize_member_cargo_tomls(member, member_inputs, member_ws_abs)

    for member in all_members:
        member_input = []
        for entry, member_abs in member_map[member]:
            sub = {"dir": str(member_abs)}
            for k, v in entry.items():
                if k != "dir":
                    sub[k] = v
            member_input.append(sub)

        inputs_path = merged_dir / f"inputs_{member.replace('/', '__')}.json"
        with open(inputs_path, "w") as f:
            json.dump(member_input, f, indent=2)

        cmd = [str(crat_merge_bin), str(inputs_path), str(merged_dir)]
        click.echo(f"Merging member '{member}' ({len(member_input)} configs)...")
        result = subprocess.run(cmd)
        if result.returncode != 0:
            raise UserFacingError(f"crat-merge failed for member '{member}'")

    toolchain_contents: dict[str, str] = {}
    for member in all_members:
        p = merged_dir / member / "rust-toolchain.toml"
        if p.exists():
            toolchain_contents[member] = p.read_text()

    if toolchain_contents:
        unique = set(toolchain_contents.values())
        if len(unique) > 1:
            raise UserFacingError("rust-toolchain.toml files differ across workspace members")
        canonical_toolchain = next(iter(unique))
        (merged_dir / "rust-toolchain.toml").write_text(canonical_toolchain)
        for member in toolchain_contents:
            (merged_dir / member / "rust-toolchain.toml").unlink()
        click.echo(f"Wrote {merged_dir / 'rust-toolchain.toml'}")

    final_data = copy.deepcopy(stripped_ws[0])
    final_data.setdefault("workspace", {})["members"] = all_members
    write_toml(merged_dir / "Cargo.toml", final_data)
    click.echo(f"Wrote {merged_dir / 'Cargo.toml'}")


# ---------------------------------------------------------------------------
# Top-level orchestrator
# ---------------------------------------------------------------------------


def do_translate_multi_config(
    root: Path,
    codebase: Path,
    resultsdir: Path,
    config_path: Path,
    cratename: str,
    guidance_str: str,
    do_not_refactor: list[ResolvedPath],
    buildcmd: str | None,
    cmake_defines: list[str],
    jobs: int,
    crat_merge_bin: Path,
    cmake_presets_path: Path | None = None,
):
    variables, combos = load_combinations(config_path)

    presets: list[dict] = []
    if cmake_presets_path is not None:
        click.echo(f"Loading CMakePresets from {cmake_presets_path}")
        presets = load_cmake_presets(cmake_presets_path)
    total = len(combos)
    click.echo(f"Found {total} configuration combinations from {config_path}")

    results = run_all_combos(
        combos,
        root,
        codebase,
        resultsdir,
        cratename,
        guidance_str,
        do_not_refactor,
        buildcmd,
        cmake_defines,
        jobs,
    )

    combo_by_name = {combo_dirname(combo): combo for combo in combos}
    failed = [(name, err) for name, ok, err in results if not ok]
    succeeded = [(name, combo_by_name[name]) for name, ok, _ in results if ok]

    click.echo(f"\n{len(succeeded)}/{total} configurations translated successfully.")
    if failed:
        click.echo("Failed configurations:", err=True)
        for name, err in failed:
            click.echo(f"  {name}: {err}", err=True)
        raise UserFacingError(f"{len(failed)} configuration(s) failed to translate")

    click.echo("\nStaging final directories for merge...")
    _staging_dir, inputs_entries = stage_finals(succeeded, resultsdir)

    inputs_json_path = resultsdir / "inputs.json"
    with open(inputs_json_path, "w") as f:
        json.dump(inputs_entries, f, indent=2)
    click.echo(f"Wrote {inputs_json_path}")

    merged_dir = resultsdir / "merged"
    click.echo(f"\nMerging into {merged_dir}...")
    run_merge(inputs_entries, merged_dir, crat_merge_bin)
    click.echo(f"\nMerge complete: {merged_dir}")

    if presets:
        emit_preset_features(merged_dir, variables, presets)
