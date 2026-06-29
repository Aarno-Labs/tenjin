import os
import enum
import json
import re

import time
import shutil
import tempfile
import graphlib
from pathlib import Path
from typing import Callable, Sequence
from subprocess import CompletedProcess, CalledProcessError

import repo_root
from tenj_types import ResolvedPath

import click

import ingest_tracking
import hermetic
from speculative_rewriters import (
    CondensedSpanGraph,
    ExplicitSpan,
    SpeculativeFileRewriter,
    SpeculativeSpansEraser,
)
import cargo_workspace_helpers
import static_measurements_rust


def run_built_workspace_binary(
    root: Path, workspace_dir: str, binary_name: str, args: list[str], dir: Path
) -> CompletedProcess:
    default_profile = "debug" if binary_name != "xj-improve-lift-call-args" else "release"
    target_subdir = os.environ.get("XJ_BUILD_RS_PROFILE", default_profile)
    return hermetic.run(
        [binary_name, *args, dir],
        cwd=dir,
        env_ext={
            "PATH": os.pathsep.join([
                str(root / workspace_dir / "target" / target_subdir),
                os.environ["PATH"],
            ]),
        },
    )


def run_ast_grep_rewrite(dir: Path, pattern: str, rewrite: str) -> CompletedProcess:
    cp = hermetic.run(
        [
            hermetic.xj_ast_grep_exe(repo_root.localdir()),
            "-p",
            pattern,
            "--rewrite",
            rewrite,
            "--update-all",
            "-l",
            "rs",
            dir,
        ],
        cwd=dir,
        capture_output=True,
        check=False,
    )
    if cp.returncode != 0:
        stderr = cp.stderr.decode("utf-8")
        if not stderr:
            # Treat pattern-not-found as success, not failure.
            cp.returncode = 0
        else:
            # If the pattern is not found, ast-grep will return 1
            # without printing anything to stderr. So if we do see
            # output, something is amiss; print it and fail loudly.
            click.echo(stderr, err=True)
            click.echo("TENJIN WARNING: ast-grep rewrite failed for pattern: " + pattern, err=True)
            click.echo(cp.stdout.decode("utf-8"), err=True)
            cp.check_returncode()
    return cp


def run_improve_synsub(root: Path, args: list[str], dir: Path) -> CompletedProcess:
    # Macro arguments are in general token trees, not expressions,
    # and ast-grep rightly treats them as such. But there are important
    # special cases, so we temporarily rewrite invocation sites to look like
    # function calls with expression arguments.
    #
    # This produces, as an ephemeral intermediate state, code that does not
    # type check, but ast-grep can still match on it.
    run_ast_grep_rewrite(dir, "println!($$$ARGS)", "xj_astgrep_println($$$ARGS)")
    run_ast_grep_rewrite(dir, "print!($$$ARGS)", "xj_astgrep_print($$$ARGS)")
    run_ast_grep_rewrite(dir, "eprintln!($$$ARGS)", "xj_astgrep_eprintln($$$ARGS)")
    run_ast_grep_rewrite(dir, "eprint!($$$ARGS)", "xj_astgrep_eprint($$$ARGS)")

    # Direct indexing of arrays takes an unnecessary detour through unsafe pointers
    run_ast_grep_rewrite(
        dir,
        "(*$BASE.as_mut_ptr().offset($IDX as isize))",
        "($BASE[$IDX as usize])",
    )

    synsub_cp = run_built_workspace_binary(
        root,
        "xj-improve-synsub",
        "xj-improve-synsub",
        args,
        dir,
    )

    return synsub_cp


def run_improve_lift_call_args(root: Path, args: list[str], dir: Path) -> CompletedProcess:
    # rust-analyzer's surface syntax treats macro args as opaque token
    # trees, so we wait until after this pass to restore the original println! syntax.

    lift_cp = run_built_workspace_binary(
        root,
        "xj-improve-lift-call-args",
        "xj-improve-lift-call-args",
        args,
        dir,
    )

    run_ast_grep_rewrite(dir, "xj_astgrep_println($$$ARGS)", "println!($$$ARGS)")
    run_ast_grep_rewrite(dir, "xj_astgrep_print($$$ARGS)", "print!($$$ARGS)")
    run_ast_grep_rewrite(dir, "xj_astgrep_eprintln($$$ARGS)", "eprintln!($$$ARGS)")
    run_ast_grep_rewrite(dir, "xj_astgrep_eprint($$$ARGS)", "eprint!($$$ARGS)")

    return lift_cp


def quiet_cargo(args: list[str], cwd: Path, env_ext=None) -> CompletedProcess:
    cp = hermetic.run_cargo_on_translated_code(
        args, cwd=cwd, check=False, capture_output=True, env_ext=env_ext
    )
    if cp.returncode != 0:
        click.echo(
            f"TENJIN: cargo invocation failed in {cwd.as_posix()}:\n\t" + " ".join(args), err=True
        )
        click.echo("TENJIN: stderr was:\n" + cp.stderr.decode(), err=True)
        cp.check_returncode()
    return cp


