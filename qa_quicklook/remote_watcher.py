"""qa_quicklook/remote_watcher.py — background SSH listing for auto-QA.

Off by default.  When ``[livemon] enabled = true`` the dashboard spins
up an instance on a dedicated QThread and feeds it the rsync config.
The worker polls the DAQ host's remote run directory every
``poll_interval_s`` seconds and emits ``new_sealed_run(run_id)`` when a
strictly-new run id transitions in at the top of the listing.

The "sealed by construction" reasoning:

    Tick 0: remote listing is [A, B, C, D], newest first.  Watermark
            is A — the run currently being acquired.
    Tick N: listing is [E, A, B, C, D].  A NEW run id E has appeared
            at the top.  The DAQ created a new run directory, which
            it only does after the previous run's files have been
            fully flushed.  Therefore A is now sealed.

The worker reports A via ``new_sealed_run("A")`` after a stability
quorum (E must remain at the top for one additional tick) to guard
against DAQ flaps — a run that gets created and immediately abandoned
shouldn't fire the auto-QA.

The worker NEVER emits the very latest id — that one might still be
acquiring.  Manual download covers the case where the shifter knows
the latest is sealed (the picker's live-run-guard checkbox confirms).

Error handling
--------------

Per operator decision (2026-05-29): any SSH listing failure emits
``error_occurred(message)``.  The MainWindow slot connected to that
signal must:

    1. Show the operator a modal error dialog with the message.
    2. Stop the worker (call ``stop()`` then quit the thread).

No silent retries — the shifter takes action explicitly so they
notice if the DAQ host is unreachable.

Template
--------

Mirrors ``sheets_worker.py``'s QObject-on-QThread pattern exactly:
construct on the main thread, ``moveToThread(thread)``, connect
``thread.started`` to ``worker.start``, listen for signals back on
the GUI thread.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from PySide6 import QtCore

from . import download

#: Minimum poll interval enforced regardless of TOML setting — protects
#: the DAQ host from accidental ssh-handshake flood from a typo'd 1 s.
_MIN_POLL_INTERVAL_S: int = 5


class RemoteWatcherWorker(QtCore.QObject):
    """Background watcher emitting ``new_sealed_run`` on DAQ-side transitions.

    Construction is cheap; the loop only starts once the parent calls
    ``start()`` (typically right after moving the worker onto a fresh
    ``QThread``).  Survives across config reloads — ``reconfigure()``
    swaps in new settings without rebuilding the thread.
    """

    #: ``new_sealed_run(run_id)`` — fires when a strictly-new id transitions
    #: in at the top of the remote listing, after the stability quorum.
    #: ``run_id`` is the PREVIOUS top (now sealed), NOT the new top.
    #: The new top is whatever's currently being acquired and we never
    #: emit it.
    new_sealed_run = QtCore.Signal(str)

    #: ``state_text(text)`` — human one-liner for the status bar.  Fires
    #: on every state change and after every successful tick.
    state_text = QtCore.Signal(str)

    #: ``error_occurred(message)`` — SSH listing failed.  Per operator
    #: decision: connected slot must stop the worker and show a modal
    #: dialog to the shifter.
    error_occurred = QtCore.Signal(str)

    def __init__(
        self,
        cfg: download.RsyncConfig,
        repo_root: Path,
        poll_interval_s: int,
        parent: Optional[QtCore.QObject] = None,
    ) -> None:
        super().__init__(parent)
        self._cfg = cfg
        self._repo_root = repo_root
        self._poll_interval_s = max(_MIN_POLL_INTERVAL_S, int(poll_interval_s))
        self._timer = QtCore.QTimer(self)
        self._timer.setSingleShot(False)
        self._timer.timeout.connect(self._tick)
        #  Stability-quorum state.  See module docstring for the
        #  transition table.  ``watermark_top`` is the stable newest id
        #  we've confirmed.  ``pending_top`` is a candidate awaiting
        #  one more tick of confirmation.
        self._watermark_top: Optional[str] = None
        self._pending_top: Optional[str] = None

    # ----- slots invoked from the GUI thread ----------------------------

    @QtCore.Slot()
    def start(self) -> None:
        """Begin polling.  Safe to call multiple times — restarts."""
        ms = self._poll_interval_s * 1000
        self._timer.start(ms)
        self.state_text.emit(
            f"live monitor armed — polling every {self._poll_interval_s} s"
        )
        #  Fire the first tick immediately so the operator sees the
        #  watermark populate without waiting a full interval.  Queued
        #  invocation so ``start()`` returns first.
        QtCore.QMetaObject.invokeMethod(
            self, "_tick", QtCore.Qt.QueuedConnection,
        )

    @QtCore.Slot()
    def stop(self) -> None:
        """Stop polling.  Idempotent."""
        if self._timer.isActive():
            self._timer.stop()
            self.state_text.emit("live monitor stopped")

    @QtCore.Slot(int)
    def reconfigure_interval(self, poll_interval_s: int) -> None:
        """Apply a new poll interval without rebuilding the worker.

        Resets the timer in place; the next tick fires after the new
        interval, not at the old phase.
        """
        new_interval = max(_MIN_POLL_INTERVAL_S, int(poll_interval_s))
        if new_interval == self._poll_interval_s:
            return
        self._poll_interval_s = new_interval
        if self._timer.isActive():
            self._timer.stop()
            self._timer.start(new_interval * 1000)
            self.state_text.emit(
                f"live monitor reconfigured — polling every "
                f"{new_interval} s"
            )

    # ----- internal -----------------------------------------------------

    @QtCore.Slot()
    def _tick(self) -> None:
        """One poll cycle: fetch the listing, diff against the state machine."""
        try:
            entries, err = download.list_remote_runs(self._cfg, self._repo_root)
        except Exception as exc:  # noqa: BLE001 — surface any failure
            self.error_occurred.emit(
                f"SSH listing raised: {type(exc).__name__}: {exc}"
            )
            return
        if err:
            self.error_occurred.emit(f"SSH listing failed: {err}")
            return

        current_top = download.newest_run_id(entries)
        if current_top is None:
            #  Empty remote — nothing to do, but it's not an error
            #  (could be a fresh DAQ install).  Reset state so any
            #  later non-empty tick treats itself as a startup.
            self._watermark_top = None
            self._pending_top = None
            self.state_text.emit("live monitor: remote is empty")
            return

        if self._watermark_top is None and self._pending_top is None:
            #  Cold start — capture the baseline without firing.  We
            #  don't know what (if anything) was sealed before we
            #  began watching, so the first observation just becomes
            #  the pending candidate.
            self._pending_top = current_top
            self.state_text.emit(
                f"live monitor: baseline observed (top={current_top})"
            )
            return

        if self._watermark_top is None and self._pending_top is not None:
            #  Second tick of cold start.  Confirm the baseline if it
            #  matches; otherwise reset and try again next tick.
            if current_top == self._pending_top:
                self._watermark_top = self._pending_top
                self._pending_top = None
                self.state_text.emit(
                    f"live monitor: baseline confirmed (top={current_top})"
                )
            else:
                self._pending_top = current_top
                self.state_text.emit(
                    f"live monitor: baseline shifted to {current_top}"
                )
            return

        #  Steady state — watermark is set.
        if current_top == self._watermark_top:
            #  No transition.  If a pending candidate was hanging from
            #  a transient flap, drop it.
            if self._pending_top is not None:
                self._pending_top = None
                self.state_text.emit(
                    f"live monitor: pending candidate cleared "
                    f"(top reverted to {current_top})"
                )
            return

        if self._pending_top is None or current_top != self._pending_top:
            #  Brand-new top observation OR a different new top.  Start
            #  (or restart) the stability quorum.
            self._pending_top = current_top
            self.state_text.emit(
                f"live monitor: new top observed ({current_top}) "
                f"— awaiting confirmation"
            )
            return

        #  current_top == self._pending_top  → confirmation tick.
        #  The previous watermark is now sealed by construction (DAQ
        #  moved on).  Emit and rotate.
        sealed = self._watermark_top
        self._watermark_top = self._pending_top
        self._pending_top = None
        self.state_text.emit(
            f"live monitor: sealed {sealed} — top is now {current_top}"
        )
        self.new_sealed_run.emit(sealed)


__all__ = ["RemoteWatcherWorker"]
