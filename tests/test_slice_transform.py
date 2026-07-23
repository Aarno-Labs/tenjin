"""Golden fixture tests for xj-prepare-pointertransform over cases that
exercise the RustSlice signature reshaping specifically (root (ptr,len)
and (lo,hi) functions, singleton callees, call-graph propagation,
T*->int return collapsing, typedef/forward-decl handling). Cases that
only exercise the index rewriting live in test_pointer_transform.py."""

from pathlib import Path

import pytest

from ptr_slice_fixture_harness import run_case

_CASES_DIR = Path(__file__).parent / "slice_transform_cases"
_CASES = sorted(p.name for p in _CASES_DIR.iterdir() if p.is_dir())


@pytest.mark.parametrize("case", _CASES)
def test_slice_transform_case(root, test_tmp_dir, case):
    run_case(root, test_tmp_dir, _CASES_DIR / case)