def get_multitool_toolchain(root: Path) -> str:
    return hermetic.get_toolchain_for_directory(root / "xj-improve-multitool")


def run_improve_multitool(root: Path, tool: str, args: list[str], dir: Path) -> CompletedProcess:
    # External Cargo tools need not be installed to be used, they only need to be on the PATH.
    # Since rustc plugins like xj-improve-multitool are tied to a specific toolchain,
    # it's not ideal to install them globally.
    # We could unconditionally `cargo install` into the _local/bin directory,
    # but it's a bit faster to just build & run from `target`.
    target_subdir = os.environ.get("XJ_BUILD_RS_PROFILE", "debug")
    return quiet_cargo(
        [
            hermetic.tenjin_multitool_toolchain_specifier(),  # don't use the toolchain for generated Rust!
            "xj-improve-multitool",
            "--tool",
            tool,
            *args,
        ],
        cwd=dir,
        env_ext={
            "PATH": os.pathsep.join([
                str(root / "xj-improve-multitool" / "target" / target_subdir),
                os.environ["PATH"],
            ]),
        },
    )


type FindUnsafe2FnMetrics = dict[str, object]


def run_un_unsafe_improvement(root: Path, dir: Path):
    """Iteratively remove unsafe blocks.

    We run xj-improve-multitool to extract a condensed (approximate) call graph,
    serialize it to JSON (one per local crate), then process the graph in
    topological order. For each function (well, SCC of functions), we'll remove
    the `unsafe` marker from the function signature, re-run cargo check,
    and roll back the edit if the check fails.

    We also run `cargo find-unsafe2` to generate per-crate unsafe metrics.
    Those metrics guide the speculative cargo checks: we subtract the function's
    own `unsafe fn` marker and calls to callees known to be safe, then skip the
    speculative cargo check only when the remaining metric count is clearly too
    high to be worth probing. This keeps us from spending checks on obviously
    unsafe functions while still allowing cargo to judge noisy borderline cases.

    A remaining optimization would be for process_function_sccs() to operate on
    multiple independent SCCs simultaneously.
    """

    if os.environ.get("XJ_SKIP_UNUNSAFE", "0") == "1":
        click.echo("TENJIN NOTE: skipping un-unsafe improvement as XJ_SKIP_UNUNSAFE=1")
        return

    def hacky_rewrite_fn_range(path: Path, lo: int, hi: int, replacement: bytes):
        """Rewrite a range in a file to a replacement string."""
        with path.open("rb+") as f:
            content = f.read()
            assert (hi - lo) == len(replacement)
            content = content[:lo] + replacement + content[hi:]
            f.seek(0)
            f.write(content)
            f.truncate()

    class FunctionUnsafeNecessity(enum.Enum):
        """Status of a function with respect to unsafe blocks."""

        SAFE = 0
        UNSAFE = 1
        UNKNOWN = 2

    # `find-unsafe2` can see HIR-expanded implementation details, such as vec!
    # internals or dead union initializers left after earlier rewrites. A couple
    # of residual non-marker metrics are still cheap enough to let cargo decide.
    max_unsafe_metric_obligations_to_probe = 2

    def run_find_unsafe2(json_dir: Path) -> None:
        hermetic.run_cargo_on_translated_code(
            ["find-unsafe2"],
            cwd=dir,
            check=True,
            capture_output=True,
            env_ext={
                "FIND_UNSAFE2_SRC_DIR": str(dir.resolve()),
                "FIND_UNSAFE2_JSON_DIR": str(json_dir.resolve()),
            },
        )

    def read_find_unsafe2_fn_metrics(json_dir: Path) -> dict[str, FindUnsafe2FnMetrics]:
        metrics: dict[str, FindUnsafe2FnMetrics] = {}
        for item in json_dir.iterdir():
            if not (item.is_file() and item.suffix == ".json"):
                continue
            with item.open("rb") as f:
                data = json.load(f)
            if not isinstance(data, dict):
                continue
            fns = data.get("fns")
            if not isinstance(fns, dict):
                continue
            for fn_name, fn_metrics in fns.items():
                if isinstance(fn_name, str) and isinstance(fn_metrics, dict):
                    metrics[fn_name] = fn_metrics
        return metrics

    def load_find_unsafe2_fn_metrics() -> dict[str, FindUnsafe2FnMetrics] | None:
        try:
            with tempfile.TemporaryDirectory() as find_unsafe2_dir:
                json_dir = Path(find_unsafe2_dir)
                run_find_unsafe2(json_dir)
                return read_find_unsafe2_fn_metrics(json_dir)
        except CalledProcessError:
            return None

    find_unsafe2_metrics = load_find_unsafe2_fn_metrics()

    def default_function_unsafe_status(has_unsafe_span: bool) -> FunctionUnsafeNecessity:
        """This reflect our understanding of the function's safety requirements.
        c2rust must mark all possibly-unsafe functions as `unsafe`, because
        (A) neither rustc nor clippy can/will add unsafety markers on demand, and
        (B) rustc cannot remove unsafety markers on demand"""
        if has_unsafe_span:
            return FunctionUnsafeNecessity.UNKNOWN
        return FunctionUnsafeNecessity.SAFE

    def process_function_scc(
        cacg: CondensedSpanGraph,
        node: int,
        unsafe_spans: list[ExplicitSpan | None],
        unsafe_status: list[FunctionUnsafeNecessity],
        unsafe_metrics: list[FindUnsafe2FnMetrics | None],
        fn_names: list[str | None],
        callee_nodes_by_caller: dict[int, set[int]],
    ):
        unknown_status_spans = []
        fnids = cacg.nodes[node]
        for fnid in fnids:
            if unsafe_status[fnid] == FunctionUnsafeNecessity.UNKNOWN:
                unknown_status_spans.append(unsafe_spans[fnid])

        if not unknown_status_spans:
            return

        if should_skip_ununsafe_attempt(
            cacg,
            node,
            unsafe_spans,
            unsafe_status,
            unsafe_metrics,
            fn_names,
            callee_nodes_by_caller,
        ):
            for fnid in fnids:
                if unsafe_status[fnid] == FunctionUnsafeNecessity.UNKNOWN:
                    unsafe_status[fnid] = FunctionUnsafeNecessity.UNSAFE
            return

        rewriter = SpeculativeSpansEraser(
            unknown_status_spans,
            lambda span: Path(dir / cacg.files[span.fileid]),
        )
        rewriter.erase_spans()
        cp = hermetic.run_cargo_on_translated_code(
            ["check", "--message-format=json"],
            cwd=dir,
            check=False,
            capture_output=True,
        )
        if cp.returncode != 0:
            # print("WARNING: cargo check failed -- rewritten SCC must have been unsafe after all...")
            # print(cp.stdout.decode("utf-8").splitlines())
            # print()
            # print()
            # print(cp.stderr)
            # print()
            rewriter.restore()
            for fnid in fnids:
                unsafe_status[fnid] = FunctionUnsafeNecessity.UNSAFE
        else:
            # If the check passes, we can safely mark all functions in this SCC as SAFE.
            for fnid in fnids:
                unsafe_status[fnid] = FunctionUnsafeNecessity.SAFE

            # rustc may have emitted warning about unnecessary unsafe blocks.
            # Neither `cargo fix` nor `cargo clippy --fix` will remove them,
            # so we do it manually.
            lines = cp.stdout.decode("utf-8").splitlines()
            for line in lines:
                j = json.loads(line)
                try:
                    if (
                        j["reason"] == "compiler-message"
                        and j["message"]["code"]["code"] == "unused_unsafe"
                    ):
                        tgtpath = dir / j["message"]["spans"][0]["file_name"]
                        lo = j["message"]["spans"][0]["byte_start"]
                        hi = j["message"]["spans"][0]["byte_end"]
                        hacky_rewrite_fn_range(tgtpath, lo, hi, b" " * len("unsafe"))
                except:  # noqa: E722
                    print("TENJIN WARNING: unexpected JSON message from cargo check:", j)
                    continue

    def process_function_sccs(
        cacg: CondensedSpanGraph,
        ready_nodes: Sequence[int],
        unsafe_spans: list[ExplicitSpan | None],
        unsafe_status: list[FunctionUnsafeNecessity],
        unsafe_metrics: list[FindUnsafe2FnMetrics | None],
        fn_names: list[str | None],
        callee_nodes_by_caller: dict[int, set[int]],
    ):
        # Each ready node is a strongly connected component (SCC)
        # in the call graph, which means that it is a set of functions
        # which are mutually recursive and therefore must all have
        # the same unsafe status.

        # We could go faster by modifying all the ready nodes simultaneously.
        # That would require some more sophistication on our part
        # to map unexpected-unsafe-operation to the corresponding
        # function signature.
        for node in ready_nodes:
            process_function_scc(
                cacg,
                node,
                unsafe_spans,
                unsafe_status,
                unsafe_metrics,
                fn_names,
                callee_nodes_by_caller,
            )

    def find_name_for_fn_sig(cacg: CondensedSpanGraph, fnspan: ExplicitSpan) -> str | None:
        relpath = cacg.files[fnspan.fileid]
        filepath = Path(dir / relpath)
        assert filepath.is_file(), "Expected a file at the path: " + str(filepath)
        contents = filepath.read_bytes()
        snippet = contents[fnspan.lo : fnspan.hi]
        assert fnspan.hi > fnspan.lo, "Expected a non-empty span: " + str(fnspan)
        assert len(snippet) > 0, "Expected a non-empty snippet: " + snippet.decode(
            "utf-8", errors="replace"
        )
        signature = snippet.split(b"{", maxsplit=1)[0].decode("utf-8", errors="replace")
        if match := re.search(
            r"\bfn\s+(r#[A-Za-z_][A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*)", signature
        ):
            return match.group(1).removeprefix("r#")
        return None

    def find_metrics_for_fn_name(fn_name: str | None) -> FindUnsafe2FnMetrics | None:
        if fn_name is None or find_unsafe2_metrics is None:
            return None
        matches = [
            metrics
            for full_name, metrics in find_unsafe2_metrics.items()
            if full_name == fn_name or full_name.endswith(f"::{fn_name}")
        ]
        if len(matches) == 1:
            return matches[0]
        return None

    def int_metric(metrics: FindUnsafe2FnMetrics, key: str) -> int:
        value = metrics.get(key, 0)
        return value if isinstance(value, int) else 0

    def bool_metric(metrics: FindUnsafe2FnMetrics, key: str) -> bool:
        return metrics.get(key, False) is True

    def count_known_safe_callee_calls(
        cacg: CondensedSpanGraph,
        fnid: int,
        fn_names: list[str | None],
        safe_callee_nodes: set[int],
    ) -> int:
        fnspan = cacg.elts[fnid]
        contents = Path(dir / cacg.files[fnspan.fileid]).read_bytes()
        snippet = contents[fnspan.lo : fnspan.hi]
        count = 0
        for callee_node in safe_callee_nodes:
            for callee_fnid in cacg.nodes[callee_node]:
                callee_name = fn_names[callee_fnid]
                if callee_name is None:
                    continue
                call_pattern = (
                    rb"(?<![A-Za-z0-9_])" + re.escape(callee_name.encode("utf-8")) + rb"\s*\("
                )
                count += len(re.findall(call_pattern, snippet))
        return count

    def should_skip_ununsafe_attempt(
        cacg: CondensedSpanGraph,
        node: int,
        unsafe_spans: list[ExplicitSpan | None],
        unsafe_status: list[FunctionUnsafeNecessity],
        unsafe_metrics: list[FindUnsafe2FnMetrics | None],
        fn_names: list[str | None],
        callee_nodes_by_caller: dict[int, set[int]],
    ) -> bool:
        safe_callee_nodes = {
            callee
            for callee in callee_nodes_by_caller.get(node, set())
            if all(
                unsafe_status[fnid] == FunctionUnsafeNecessity.SAFE for fnid in cacg.nodes[callee]
            )
        }

        for fnid in cacg.nodes[node]:
            metrics = unsafe_metrics[fnid]
            if metrics is None:
                continue

            total_unsafe = int_metric(metrics, "total_unsafe")
            unsafe_marker_count = int(bool_metric(metrics, "is_unsafe_fn"))
            calls_unsafe = int_metric(metrics, "calls_unsafe")
            safe_callee_calls = count_known_safe_callee_calls(
                cacg, fnid, fn_names, safe_callee_nodes
            )
            adjusted_calls_unsafe = max(0, calls_unsafe - safe_callee_calls)
            adjusted_total_unsafe = total_unsafe - calls_unsafe + adjusted_calls_unsafe
            adjusted_non_marker_unsafe = max(0, adjusted_total_unsafe - unsafe_marker_count)
            if adjusted_non_marker_unsafe > max_unsafe_metric_obligations_to_probe:
                return True
        return False

    def find_unsafe_span_for_fn_sig(
        fnspan: ExplicitSpan,
    ) -> ExplicitSpan | None:
        relpath = cacg.files[fnspan.fileid]
        filepath = Path(dir / relpath)
        assert filepath.is_file(), "Expected a file at the path: " + str(filepath)
        # We need to read as bytes because the offsets will be wrong if the file
        # contains any non-ASCII UTF-8 characters.
        contents = filepath.read_bytes()
        snippet = contents[fnspan.lo : fnspan.hi]
        assert fnspan.hi > fnspan.lo, "Expected a non-empty span: " + str(fnspan)
        assert len(snippet) > 0, "Expected a non-empty snippet: " + snippet.decode(
            "utf-8", errors="replace"
        )

        if snippet.startswith(b"fn") or snippet.startswith(b"\nfn"):
            # Can't be unsafe!
            return None

        fnidx = snippet.find(b" fn ")
        if fnidx == -1:
            print(
                "WARNING: no fn signature found in span",
                fnspan,
                "in file",
                cacg.files[fnspan.fileid],
                "with snippet:",
                snippet,
                "\n\t\t\tlen snippet:",
                len(snippet),
            )
            return None
        snippet = contents[fnspan.lo : fnspan.lo + fnidx + 1]
        unsafe_idx = snippet.find(b"unsafe ")
        if unsafe_idx == -1:
            return None
        return ExplicitSpan(
            fileid=fnspan.fileid,
            lo=fnspan.lo + unsafe_idx,
            hi=fnspan.lo + unsafe_idx + len(b"unsafe"),
        )

    def remove_unsafe_blocks(cacg: CondensedSpanGraph):
        # The `elts` field contains full spans for functions;
        # we want to construct a sidecar list of the spans for the
        # unsafe keyword for each function (if it exists).
        unsafe_spans = [find_unsafe_span_for_fn_sig(fnspan) for fnspan in cacg.elts]
        unsafe_status = [default_function_unsafe_status(span is not None) for span in unsafe_spans]
        fn_names = [find_name_for_fn_sig(cacg, fnspan) for fnspan in cacg.elts]
        unsafe_metrics = [find_metrics_for_fn_name(fn_name) for fn_name in fn_names]

        # print("fn spans:", cacg.elts)
        # print("unsafe spans:", unsafe_spans)
        # print("unsafe status:", unsafe_status)

        node_for_elt: dict[int, int] = {}
        for nodeidx, elts in enumerate(cacg.nodes):
            for elt in elts:
                node_for_elt[elt] = nodeidx

        called_elts: set[int] = set()
        callee_nodes_by_caller: dict[int, set[int]] = {}

        # Use graphlib to collect ready nodes in topological order
        # from the *reverse* of the call graph -- we want to process
        # the leaves first, so we can remove unsafe blocks from them
        # before we get to the functions that call them.
        g: graphlib.TopologicalSorter = graphlib.TopologicalSorter()
        for caller, callee in cacg.edges:
            # Note that the add method here is (node, predecessor)
            # which is already the reverse of the "conventional"
            # graph edge direction of src -> tgt / pred -> node.
            g.add(caller, callee)
            callee_nodes_by_caller.setdefault(caller, set()).add(callee)
            called_elts = called_elts.union(cacg.nodes[callee])

        # For library crates, public API functions may not have internal
        # callers, so we must synthesize a virtual caller for them.
        for scc in cacg.nodes:
            for e in scc:
                if e not in called_elts:
                    g.add(-1, node_for_elt[e])

        g.prepare()
        while g.is_active():
            ready_nodes = g.get_ready()
            if ready_nodes != (-1,):
                process_function_sccs(
                    cacg,
                    ready_nodes,
                    unsafe_spans,
                    unsafe_status,
                    unsafe_metrics,
                    fn_names,
                    callee_nodes_by_caller,
                )
            g.done(*ready_nodes)

    with tempfile.TemporaryDirectory() as cacgdir:
        # Extract the call graph to a temporary directory.
        run_improve_multitool(root, "ExtractCACG", ["--cacg-json-outdir", cacgdir], dir)

        for item in Path(cacgdir).iterdir():
            if item.is_file() and item.name.endswith(".json"):
                cacg_json = json.load(item.open("rb"))
                cacg = CondensedSpanGraph.from_dict(cacg_json)  # type:ignore
                # Remove unsafe blocks from the call graph.
                remove_unsafe_blocks(cacg)


