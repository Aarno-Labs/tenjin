import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

from subprocess import CompletedProcess

import ingest


@dataclass
class Interval:
    start_ns: int
    end_ns: int

    def duration_ns(self) -> int:
        """Duration in nanoseconds"""
        return self.end_ns - self.start_ns

    def duration_ms_int(self) -> int:
        """Duration in milliseconds (rounded down)"""
        return self.duration_ns() // 1_000_000

    def duration_s(self) -> float:
        """Duration in seconds"""
        return self.duration_ns() / 1_000_000_000


class TimingRepo:
    def __init__(self, translation_record: ingest.TranslationRecord | None):
        self._translation_record = translation_record
        self._current_step: ingest.TransformationRecord | None = None
        self._results: list[ingest.TransformationRecord] = []
        self._start_time_ns = time.monotonic_ns()

    @contextmanager
    def tracking(self, step_name: str, results_path: Path):
        """Context manager to track timing for a named step"""
        start_time = time.monotonic_ns()
        self._current_step = ingest.TransformationRecord(
            name=step_name,
            results_path=str(results_path),
            start_unix_timestamp=int(time.time()),
            elapsed_ms=0,  # Will be set after the context manager exits
            exit_code=0,
            stderr_lines=None,
            stdout_lines=None,
        )

        try:
            yield self
        finally:
            end_time = time.monotonic_ns()
            interval = Interval(start_time, end_time)
            self._current_step.elapsed_ms = interval.duration_ms_int()
            self._results.append(self._current_step)
            self._current_step = None

    def update_sub(self, cp: CompletedProcess):
        """Update the current step with a subprocess result"""
        if self._current_step is None:
            raise RuntimeError("No current step to update")
        self._current_step.exit_code = cp.returncode
        self._current_step.stderr_lines = (
            cp.stderr.decode("utf-8").splitlines() if cp.stderr is not None else None
        )
        self._current_step.stdout_lines = (
            cp.stdout.decode("utf-8").splitlines() if cp.stdout is not None else None
        )

    def set_exit_code(self, exit_code: int):
        """Set the exit code for the current step"""
        if self._current_step is None:
            raise RuntimeError("No current step to set exit code for")
        self._current_step.exit_code = exit_code

    def finalize(self) -> ingest.TranslationRecord | None:
        """Get the list of recorded transformation records"""
        if self._current_step is not None:
            raise RuntimeError("Current step is not finalized")

        if self._translation_record is None:
            return None

        self._translation_record.results.transformations = list(self._results)
        self._translation_record.results.translation_elapsed_ms = Interval(
            self._start_time_ns, time.monotonic_ns()
        ).duration_ms_int()
        return self._translation_record
