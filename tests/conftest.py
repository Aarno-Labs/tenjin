import pytest

pytest_plugins = "10j_pytest_fixtures"


def pytest_html_results_table_header(cells):
    cells.insert(2, "<th>Summary</th>")


def pytest_html_results_table_row(report, cells):
    cells.insert(2, f"<td>{report.summary_html}</td>")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    report.summary_html = getattr(item, "summary_html", "")
