from itertools import pairwise

from caching_file_contents import CachingFileContents
from tenj_types import FilePathStr


class BatchingRewriter:
    """
    Context manager for batching multiple rewrites to multiple files.
    Each rewrite is a tuple: (filepath, offset, length, replacement_text).

    Offsets and lengths always refer to the original file. Replacements may
    not overlap, and insertions may not occur inside a replaced range.
    Insertions at the same offset are emitted in registration order.
    """

    def __init__(self) -> None:
        self.rewrites: dict[FilePathStr, list[tuple[int, int, str]]] = {}  # type: ignore
        self.contents_cache = CachingFileContents()

    def add_rewrite(self, filepath: FilePathStr, offset: int, length: int, replacement_text: str):
        """Add a rewrite operation for a specific file."""
        if filepath not in self.rewrites:
            self.rewrites[filepath] = []
        self.rewrites[filepath].append((offset, length, replacement_text))

    def get_content(self, filepath: FilePathStr) -> bytes:
        """Get the current content of a file."""
        return self.contents_cache.get_bytes(filepath)

    def get_rewrites(self, reverse: bool = True) -> dict[str, list[tuple[int, int, str]]]:
        return {k: sorted_file_rewrites(v, reverse=reverse) for k, v in self.rewrites.items()}

    def replace_rewrites(self, rewrites: dict[str, list[tuple[int, int, str]]]):
        self.rewrites = rewrites

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if exc_type is not None:
            return False  # propagate exception

        self.apply_rewrites()

    def apply_rewrites(self):
        if not self.rewrites:
            return
        for filepath, file_rewrites in self.rewrites.items():
            # Read file contents
            with open(filepath, "rb") as f:
                content = f.read()
            content = apply_file_rewrites(content, file_rewrites, filepath)
            # Write back to file
            with open(filepath, "wb") as f:
                f.write(content)

    def capture_snapshot(self) -> dict[str, bytes]:
        """Capture a snapshot of the current contents of all files involved in rewrites
        (without any pending rewrites applied)."""
        snapshot = {}
        for filepath in self.rewrites.keys():
            snapshot[filepath] = self.get_content(filepath)
        return snapshot

    def restore_snapshot(self, snapshot: dict[str, bytes]):
        """Restore the contents of files from a previously captured snapshot."""
        for filepath, content in snapshot.items():
            with open(filepath, "wb") as f:
                f.write(content)


def sorted_file_rewrites(
    file_rewrites: list[tuple[int, int, str]], reverse: bool = True
) -> list[tuple[int, int, str]]:
    # Python's sort is stable, so rewrites at the same offset retain their
    # registration order.
    return sorted(file_rewrites, key=lambda r: r[0], reverse=reverse)


def apply_file_rewrites(
    content: bytes, file_rewrites: list[tuple[int, int, str]], filepath: FilePathStr
) -> bytes:
    insertions: dict[int, list[str]] = {}
    replacements: list[tuple[int, int, str]] = []
    seen_replacements: set[tuple[int, int, str]] = set()

    for offset, length, replacement_text in file_rewrites:
        if offset < 0 or length < 0 or offset + length > len(content):
            raise ValueError(
                f"Rewrite out of bounds: offset={offset}, length={length}, file={filepath}"
            )
        if length == 0:
            insertions.setdefault(offset, []).append(replacement_text)
        elif (offset, length, replacement_text) not in seen_replacements:
            replacements.append((offset, offset + length, replacement_text))
            seen_replacements.add((offset, length, replacement_text))

    replacements.sort(key=lambda replacement: (replacement[0], replacement[1]))
    for previous, current in pairwise(replacements):
        previous_start, previous_end, previous_text = previous
        current_start, current_end, current_text = current
        if current_start >= previous_end:
            continue
        if previous_start == current_start and previous_end == current_end:
            raise ValueError(
                "conflicting replacements at "
                f"[{current_start}, {current_end}) in {filepath}: "
                f"{previous_text!r} and {current_text!r}"
            )
        raise ValueError(
            "overlapping replacements at "
            f"[{previous_start}, {previous_end}) and [{current_start}, {current_end}) "
            f"in {filepath}"
        )

    for insertion_offset in insertions:
        for replacement_start, replacement_end, _replacement_text in replacements:
            if replacement_start < insertion_offset < replacement_end:
                raise ValueError(
                    f"insertion at {insertion_offset} is inside replacement "
                    f"[{replacement_start}, {replacement_end}) in {filepath}"
                )

    replacements_by_start = {replacement[0]: replacement for replacement in replacements}
    edit_offsets = sorted(set(insertions) | set(replacements_by_start))
    output = bytearray()
    cursor = 0
    for offset in edit_offsets:
        output.extend(content[cursor:offset])
        for insertion in insertions.get(offset, []):
            output.extend(insertion.encode())
        replacement = replacements_by_start.get(offset)
        if replacement is None:
            cursor = offset
        else:
            _start, end, replacement_text = replacement
            output.extend(replacement_text.encode())
            cursor = end
    output.extend(content[cursor:])
    return bytes(output)
