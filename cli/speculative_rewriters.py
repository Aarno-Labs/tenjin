from pathlib import Path
from typing import Callable, Sequence
from dataclasses import dataclass

from dataclasses_json import dataclass_json


# XREF:CONDENSED_SPAN_GRAPH_REPRESENTATION
@dataclass_json
@dataclass
class ExplicitSpan:
    fileid: int
    lo: int
    hi: int


@dataclass_json
@dataclass
class CondensedSpanGraph:
    files: list[Path]
    elts: list[ExplicitSpan]  # full spans for functions
    nodes: list[list[int]]  # indices into `elts`
    edges: list[tuple[int, int]]  # indices into `nodes`


class SpeculativeFileRewriter:
    def __init__(self, p: Path):
        self.p = p
        self.original_content = p.read_bytes()
        self.cached_content = self.original_content

    def update_content_via(self, fn: Callable[[bytes], bytes]):
        self.cached_content = fn(self.cached_content)

    def write(self) -> bool:
        """Returns true if any changes were made."""
        if self.cached_content != self.original_content:
            self.p.write_bytes(self.cached_content)
            return True
        return False

    def restore(self):
        self.p.write_bytes(self.original_content)


class SpeculativeSpansEraser:
    def __init__(
        self, spans: Sequence[ExplicitSpan | None], span_to_path: Callable[[ExplicitSpan], Path]
    ):
        self.spans = spans
        self.rewriters: dict[Path, SpeculativeFileRewriter] = {}
        self.spans_by_path: dict[Path, list[ExplicitSpan]] = {}

        for span in spans:
            if span is not None:
                canonical = span_to_path(span).resolve()
                self.spans_by_path.setdefault(canonical, []).append(span)
                if canonical not in self.rewriters:
                    self.rewriters[canonical] = SpeculativeFileRewriter(canonical)

    def erase_spans(self):
        def erase_span(span: ExplicitSpan, content: bytes) -> bytes:
            """Erase the span from the content."""
            assert span.hi > span.lo, "Span must be non-empty"
            assert span.hi <= len(content), "Span end must be within content bounds"
            return content[: span.lo] + (b" " * (span.hi - span.lo)) + content[span.hi :]

        for rewriter in self.rewriters.values():
            for span in self.spans_by_path[rewriter.p]:
                rewriter.update_content_via(lambda content, span=span: erase_span(span, content))
            rewriter.write()

    def restore(self):
        for rewriter in self.rewriters.values():
            rewriter.restore()


class SpeculativeFilesEraser:
    def __init__(self, files: Sequence[Path]):
        self.files = files
        self.rewriters: dict[Path, SpeculativeFileRewriter] = {
            file: SpeculativeFileRewriter(file) for file in files
        }

    def add_file(self, file: Path) -> SpeculativeFileRewriter:
        if file not in self.rewriters:
            self.rewriters[file] = SpeculativeFileRewriter(file)

        return self.rewriters[file]

    def restore(self):
        for rewriter in self.rewriters.values():
            rewriter.restore()