def hacky_whiteout_first_occurrence_within_first_n_bytes(contents: str, needle: str, n: int) -> str:
    # Find the first occurrence of the needle within the first n bytes
    first_n_bytes = contents[:n]
    start_idx = first_n_bytes.find(needle)
    if start_idx == -1:
        return contents  # No occurrence found, return original contents
    return contents[:start_idx] + (" " * len(needle)) + contents[start_idx + len(needle) :]


def run_trim_allows(root: Path, dir: Path):
    base_cp = hermetic.run_cargo_on_translated_code(
        ["check", "--message-format=json"], cwd=dir, check=True, capture_output=True
    )

    if base_cp.returncode != 0:
        print("TENJIN NOTE: skipping trim-allows as initial cargo check failed.")
        return

    things_to_trim = [
        "  dead_code,",
        "  mutable_transmutes,",
        "  unused_assignments,",
        "  unused_mut,",
        "  unused_mut",
        "#![feature(extern_types)]",
        "#![allow(dead_code)]",
        "#![allow(unused_mut)]",
        "#![allow(mutable_transmutes)]",
        "#![allow(unused_assignments)]",
    ]

    def rough_parse_inner_attributes_len(rs_file_content: str) -> int:
        """Estimate the length of the inner attribute prefix in a Rust file."""
        lines = rs_file_content.splitlines()
        prelude_len = 0
        for line in lines:
            if line.startswith("#![") or line.startswith("    ") or line.startswith(")]"):
                prelude_len += len(line) + 1
            else:
                break
        return prelude_len

    for path in Path(dir).rglob("*.rs"):
        if not path.is_file():
            continue

        rewriter = SpeculativeFileRewriter(path)
        prelude_len = rough_parse_inner_attributes_len(rewriter.original_content)

        for thing in things_to_trim:

            def whiteout_first_thing(contents: str) -> str:
                return hacky_whiteout_first_occurrence_within_first_n_bytes(
                    contents, thing, prelude_len
                )

            rewriter.update_content_via(whiteout_first_thing)
            changes_made = rewriter.write()
            if changes_made:
                cp = hermetic.run_cargo_on_translated_code(
                    ["check", "--message-format=json"], cwd=dir, check=False, capture_output=True
                )
                if cp.returncode != 0 or len(cp.stdout) > len(base_cp.stdout):
                    # If the check fails, we restore the original content.
                    print(
                        "TENJIN WARNING: cargo check failed after trimming",
                        thing,
                        "in",
                        dir,
                        "restoring original content.",
                    )
                    rewriter.restore()


