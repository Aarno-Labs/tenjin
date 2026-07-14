from pathlib import Path

import batching_rewriter
from c_refact import XjLocateJoinedDeclsOutput
import repo_root


def apply_decl_splitting_rewrites(current_codebase: Path, j: XjLocateJoinedDeclsOutput) -> None:
    """Apply rewrites to (when safe) duplicate conjoined declarators.
    For example, converts `int a, *b[2];` into `int a; int *b[2];`.

    This simplifies declaration edit broadcasting when lifting
    global variables.

    Of note, this pass is specifically designed to run before
    preprocessing, so that it can modify declarations in header files.
    The tradeoff is that it does not modify declarations created
    inside macros."""
    with batching_rewriter.BatchingRewriter() as rewriter:
        for e in j["edits"]:
            r = e["r"]
            if r is None:
                continue

            if e["cat"] == "local":
                # Skip function-local declarations; we only want to modify
                # global, field, and typedef declarations.
                continue

            if "{" in e["prefix"]:
                # Duplicating a prefix with an embedded type definition would
                # declare distinct tags or redefine a tag in the same scope.
                continue

            srcfile = Path(r["f"])
            if not srcfile.is_absolute():
                srcfile = current_codebase / srcfile

            if srcfile.is_relative_to(repo_root.localdir()):
                # Headers within the `_local` directory are ineligible for rewriting.
                continue

            assert srcfile.is_relative_to(current_codebase), (
                f"srcfile {srcfile} is not relative to current_codebase {current_codebase}"
            )
            startoff = r["b"]
            length = r["e"] - r["b"]

            def d(declarator: str) -> str:
                return e["prefix"] + declarator

            new_text = ";\n".join(d(decl) for decl in e["declarators"])
            rewriter.add_rewrite(srcfile.as_posix(), startoff, length, new_text)
