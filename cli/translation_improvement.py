import os
import enum
import json
import time
import shutil
import tempfile
import graphlib
from pathlib import Path
from typing import Callable, Sequence
from subprocess import CompletedProcess

import ingest_tracking
import hermetic
from speculative_rewriters import (
    CondensedSpanGraph,
    ExplicitSpan,
    SpeculativeFileRewriter,
    SpeculativeSpansEraser,
)


def get_multitool_toolchain(root: Path) -> str:
    return hermetic.get_toolchain_for_directory(root / "xj-improve-multitool")


def run_improve_multitool(root: Path, tool: str, args: list[str], dir: Path) -> CompletedProcess:
    # External Cargo tools need not be installed to be used, they only need to be on the PATH.
    # Since rustc plugins like xj-improve-multitool are tied to a specific toolchain,
    # it's not ideal to install them globally.
    # We could unconditionally `cargo install` into the _local/bin directory,
    # but it's a bit faster to just build & run from `target`.
    return hermetic.run_cargo_in(
        ["xj-improve-multitool", "--tool", tool, *args],
        cwd=dir,
        env_ext={
            "PATH": os.pathsep.join([
                str(root / "xj-improve-multitool" / "target" / "debug"),
                os.environ["PATH"],
            ]),
        },
        check=True,
    )


def run_un_unsafe_improvement(root: Path, dir: Path):
    """Iteratively remove unsafe blocks.

    We run xj-improve-multitool to extract a condensed (approximate) call graph,
    serialize it to JSON (one per local crate), then process the graph in
    topological order. For each function (well, SCC of functions), we'll remove
    the `unsafe` marker from the function signature, re-run cargo check,
    and roll back the edit if the check fails.

    There are two potentially significant optimizations one can do
    to reduce the number of times we run cargo check:
    1. process_function_sccs() could operate on multiple independent
       SCCs simultaneously.
    2. When function F mentions G, and G is known to be unsafe,
       we can assume F will also be judged unsafe by rustc.
       This is merely an approximation, since F may merely
       pass around G by address, for example, but in the common case
       it will be correct, and it's probably worth doing
       to unlock better scaling for larger crates. We are already tracking
       `unsafe_status` but are not yet using it.
    """

    def hacky_rewrite_fn_range(path: Path, lo: int, hi: int, replacement: str):
        """Rewrite a range in a file to a replacement string."""
        with path.open("r+") as f:
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
    ):
        fnids = cacg.nodes[node]
        unknown_status_spans = []
        for fnid in fnids:
            if unsafe_status[fnid] == FunctionUnsafeNecessity.UNKNOWN:
                unknown_status_spans.append(unsafe_spans[fnid])

        if not unknown_status_spans:
            return

        rewriter = SpeculativeSpansEraser(
            unknown_status_spans,
            lambda span: Path(dir / cacg.files[span.fileid]),
        )
        rewriter.erase_spans()
        cp = hermetic.run_cargo_in(
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
                        hacky_rewrite_fn_range(tgtpath, lo, hi, " " * len("unsafe"))
                except:  # noqa: E722
                    print("TENJIN WARNING: unexpected JSON message from cargo check:", j)
                    continue

    def process_function_sccs(
        cacg: CondensedSpanGraph,
        ready_nodes: Sequence[int],
        unsafe_spans: list[ExplicitSpan | None],
        unsafe_status: list[FunctionUnsafeNecessity],
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
            process_function_scc(cacg, node, unsafe_spans, unsafe_status)

    def find_unsafe_span_for_fn_sig(
        fnspan: ExplicitSpan,
    ) -> ExplicitSpan | None:
        filepath = Path(dir / cacg.files[fnspan.fileid])
        assert filepath.is_file(), "Expected a file at the path: " + str(filepath)
        contents = filepath.read_text("utf-8")
        snippet = contents[fnspan.lo : fnspan.hi]
        assert fnspan.hi > fnspan.lo, "Expected a non-empty span: " + str(fnspan)
        assert len(snippet) > 0, "Expected a non-empty snippet: " + snippet

        if snippet.startswith("fn") or snippet.startswith("\nfn"):
            # Can't be unsafe!
            return None

        fnidx = snippet.find(" fn ")
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
        unsafe_idx = snippet.find("unsafe ")
        if unsafe_idx == -1:
            return None
        return ExplicitSpan(
            fileid=fnspan.fileid,
            lo=fnspan.lo + unsafe_idx,
            hi=fnspan.lo + unsafe_idx + len("unsafe"),
        )

    def remove_unsafe_blocks(cacg: CondensedSpanGraph):
        # The `elts` field contains full spans for functions;
        # we want to construct a sidecar list of the spans for the
        # unsafe keyword for each function (if it exists).
        unsafe_spans = [find_unsafe_span_for_fn_sig(fnspan) for fnspan in cacg.elts]
        unsafe_status = [default_function_unsafe_status(span is not None) for span in unsafe_spans]

        # print("fn spans:", cacg.elts)
        # print("unsafe spans:", unsafe_spans)
        # print("unsafe status:", unsafe_status)

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

        g.prepare()
        while g.is_active():
            ready_nodes = g.get_ready()
            process_function_sccs(cacg, ready_nodes, unsafe_spans, unsafe_status)
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
    base_cp = hermetic.run_cargo_in(
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
                cp = hermetic.run_cargo_in(
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


def elapsed_ms_of_ns(start_ns: int, end_ns: int) -> float:
    """Calculate elapsed time in milliseconds from nanoseconds."""
    return (end_ns - start_ns) / 1_000_000.0


def run_improvement_passes(
    root: Path, output: Path, resultsdir: Path, cratename: str, tracker: ingest_tracking.TimingRepo
):
    def run_cargo_fmt(_root: Path, dir: Path) -> CompletedProcess:
        return hermetic.run_cargo_in(
            ["fmt"],
            cwd=dir,
            check=True,
            capture_output=True,
        )

    def run_cargo_fix(_root: Path, dir: Path) -> CompletedProcess:
        return hermetic.run_cargo_in(
            ["fix", "--allow-no-vcs", "--allow-dirty"],
            cwd=dir,
            check=True,
            capture_output=True,
        )

    improvement_passes: list[tuple[str, Callable[[Path, Path], CompletedProcess | None]]] = [
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
            mid_ns = time.perf_counter_ns()
            # Use explicit toolchain for checks because c2rust may use extern_types which is unstable.
            hermetic.run_cargo_in(["check"], cwd=newdir, check=True)
            # Clean up the target directory so the next pass starts fresh.
            hermetic.run_cargo_in(["clean", "-p", cratename], cwd=newdir, check=True)
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