def run_whiteout_clippy_no_effect_paths(root: Path, dir: Path) -> None:
    """Remove path statements with no effect, as reported by clippy.

    In idiomatic human-written Rust code this rewrite can be behavior-changing
    due to Drop trait implementations but c2rust's output does not yet use any
    non-trivial Drop impls, and it generates no-effect path statements for
    post increment operators."""
    messages, _res = static_measurements_rust.get_clippy_messages_json(dir)

    spans_to_erase: list[ExplicitSpan] = []
    files_map: dict[str, int] = {}
    files_list: list[ResolvedPath] = []

    for obj in messages:
        message = obj.get("message", {})
        if message.get("message") != "path statement with no effect":
            continue

        for span in message.get("spans", []):
            if not span.get("is_primary"):
                continue

            file_name = span.get("file_name")
            if file_name is None:
                continue

            # clippy file_name is relative to the crate root (`dir`)
            abs_path: ResolvedPath = (dir / file_name).resolve()  # TODO test-resolve
            abs_path_str = str(abs_path)

            if abs_path_str not in files_map:
                files_map[abs_path_str] = len(files_list)
                files_list.append(abs_path)

            fileid = files_map[abs_path_str]
            lo = span.get("byte_start")
            hi = span.get("byte_end")

            if lo is not None and hi is not None:
                spans_to_erase.append(ExplicitSpan(fileid=fileid, lo=int(lo), hi=int(hi)))

    if not spans_to_erase:
        return

    def span_to_path(span: ExplicitSpan) -> ResolvedPath:
        return files_list[span.fileid]

    rewriter = SpeculativeSpansEraser(spans_to_erase, span_to_path)
    rewriter.erase_spans()


