"""Golden tests for xj-prepare-pointertransform over cases that trigger the RustSlice transformation"""

from pathlib import Path

import pytest

from pointer_transform_utils import run_case

_CASES_DIR = Path(__file__).parent / "slice_transform_cases"
_CASES = sorted(p.name for p in _CASES_DIR.iterdir() if p.is_dir())


@pytest.mark.parametrize("case", _CASES)
def test_slice_transform_case(root, test_tmp_dir, case):
    # `root` is requested for its side effect of building the tool.
    run_case(test_tmp_dir, _CASES_DIR / case)
