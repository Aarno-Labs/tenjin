import json
from pathlib import Path

import pytest_html.extras


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