def run_trivial_numeric_casts_improvement(root: Path, dir: Path) -> None:
    """Remove trivial numeric casts, as reported by clippy."""

    # Stats
    cargo_check_runs = 0
    cargo_check_failures = 0
    cargo_clippy_runs = 0

    def run_check(cwd: Path) -> bool:
        nonlocal cargo_check_runs, cargo_check_failures
        cargo_check_runs += 1
        cp = hermetic.run_cargo_on_translated_code(
            ["check"],
            cwd=cwd,
            check=False,
            capture_output=True,
        )
        if cp.returncode != 0:
            cargo_check_failures += 1
            return False
        return True

    def get_trivial_casts(cwd: Path) -> list[dict]:
        nonlocal cargo_clippy_runs
        cargo_clippy_runs += 1

        allowed_lints = [
            "unused",
            "static_mut_refs",
            "clippy::missing_safety_doc",
            "clippy::if_same_then_else",
        ]
        clippy_args = ["clippy", "--message-format=json", "--", "-W", "trivial_numeric_casts"]
        for lint in allowed_lints:
            clippy_args.extend(["-A", lint])

        cp = hermetic.run_cargo_on_translated_code(
            clippy_args,
            cwd=cwd,
            check=False,
            capture_output=True,
        )

        messages = []
        if cp.stdout:
            for line in cp.stdout.decode("utf-8").splitlines():
                try:
                    messages.append(json.loads(line))
                except json.JSONDecodeError:
                    continue

        trivial_casts = []
        file_contents: dict[str, str] = {}

        for msg in messages:
            if msg.get("reason") != "compiler-message":
                continue

            message = msg.get("message", {})
            if msg_code := message.get("code", {}):
                if not msg_code or msg_code.get("code") != "trivial_numeric_casts":
                    continue

            for span in message.get("spans", []):
                if not span.get("is_primary"):
                    continue

                file_path = dir / span["file_name"]
                if str(file_path) not in file_contents:
                    try:
                        with open(file_path, "r", encoding="utf-8") as f:
                            file_contents[str(file_path)] = f.read()
                    except FileNotFoundError:
                        continue

                content = file_contents[str(file_path)]
                span_text = content[span["byte_start"] : span["byte_end"]]

                if span["byte_start"] > 60:
                    # Quick hack: type-inferred variable initializers are not safe to remove
                    # annotations from, because the type of a constant may change when
                    # the cast is removed, changing the meaning of the code.
                    preceding_text = content[span["byte_start"] - 60 : span["byte_start"]]
                    if ";" in preceding_text:
                        preceding_text = preceding_text.split(";")[-1]
                    if " let " in preceding_text or "const " in preceding_text:
                        let_idx = preceding_text.rfind(" let ")
                        col_idx = preceding_text.rfind(": ")
                        eql_idx = preceding_text.rfind("=")
                        if let_idx != -1 and let_idx < col_idx < eql_idx:
                            # This is a let binding with an explicit type annotation; safe to remove.
                            pass
                        else:
                            continue

                if " as " in span_text:
                    val_part = span_text.split(" as ")[0].strip()
                    cleaned_val = val_part.replace("_", "")
                    try:
                        int(cleaned_val, base=0)
                        trivial_casts.append(span)
                    except ValueError:
                        continue
        return trivial_casts

    def apply_removals(casts: list[dict]) -> dict[str, SpeculativeFileRewriter]:
        rewriters: dict[str, SpeculativeFileRewriter] = {}
        for span in casts:
            file_name = span["file_name"]
            if file_name not in rewriters:
                rewriters[file_name] = SpeculativeFileRewriter(dir / file_name)

            rewriter = rewriters[file_name]

            def create_replacer(span_to_replace):
                def replacer(content: str) -> str:
                    start = span_to_replace["byte_start"]
                    end = span_to_replace["byte_end"]
                    original_snippet = content[start:end]

                    as_index = original_snippet.find(" as ")
                    if as_index != -1:
                        value_part = original_snippet[:as_index]
                        padding = " " * (len(original_snippet) - len(value_part))
                        new_snippet = value_part + padding
                        return content[:start] + new_snippet + content[end:]
                    return content

                return replacer

            rewriter.update_content_via(create_replacer(span))

        for rewriter in rewriters.values():
            rewriter.write()
        return rewriters

    def revert_removals(rewriters: dict[str, SpeculativeFileRewriter]):
        for rewriter in rewriters.values():
            rewriter.restore()

    def span_to_tuple(span) -> tuple[str, int, int]:
        return (span["file_name"], span["byte_start"], span["byte_end"])

    def find_all_bad_and_apply_good(casts: list[dict]) -> list[dict]:
        if not casts:
            return []

        rewriters = apply_removals(casts)
        if run_check(dir):
            # All good, leave applied.
            return []

        revert_removals(rewriters)

        if len(casts) == 1:
            return casts

        mid = len(casts) // 2
        h1 = casts[:mid]
        h2 = casts[mid:]

        bad1 = find_all_bad_and_apply_good(h1)
        # Good ones from h1 are now applied.

        bad2 = find_all_bad_and_apply_good(h2)
        # Good ones from h2 are now applied.

        return bad1 + bad2

    removed_count = 0
    known_failing_spans: set[tuple[str, int, int]] = set()
    while True:
        current_casts = get_trivial_casts(dir)
        current_spans = {span_to_tuple(c) for c in current_casts}

        if not current_spans or current_spans.issubset(known_failing_spans):
            break

        casts_to_try = [c for c in current_casts if span_to_tuple(c) not in known_failing_spans]

        new_failures = find_all_bad_and_apply_good(casts_to_try)

        removed_count += len(casts_to_try) - len(new_failures)

        for failure in new_failures:
            known_failing_spans.add(span_to_tuple(failure))

        # Even if we had no new failures, we must loop and check again
        # in case clippy didn't feed us all the trivial casts in one go.

    print(
        f"TENJIN: Trivial numeric cast removal: Ran cargo clippy+check {cargo_clippy_runs}+{cargo_check_runs} times,"
        f" with {cargo_check_failures} failures."
    )
    print(f"TENJIN: Trivial numeric cast removal: Removed {removed_count} casts.")


