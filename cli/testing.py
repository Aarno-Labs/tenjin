"""
Common definitions for tests

Definitions decorated with `pytest.fixture` are pytest test fixtures
(see pytest documentation). In short, they are executed by
by the pytest framework before a test that requests them, that is,
given a test

`test_foo(a, b, c)`

the framework will look for a fixture named `a`, execute it
(potentially pulling in other fixtures), and provide the returned
value as the value for `a` within `test_foo`.
"""

from pathlib import Path
import pytest

import cli_subcommands
import repo_root


@pytest.fixture
def root() -> Path:
    """The tenjin root dir"""
    root = repo_root.find_repo_root_dir_Path()
    cli_subcommands.do_build_rs(root)
    return root


@pytest.fixture
def test_dir(request) -> Path:
    """The directory containing the test script"""
    return request.path.parent


@pytest.fixture
def test_tmp_dir(tmp_path) -> Path:
    """A temporary directory created for the currently executing test"""
    return tmp_path


@pytest.fixture
def tmp_codebase(test_tmp_dir) -> Path:
    """A codebase directory name that has not yet been created"""
    codebase = test_tmp_dir / "codebase"
    return codebase


@pytest.fixture
def tmp_resultsdir(test_tmp_dir) -> Path:
    """A results directory that has already been created"""
    resultsdir = test_tmp_dir / "resultsdir"
    resultsdir.mkdir()
    return resultsdir
