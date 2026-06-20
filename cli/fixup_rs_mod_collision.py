import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import tree_sitter_rust as tsrust  # type: ignore[import-untyped]
from tree_sitter import Language, Node, Parser


LOG = logging.getLogger(__name__)

RUST_LANGUAGE = Language(tsrust.language())
RUST_PARSER = Parser(RUST_LANGUAGE)


class CrateRootNotFound(Exception):
    pass


@dataclass
class Injection:
    module_path: str
    include_path: str
    byte_offset: int
    applied: bool


@dataclass(frozen=True)
class ModItem:
    name: str
    stack: tuple[str, ...]
    body: Node
    open_brace_byte: int


def inject_tu_includes(
    build_dir: str | Path,
    *,
    crate_root: str | Path | None = None,
    dry_run: bool = False,
) -> list[Injection]:
    build_dir = Path(build_dir)
    roots = (
        [(Path(crate_root), build_dir)]
        if crate_root is not None
        else list(_find_crate_roots(build_dir))
    )

    all_planned: list[Injection] = []
    for root, module_dir in roots:
        all_planned.extend(_inject_tu_includes_for_crate(root, module_dir, dry_run=dry_run))
    return all_planned


def _inject_tu_includes_for_crate(
    root: Path,
    module_dir: Path,
    *,
    dry_run: bool,
) -> list[Injection]:
    data = root.read_bytes()
    tree = RUST_PARSER.parse(data)

    planned: list[Injection] = []
    for item in _iter_mod_items(tree.root_node, data, (), False):
        candidate = module_dir.joinpath(*item.stack, f"{item.name}.rs")
        if not candidate.is_file():
            continue
        if _body_first_item_is_include(item.body, data):
            continue

        include_path = "/".join([*item.stack, f"{item.name}.rs"])
        planned.append(
            Injection(
                module_path="::".join([*item.stack, item.name]),
                include_path=include_path,
                byte_offset=item.open_brace_byte + 1,
                applied=False,
            )
        )

    if dry_run:
        return planned

    for injection in sorted(planned, key=lambda i: i.byte_offset, reverse=True):
        include_text = f'\n    include!("{injection.include_path}");'.encode()
        data = data[: injection.byte_offset] + include_text + data[injection.byte_offset :]
        injection.applied = True

    if planned:
        root.write_bytes(data)

    return planned


def _find_crate_roots(build_dir: Path) -> Iterator[tuple[Path, Path]]:
    try:
        root = _find_crate_root(build_dir)
    except CrateRootNotFound:
        pass
    else:
        yield root, build_dir
        return

    child_roots: list[tuple[Path, Path]] = []
    for child in sorted(build_dir.iterdir()):
        if not child.is_dir():
            continue
        try:
            child_roots.append((_find_crate_root(child), child))
        except CrateRootNotFound:
            continue

    if not child_roots:
        raise CrateRootNotFound(f"Could not find lib.rs or c2rust-lib.rs in {build_dir}")

    yield from child_roots


def _find_crate_root(build_dir: Path) -> Path:
    for name in ("lib.rs", "c2rust-lib.rs"):
        candidate = build_dir / name
        if candidate.is_file():
            return candidate
    raise CrateRootNotFound(f"Could not find lib.rs or c2rust-lib.rs in {build_dir}")


def _iter_mod_items(
    parent: Node,
    source: bytes,
    stack: tuple[str, ...],
    inherited_path_attr: bool,
) -> Iterator[ModItem]:
    for child in parent.children:
        if child.type != "mod_item":
            continue

        name_node = child.child_by_field_name("name")
        if name_node is None:
            continue
        name = source[name_node.start_byte : name_node.end_byte].decode("utf-8")

        has_path_attr = inherited_path_attr or _has_path_attribute(child, source)
        module_path = "::".join([*stack, name])
        body = child.child_by_field_name("body")

        if has_path_attr:
            LOG.warning("skipping #[path] module subtree during include injection: %s", module_path)
            continue

        if body is None:
            continue

        yield ModItem(
            name=name,
            stack=stack,
            body=body,
            open_brace_byte=body.start_byte,
        )
        yield from _iter_mod_items(body, source, (*stack, name), has_path_attr)


def _has_path_attribute(node: Node, source: bytes) -> bool:
    sibling = node.prev_named_sibling
    while sibling is not None and sibling.type in {
        "attribute_item",
        "block_comment",
        "line_comment",
    }:
        if sibling.type == "attribute_item" and _attribute_is_path(sibling, source):
            return True
        sibling = sibling.prev_named_sibling
    return False


def _attribute_is_path(attribute: Node, source: bytes) -> bool:
    for child in attribute.children:
        if child.type != "attribute":
            continue
        identifier = child.child(0)
        if identifier is not None and identifier.type == "identifier":
            return source[identifier.start_byte : identifier.end_byte] == b"path"
    return False


def _body_first_item_is_include(body: Node, source: bytes) -> bool:
    for child in body.children:
        if not child.is_named or child.type in {
            "block_comment",
            "line_comment",
            "empty_statement",
        }:
            continue
        return child.type == "macro_invocation" and _macro_name(child, source) == "include"
    return False


def _macro_name(node: Node, source: bytes) -> str | None:
    name_node = node.child_by_field_name("macro")
    if name_node is None:
        for child in node.children:
            if child.type == "identifier":
                name_node = child
                break
    if name_node is None:
        return None
    return source[name_node.start_byte : name_node.end_byte].decode("utf-8")
