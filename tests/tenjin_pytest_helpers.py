import json
from pathlib import Path
import tempfile

import pytest_html.extras

import hermetic


def annotate_pytest_request_with_translation_notes(request, tmp_resultsdir: Path, extras):
    if (tmp_resultsdir / "translation_metadata.json").exists():
        metadata_text = (tmp_resultsdir / "translation_metadata.json").read_text(encoding="utf-8")
        metadata_json = json.loads(metadata_text)

        # This would attach a "file" (as a data: URI) -- not what we want!
        # extras.append(pytest_html.extras.text(metadata_text))

        # This puts the metadata text directly in the report, preceding the
        # test's captured CLI output.
        extras.append(pytest_html.extras.html("<pre>" + metadata_text + "</pre>"))

        before = (
            metadata_json.get("results", {})
            .get("c2rust_baseline", {})
            .get("total_unsafe_fns_count")
        )
        after = (
            metadata_json.get("results", {}).get("tenjin_final", {}).get("total_unsafe_fns_count")
        )
        request.node.summary_html = f"unsafe: {before}->{after}"


def run_cargo_on_final(cwd: Path, args: list[str], capture_output: bool = False):
    return hermetic.run_cargo_on_translated_code(
        args,
        cwd=cwd,
        check=True,
        capture_output=capture_output,
    )


def cached_git_clone_at_commit(repo_url: str, commit: str) -> Path:
    """
    Clone the given repo at the given commit, and cache in Python's temporary directory
    so that subsequent calls with the same repo and commit are fast, even across
    multiple test runs.
    """

    cache_dir = Path(tempfile.gettempdir()) / "tenjin_pytest_repo_cache"
    repo_cache_path = cache_dir / f"{repo_url.replace('/', '_')}_{commit}"
    if repo_cache_path.exists():
        return repo_cache_path

    try:
        hermetic.run(["git", "clone", repo_url, str(repo_cache_path)], check=True)
        hermetic.run(["git", "checkout", "--detach", commit], cwd=repo_cache_path, check=True)
    except Exception:
        # If something goes wrong, remove the cache directory to avoid leaving a broken clone around
        if repo_cache_path.exists():
            hermetic.run(["rm", "-rf", str(repo_cache_path)], check=True)
        raise
    return repo_cache_path