def elapsed_ms_of_ns(start_ns: int, end_ns: int) -> float:
    """Calculate elapsed time in milliseconds from nanoseconds."""
    return (end_ns - start_ns) / 1_000_000.0


def run_improvement_passes(
    root: Path, output: Path, resultsdir: Path, cratename: str, tracker: ingest_tracking.TimingRepo
):
    def run_cargo_fmt(_root: Path, dir: Path) -> CompletedProcess:
        cp1 = hermetic.run_cargo_in(
            ["fmt"],
            cwd=dir,
            check=False,  # formatting failure is not a fatal error
            capture_output=True,
        )
        if cp1.returncode != 0:
            if b"left behind trailing whitespace" in cp1.stderr:
                for rs_file in dir.rglob("*.rs"):
                    if not rs_file.is_file():
                        continue
                    with rs_file.open("r", encoding="utf-8") as f:
                        lines = f.readlines()
                    with rs_file.open("w", encoding="utf-8") as f:
                        for line in lines:
                            f.write(line.rstrip() + "\n")
                cp2 = hermetic.run_cargo_on_translated_code(
                    ["fmt"],
                    cwd=dir,
                    check=False,
                    capture_output=True,
                )
                return cp2
        return cp1

    def run_cargo_fix(_root: Path, dir: Path) -> CompletedProcess:
        return hermetic.run_cargo_on_translated_code(
            ["fix", "--allow-no-vcs", "--allow-dirty"],
            cwd=dir,
            check=True,
            capture_output=True,
        )

    def run_cargo_clippy_fix(_root: Path, dir: Path) -> CompletedProcess:
        return hermetic.run_cargo_on_translated_code(
            ["clippy", "--fix", "--allow-no-vcs", "--allow-dirty"],
            cwd=dir,
            check=True,
            capture_output=True,
        )

    improvement_passes: list[tuple[str, Callable[[Path, Path], CompletedProcess | None]]] = [
        (
            "synsub",
            lambda root, dir: run_improve_synsub(root, ["--modify-in-place"], dir),
        ),
        (
            "lift-call-args",
            lambda root, dir: run_improve_lift_call_args(root, ["--modify-in-place"], dir),
        ),
        ("fmt", run_cargo_fmt),
        ("fix", run_cargo_fix),
        (
            "trimdead",
            lambda root, dir: run_improve_multitool(
                root, "TrimDeadItems", ["--modify-in-place"], dir
            ),
        ),
        ("ununsafe", run_un_unsafe_improvement),
        # We run `fix` immedately after removing unsafe markers, because
        # removing an unsafe marker on a block may have rendered
        # the block safe, and therefore subject to removal.
        # But if we format first, the block may not be removable by `fix`!
        ("fix", run_cargo_fix),
        # Numeric cast removal should come before `clippy fix` because the latter
        # can collapse literal casts into suffixed literals, which won't be counted
        # as fixable.
        ("trivial-numeric-casts", run_trivial_numeric_casts_improvement),
        ("clippy-fix", run_cargo_clippy_fix),
        ("clippy-whiteout-no-effect-paths", run_whiteout_clippy_no_effect_paths),
        ("trim-allows", run_trim_allows),
        ("fmt", run_cargo_fmt),
    ]

    prev = output
    for counter, (tag, func) in enumerate(improvement_passes, start=1):
        newdir = resultsdir / f"{counter:02d}_{tag}"
        with tracker.tracking(f"improvement_pass_{counter:02d}_{tag}", newdir) as _step:
            start_ns = time.perf_counter_ns()
            shutil.copytree(prev, newdir)
            # Run the actual improvement pass, modifying the contents of `newdir`.
            cp_or_None: CompletedProcess | None = func(root, newdir)
            if cp_or_None is not None:
                _step.update_sub(cp_or_None)
                if cp_or_None.returncode != 0:
                    print(
                        f"TENJIN WARNING: improvement pass {counter} ({tag}) failed, skipping further passes."
                    )
                    break

            mid_ns = time.perf_counter_ns()
            # Assumption: the first two passes are `synsub` and `lift-call-args`.
            # The output of synsub is assumed to be type-correct but not necessarily
            # borrowck-correct, so we only run `cargo check` on passes after the first.
            if counter > 1:
                # Use explicit toolchain for checks because c2rust may use extern_types which is unstable.
                quiet_cargo(["check"], cwd=newdir)
                # Clean up the target directory so the next pass starts fresh.
                quiet_cargo(
                    [
                        "clean",
                        *cargo_workspace_helpers.flags_for_all_cargo_workspace_packages(newdir),
                    ],
                    cwd=newdir,
                )
                # Ensure any plugin results are also removed, since they may prevent
                # subsequent passes from running correctly.
                for child in (newdir / "target").glob("plugin-*"):
                    if child.is_dir():
                        shutil.rmtree(child)
            end_ns = time.perf_counter_ns()

            core_ms = round(elapsed_ms_of_ns(start_ns, mid_ns))
            cleanup_ms = round(elapsed_ms_of_ns(mid_ns, end_ns))

            print(
                "Improvement pass",
                counter,
                tag,
                "took",
                core_ms,
                "ms (then cleanup took another",
                cleanup_ms,
                "ms)",
            )
            print()
            print()
            print()
            prev = newdir
