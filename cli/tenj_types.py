from pathlib import Path

from click import style

type ResolvedPath = Path
type FilePathStr = str
type RelativeFilePathStr = str
type FileContentsStr = str
type PreprocessorDefinition = tuple[str, str | None]
type PerFilePreprocessorDefinitions = dict[str, list[PreprocessorDefinition]]

type CIdentifier = str
type ClangUSR = str


def style_path(path: Path) -> str:
    return style(str(path), fg="bright_cyan", bold=True)


def style_flag(flag: str) -> str:
    return style(flag, fg="bright_white", bold=True)


class UserFacingError(Exception):
    """Base class for errors that are expected to be shown to the user,
    without a stack trace."""

    pass
