import pytest

import cli_subcommands
import repo_root

@pytest.fixture
def root():
    root = repo_root.find_repo_root_dir_Path()
    cli_subcommands.do_build_rs(root)
    return root

@pytest.fixture
def test_dir(request):
  return request.path.parent

@pytest.fixture
def test_tmp_dir(tmp_path):
  return tmp_path

@pytest.fixture
def tmp_codebase(test_tmp_dir):
  codebase = test_tmp_dir / "codebase"
  return codebase

@pytest.fixture
def tmp_resultsdir(test_tmp_dir):
  resultsdir = test_tmp_dir / "resultsdir"
  resultsdir.mkdir()
  return resultsdir
