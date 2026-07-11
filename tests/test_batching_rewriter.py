from pathlib import Path

import pytest

from batching_rewriter import BatchingRewriter


def rewrite(path: Path, edits: list[tuple[int, int, str]]) -> str:
    with BatchingRewriter() as rewriter:
        for offset, length, replacement in edits:
            rewriter.add_rewrite(path.as_posix(), offset, length, replacement)
    return path.read_text(encoding="utf-8")


def test_applies_non_overlapping_replacements_in_original_coordinates(tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("0123456789", encoding="utf-8")

    result = rewrite(path, [(2, 2, "ab"), (7, 2, "XYZ")])

    assert result == "01ab456XYZ9"


def test_insertion_at_replacement_start_precedes_replacement(tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("before OLD after", encoding="utf-8")
    start = len("before ")

    result = rewrite(path, [(start, 0, "INSERT "), (start, len("OLD"), "NEW")])

    assert result == "before INSERT NEW after"


def test_insertion_at_replacement_end_follows_replacement(tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("before OLD after", encoding="utf-8")
    start = len("before ")
    end = start + len("OLD")

    result = rewrite(path, [(end, 0, " INSERT"), (start, len("OLD"), "NEW")])

    assert result == "before NEW INSERT after"


def test_colocated_insertions_preserve_registration_order(tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("ab", encoding="utf-8")

    result = rewrite(path, [(1, 0, "first"), (1, 0, "second")])

    assert result == "afirstsecondb"


def test_identical_replacements_are_deduplicated(tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("abc", encoding="utf-8")

    result = rewrite(path, [(1, 1, "X"), (1, 1, "X")])

    assert result == "aXc"


def test_conflicting_replacements_are_rejected(tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("abc", encoding="utf-8")

    with pytest.raises(ValueError, match="conflicting replacements"):
        rewrite(path, [(1, 1, "X"), (1, 1, "Y")])


def test_partially_overlapping_replacements_are_rejected(tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("abcdef", encoding="utf-8")

    with pytest.raises(ValueError, match="overlapping replacements"):
        rewrite(path, [(1, 3, "X"), (3, 2, "Y")])


def test_insertion_inside_replacement_is_rejected(tmp_path):
    path = tmp_path / "input.txt"
    path.write_text("abcdef", encoding="utf-8")

    with pytest.raises(ValueError, match="inside replacement"):
        rewrite(path, [(1, 4, "X"), (3, 0, "Y")])
