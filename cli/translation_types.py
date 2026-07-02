from dataclasses import dataclass
from pathlib import Path

from tenj_types import ResolvedPath


@dataclass
class TranslationFlags:
    root: Path
    codebase: Path
    resultsdir: Path
    cratename: str
    cmake_defines: list[str]
    do_not_refactor_headers_within: list[ResolvedPath]
    prebuildcmd: str | None
    buildcmd: str | None

    @classmethod
    def simple(
        cls,
        root: Path,
        codebase: Path,
        resultsdir: Path,
        cratename: str = "tenjinized",
        prebuildcmd: str | None = None,
        buildcmd: str | None = None,
    ) -> "TranslationFlags":
        return cls(
            root=root,
            codebase=codebase,
            resultsdir=resultsdir,
            cratename=cratename,
            cmake_defines=[],
            do_not_refactor_headers_within=[],
            prebuildcmd=prebuildcmd,
            buildcmd=buildcmd,
        )

    def for_combo(self, resultsdir: Path, cmake_defines: list[str]) -> "TranslationFlags":
        return TranslationFlags(
            root=self.root,
            codebase=self.codebase,
            resultsdir=resultsdir,
            cratename=self.cratename,
            cmake_defines=cmake_defines,
            do_not_refactor_headers_within=self.do_not_refactor_headers_within,
            prebuildcmd=self.prebuildcmd,
            buildcmd=self.buildcmd,
        )
