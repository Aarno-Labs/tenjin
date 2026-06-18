from pathlib import Path

import batching_rewriter
from c_refact import XjHoistEmbeddedTagDefsOutput


def apply_tag_hoisting_rewrites(current_codebase: Path, j: XjHoistEmbeddedTagDefsOutput) -> None:
    with batching_rewriter.BatchingRewriter() as rewriter:
        for e in j["edits"]:
            insert = e["insert"]
            replace = e["replace"]
            if insert is None or replace is None:
                continue

            srcfile = Path(replace["f"])
            if not srcfile.is_absolute():
                srcfile = current_codebase / srcfile
            assert srcfile.is_relative_to(current_codebase)

            insert_file = Path(insert["f"])
            if not insert_file.is_absolute():
                insert_file = current_codebase / insert_file
            assert insert_file == srcfile

            if insert["b"] == replace["b"]:
                rewriter.add_rewrite(
                    srcfile.as_posix(),
                    replace["b"],
                    replace["e"] - replace["b"],
                    e["insert_text"] + e["replace_text"],
                )
            else:
                rewriter.add_rewrite(srcfile.as_posix(), insert["b"], 0, e["insert_text"])
                rewriter.add_rewrite(
                    srcfile.as_posix(),
                    replace["b"],
                    replace["e"] - replace["b"],
                    e["replace_text"],
                )

            for ref in e["refs"]:
                r = ref["r"]
                if r is None:
                    continue
                ref_file = Path(r["f"])
                if not ref_file.is_absolute():
                    ref_file = current_codebase / ref_file
                assert ref_file == srcfile
                rewriter.add_rewrite(
                    srcfile.as_posix(),
                    r["b"],
                    r["e"] - r["b"],
                    ref["text"],
                )
