"""Golden tests for the xj-prepare-pointertransform ->
xj-prepare-slicetransform chain over cases that exercise RustSlice
signature reshaping. See ptr_slice_fixture_harness for the protocol."""

from pathlib import Path

import pytest

from ptr_slice_fixture_harness import run_case

_CASES_DIR = Path(__file__).parent / "slice_transform_cases"
_CASES = sorted(p.name for p in _CASES_DIR.iterdir() if p.is_dir())


@pytest.mark.parametrize("case", _CASES)
def test_slice_transform_case(root, test_tmp_dir, case):
    run_case(root, test_tmp_dir, _CASES_DIR / case)
