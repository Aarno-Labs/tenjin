import pytest

pytest_plugins = "10j_pytest_fixtures"

_INTERRUPTED_EXIT_CODE = int(pytest.ExitCode.INTERRUPTED)

# Slow test management


def pytest_addoption(parser):
    parser.addoption(
        "--also-slow", action="store_true", default=False, help="Run slow tests as well"
    )


def pytest_collection_modifyitems(config, items):
    if not config.getoption("--also-slow"):
        skip_slow = pytest.mark.skip(reason="Need --also-slow option to run")
        for item in items:
            if item.get_closest_marker("slow"):
                item.add_marker(skip_slow)


@pytest.hookimpl(hookwrapper=True)
def pytest_sessionfinish(session, exitstatus):
    outcome = yield
    if outcome.excinfo and issubclass(outcome.excinfo[0], KeyboardInterrupt):
        # A user Ctrl-C can land again while xdist tears down workers, which otherwise
        # turns an already-interrupted run into a second raw traceback during shutdown.
        session.exitstatus = _INTERRUPTED_EXIT_CODE
        outcome.force_result(None)


# Pytest HTML report customization


def pytest_html_results_table_header(cells):
    cells.insert(2, "<th>Summary</th>")


def pytest_html_results_table_row(report, cells):
    cells.insert(2, f"<td>{report.summary_html}</td>")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    report.summary_html = getattr(item, "summary_html", "")
