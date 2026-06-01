"""Tests for ``qa_quicklook.remote_watcher.RemoteWatcherWorker``.

Stability-quorum state-machine coverage.  We don't spin up a real
QThread — instead we monkeypatch ``download.list_remote_runs`` to
return a scripted sequence of listings and drive ``_tick()`` manually.

The signals are captured via a tiny ``SignalSpy`` shim because Qt's
``QSignalSpy`` doesn't play nicely with synthetic test ticks.

Run with ``.venv/bin/python -m pytest tests/test_remote_watcher.py``.
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterator
from unittest.mock import MagicMock

import pytest

#  Skip the entire suite if PySide6 isn't installed (headless CI).
PySide6 = pytest.importorskip("PySide6")

from PySide6 import QtCore

from qa_quicklook import remote_watcher, download


# ---------------------------------------------------------------------------
# Helpers — a Qt application is needed for QObject construction even
# without a QThread.
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def qapp() -> Iterator[QtCore.QCoreApplication]:
    app = QtCore.QCoreApplication.instance() or QtCore.QCoreApplication([])
    yield app


def _fake_remote_runs(ids: list[str]) -> list[download.RemoteRun]:
    """Build a list of RemoteRun stubs for a given list of run ids."""
    return [
        download.RemoteRun(
            run_id=rid, mtime_epoch=0, mtime_iso="", local_present=False,
        )
        for rid in ids
    ]


class SignalSpy:
    """Trivial signal capture — accumulates each emission as a tuple."""

    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def __call__(self, *args) -> None:  # type: ignore[no-untyped-def]
        self.calls.append(args)


@pytest.fixture
def cfg(qapp: QtCore.QCoreApplication) -> download.RsyncConfig:
    return download.RsyncConfig(
        remote_host="fake@fake.local",
        remote_data_dir="/data/",
        local_data_dir="Data",
        extra_args="-av",
    )


@pytest.fixture
def worker(
    cfg: download.RsyncConfig, qapp: QtCore.QCoreApplication,
) -> remote_watcher.RemoteWatcherWorker:
    w = remote_watcher.RemoteWatcherWorker(cfg, Path("."), poll_interval_s=10)
    # Attach signal spies — make them attributes for convenience.
    w._spy_new_sealed = SignalSpy()
    w._spy_state = SignalSpy()
    w._spy_error = SignalSpy()
    w.new_sealed_run.connect(w._spy_new_sealed)
    w.state_text.connect(w._spy_state)
    w.error_occurred.connect(w._spy_error)
    return w


def _set_listing(monkeypatch: pytest.MonkeyPatch, ids: list[str]) -> None:
    """Patch ``list_remote_runs`` to return ``ids`` (newest-first)."""
    monkeypatch.setattr(
        remote_watcher.download, "list_remote_runs",
        lambda cfg, repo: (_fake_remote_runs(ids), ""),
    )


# ---------------------------------------------------------------------------
# Cold-start path — two ticks of the same top establish a baseline
# without firing.
# ---------------------------------------------------------------------------

class TestColdStart:
    def test_first_tick_sets_pending_no_emit(
        self, worker, monkeypatch,
    ) -> None:
        _set_listing(monkeypatch, ["20260101-100000"])
        worker._tick()
        assert worker._pending_top == "20260101-100000"
        assert worker._watermark_top is None
        assert worker._spy_new_sealed.calls == []

    def test_second_tick_same_top_promotes_to_watermark(
        self, worker, monkeypatch,
    ) -> None:
        _set_listing(monkeypatch, ["20260101-100000"])
        worker._tick()
        worker._tick()
        assert worker._watermark_top == "20260101-100000"
        assert worker._pending_top is None
        # No emission yet — baseline doesn't fire a sealed-run event.
        assert worker._spy_new_sealed.calls == []

    def test_baseline_flap_resets_pending(
        self, worker, monkeypatch,
    ) -> None:
        _set_listing(monkeypatch, ["A"])
        worker._tick()
        _set_listing(monkeypatch, ["B"])
        worker._tick()
        # Pending shifted to B; watermark still None.
        assert worker._pending_top == "B"
        assert worker._watermark_top is None
        assert worker._spy_new_sealed.calls == []


# ---------------------------------------------------------------------------
# Steady-state transition — the main feature.
# ---------------------------------------------------------------------------

class TestTransition:
    def _establish_baseline(self, worker, monkeypatch, top: str) -> None:
        _set_listing(monkeypatch, [top])
        worker._tick()
        worker._tick()
        assert worker._watermark_top == top

    def test_new_top_then_confirm_emits_sealed(
        self, worker, monkeypatch,
    ) -> None:
        self._establish_baseline(worker, monkeypatch, "A")
        # New top E appears.
        _set_listing(monkeypatch, ["E", "A"])
        worker._tick()
        # Stability quorum: still pending, no emit yet.
        assert worker._pending_top == "E"
        assert worker._spy_new_sealed.calls == []
        # Confirmation tick.
        worker._tick()
        # Now A is sealed.
        assert worker._spy_new_sealed.calls == [("A",)]
        assert worker._watermark_top == "E"
        assert worker._pending_top is None

    def test_flap_during_quorum_does_not_emit(
        self, worker, monkeypatch,
    ) -> None:
        """New top observed once, then reverts → no emission."""
        self._establish_baseline(worker, monkeypatch, "A")
        _set_listing(monkeypatch, ["E", "A"])
        worker._tick()  # pending = E
        _set_listing(monkeypatch, ["A"])
        worker._tick()  # top reverted; pending cleared.
        assert worker._pending_top is None
        assert worker._spy_new_sealed.calls == []

    def test_two_consecutive_new_tops_each_fire(
        self, worker, monkeypatch,
    ) -> None:
        """A → E (sealed A) → F (sealed E).  Each transition gets its quorum."""
        self._establish_baseline(worker, monkeypatch, "A")
        # First transition.
        _set_listing(monkeypatch, ["E", "A"])
        worker._tick()
        worker._tick()
        assert worker._spy_new_sealed.calls == [("A",)]
        # Second transition.
        _set_listing(monkeypatch, ["F", "E", "A"])
        worker._tick()
        worker._tick()
        assert worker._spy_new_sealed.calls == [("A",), ("E",)]

    def test_steady_top_no_emit(self, worker, monkeypatch) -> None:
        self._establish_baseline(worker, monkeypatch, "A")
        # Many ticks, no change.
        _set_listing(monkeypatch, ["A"])
        for _ in range(5):
            worker._tick()
        assert worker._spy_new_sealed.calls == []


# ---------------------------------------------------------------------------
# Error handling.
# ---------------------------------------------------------------------------

class TestErrors:
    def test_ssh_failure_emits_error(self, worker, monkeypatch) -> None:
        monkeypatch.setattr(
            remote_watcher.download, "list_remote_runs",
            lambda cfg, repo: ([], "ssh: Connection timed out"),
        )
        worker._tick()
        assert worker._spy_error.calls == [
            ("SSH listing failed: ssh: Connection timed out",),
        ]
        assert worker._spy_new_sealed.calls == []

    def test_exception_emits_error(self, worker, monkeypatch) -> None:
        def boom(*_):
            raise ConnectionError("network is down")
        monkeypatch.setattr(
            remote_watcher.download, "list_remote_runs", boom,
        )
        worker._tick()
        assert len(worker._spy_error.calls) == 1
        msg = worker._spy_error.calls[0][0]
        assert "ConnectionError" in msg
        assert "network is down" in msg


# ---------------------------------------------------------------------------
# Edge cases.
# ---------------------------------------------------------------------------

class TestEdge:
    def test_empty_remote_resets_state(self, worker, monkeypatch) -> None:
        _set_listing(monkeypatch, [])
        worker._tick()
        assert worker._watermark_top is None
        assert worker._pending_top is None
        assert worker._spy_new_sealed.calls == []

    def test_min_interval_clamp(self, worker) -> None:
        """Reconfiguring below the minimum clamps without warning."""
        worker.reconfigure_interval(1)  # below _MIN_POLL_INTERVAL_S = 5
        assert worker._poll_interval_s == 5
