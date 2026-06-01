"""Qt background worker for the Google Sheets push loop.

Lives in its own ``QThread`` so the synchronous Sheets API calls
(network-bound, hundreds of ms when the link is healthy, multiple
seconds on a flaky one) never block the dashboard's GUI thread.

The worker is a ``QObject`` driven by an internal ``QTimer`` running
inside the worker thread — Qt-idiomatic for periodic background
work.  Signals fire back to the GUI thread for status-bar updates;
no shared mutable state crosses thread boundaries.

The cadence is intentionally cheap: every tick does an mtime sweep
of the source files (years' database TOMLs, runlists TOML, audit
TOML, the joblock cache directory) and skips the actual push when
nothing has changed since the last successful one.  An explicit
``push_now`` slot exists for the "force a push" affordance the
status-bar widget exposes.

State machine the status bar should see (in ``state_text`` signal
values, all single-line and operator-friendly):

  - ``"disabled: <reason>"``           — config off or service-account
                                          missing
  - ``"idle (nothing changed since…)"``— between ticks, no edits
  - ``"…pushing…"``                    — push in flight
  - ``"pushed at <time> — N edits"``   — last successful push
  - ``"error: <one-line>"``            — last push failed; will retry
                                          on the next tick
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from PySide6 import QtCore

from . import sheets_sync


#  Bottom of the cadence range — refuse to spin faster than this
#  regardless of what the config says, to avoid an accidental
#  ``push_interval_s = 1`` from saturating the Sheets-API quota.
_MIN_INTERVAL_S = 5

#  Top of the backoff curve — never wait longer than this between
#  retries when consecutive errors pile up.  The 2026-05-29 design
#  call: operators want freshness when the link's healthy, but a
#  push-storm against a broken Sheet is worse than a stale view,
#  so we cap at 3 minutes (180 s).  Hit on the 4th-5th consecutive
#  error at the default 30 s base.
_MAX_BACKOFF_S = 180


class SheetsSyncWorker(QtCore.QObject):
    """Periodic push + reverse-merge driver.

    Construction is cheap; the actual loop only spins up once the
    parent calls ``start()`` (typically just after moving the worker
    onto a fresh ``QThread``).  The same instance survives across
    config reloads — ``reload_config`` re-reads ``qa_quicklook.toml``
    and resizes the timer in place.
    """

    #: ``state_text(text)`` — one-line status for the status-bar
    #: widget.  Emitted on every transition AND on every tick that
    #: produces a new outcome.
    state_text = QtCore.Signal(str)

    #: ``pushed(last_push_at, edits_applied, edits_skipped_in_sync)``
    #: — fires after a successful push so the status bar can stamp a
    #: timestamp + diagnostic counts.  Always immediately followed by
    #: a ``state_text`` emission summarising the same outcome.
    pushed = QtCore.Signal(str, int, int)

    #: ``error(message)`` — last push failed.  The worker will retry
    #: on the next tick (errors don't latch); operators see the
    #: failure in the status bar until the next success.
    error = QtCore.Signal(str)

    def __init__(
        self,
        cfg: sheets_sync.SheetsConfig,
        repo_root: Path,
        dashboard_config: Path,
        parent: Optional[QtCore.QObject] = None,
    ) -> None:
        super().__init__(parent)
        self._cfg = cfg
        self._repo_root = repo_root
        self._dashboard_config = dashboard_config
        self._timer = QtCore.QTimer(self)
        self._timer.setSingleShot(False)
        self._timer.timeout.connect(self._tick)
        #  Watermark of "the source files looked like this on the last
        #  successful push".  An identical sweep means there's nothing
        #  to do — skip the network round-trip entirely.  Re-built
        #  post-push so reverse-merged edits don't trigger an infinite
        #  loop on the next tick.
        self._last_pushed_mtimes: dict[str, int] = {}
        #  Exponential-backoff state.  ``_skip_remaining`` is the
        #  number of upcoming ``_tick`` fires we should no-op through;
        #  ``_consecutive_errors`` counts straight-line failures so
        #  the backoff curve resets cleanly on the first success.
        self._skip_remaining: int = 0
        self._consecutive_errors: int = 0
        #  Sticky reverse-merge brake — once an operator manually
        #  acknowledges and forces, we let the next single tick apply.
        #  Otherwise a tripped brake keeps tripping until a Sheet
        #  reset or local snapshot wipe.
        self._allow_next_force: bool = False

    # ----- slots invoked from the GUI thread ----------------------------

    @QtCore.Slot()
    def start(self) -> None:
        """Begin ticking.  Safe to call multiple times — restarts."""
        if not self._cfg.enabled:
            #  Stay silent — the parent shouldn't have spun us up at
            #  all in this case, but defensive in case of races
            #  between config reload + thread startup.
            self.state_text.emit(f"disabled: {self._cfg.disabled_reason()}")
            return
        ms = max(_MIN_INTERVAL_S, self._cfg.push_interval_s) * 1000
        self._timer.start(ms)
        self.state_text.emit("starting — first push imminent")
        #  Fire the first tick immediately so the operator sees the
        #  Sheet populate without waiting a full interval.  Queued
        #  invocation so this slot returns first.
        QtCore.QMetaObject.invokeMethod(
            self, "_tick", QtCore.Qt.QueuedConnection,
        )

    @QtCore.Slot()
    def stop(self) -> None:
        """Pause the loop; the timer can be restarted via ``start``."""
        self._timer.stop()
        self.state_text.emit("stopped")

    @QtCore.Slot()
    def push_now(self) -> None:
        """Force a push on the next event-loop tick, regardless of mtimes.

        Also clears the exponential-backoff skip counter so a stuck
        worker can be kicked back into action without waiting for the
        full backoff window to elapse.  Does NOT bypass the reverse-
        merge brake — that needs the explicit ``push_now_with_force``
        slot below.
        """
        self._skip_remaining = 0
        QtCore.QMetaObject.invokeMethod(
            self, "_tick_forced", QtCore.Qt.QueuedConnection,
        )

    @QtCore.Slot()
    def push_now_with_force(self) -> None:
        """Push immediately AND bypass the reverse-merge brake once.

        Operator-only escape hatch: the brake exists to prevent the
        2026-05-29 incident (snapshot wipe → 260 spurious edits).  An
        operator who's certain a bulk edit on the Sheet IS intended
        invokes this — one tick, then the brake re-arms.
        """
        self._skip_remaining = 0
        self._allow_next_force = True
        QtCore.QMetaObject.invokeMethod(
            self, "_tick_forced", QtCore.Qt.QueuedConnection,
        )

    @QtCore.Slot()
    def reload_config(self) -> None:
        """Re-read ``[sheets_sync]`` and resize the timer in place.

        Called from the GUI thread (queued) when the dashboard config
        file changes — the same hook ``MainWindow._on_dashboard_config_changed``
        already uses for theme + tab toggles.
        """
        was_active = self._timer.isActive()
        new_cfg = sheets_sync.load_config(self._dashboard_config)
        self._cfg = new_cfg
        if not new_cfg.enabled:
            self._timer.stop()
            self.state_text.emit(f"disabled: {new_cfg.disabled_reason()}")
            return
        ms = max(_MIN_INTERVAL_S, new_cfg.push_interval_s) * 1000
        self._timer.start(ms)
        if not was_active:
            self.state_text.emit("re-enabled — first push imminent")
            QtCore.QMetaObject.invokeMethod(
                self, "_tick", QtCore.Qt.QueuedConnection,
            )

    # ----- internals (run in the worker thread) -------------------------

    @QtCore.Slot()
    def _tick(self) -> None:
        self._do_tick(force=False)

    @QtCore.Slot()
    def _tick_forced(self) -> None:
        self._do_tick(force=True)

    def _do_tick(self, *, force: bool) -> None:
        if not self._cfg.is_configured:
            self.state_text.emit(f"disabled: {self._cfg.disabled_reason()}")
            return

        #  Exponential-backoff gate: previous error armed N skip-
        #  ticks; honour them unless the caller forced a push.
        if not force and self._skip_remaining > 0:
            self._skip_remaining -= 1
            return

        mtimes = self._scan_mtimes()
        if (not force) and mtimes == self._last_pushed_mtimes and self._last_pushed_mtimes:
            #  Nothing changed since the last successful push — skip.
            #  The "and self._last_pushed_mtimes" guard ensures the
            #  very first tick always runs (empty dict != current scan).
            return

        self.state_text.emit("…pushing…")
        try:
            #  ``_allow_next_force`` is the operator's explicit "yes
            #  the bulk reverse-merge is intended" — consumed on use
            #  so a single force doesn't latch.
            use_force = self._allow_next_force
            self._allow_next_force = False
            result = sheets_sync.sync_once(
                self._cfg, self._repo_root, dry_run=False,
                force_reverse_merge=use_force,
            )
        except sheets_sync.ReverseMergeBrake as exc:
            #  The brake is a special non-error: the push didn't
            #  fail, we refused to apply the suspicious bulk edit.
            #  Don't back off (the local data is fine), but surface
            #  a clear status so the operator knows to investigate.
            self.error.emit(str(exc))
            self.state_text.emit(
                f"reverse-merge brake tripped: {exc.edit_count} edits "
                f">{exc.threshold} — investigate, then push_now"
            )
            return
        except Exception as exc:  # noqa: BLE001
            #  Real failure: arm exponential backoff.  Errors aren't
            #  latching — the next tick (after the backoff window)
            #  retries.  Surface one clean line; the full type prefix
            #  beats a raw str() when the cause is e.g. PermissionDeniedError.
            self._consecutive_errors += 1
            base = max(_MIN_INTERVAL_S, self._cfg.push_interval_s)
            target_s = min(
                _MAX_BACKOFF_S,
                base * (2 ** (self._consecutive_errors - 1)),
            )
            #  Skip count = how many additional regular ticks to
            #  no-op past.  The timer keeps firing at the base
            #  interval; this counter says "ignore the next K of them".
            self._skip_remaining = max(0, (target_s // base) - 1)
            msg = f"{type(exc).__name__}: {exc}"
            self.error.emit(msg)
            self.state_text.emit(
                f"error (retry in ~{target_s}s — "
                f"{self._consecutive_errors} consecutive): "
                f"{msg.splitlines()[0]}"
            )
            return

        #  Re-scan AFTER the push so the watermark reflects any
        #  reverse-merge edits the sync just applied — otherwise the
        #  next tick would see the post-merge file mtime as "new"
        #  and loop forever.
        self._last_pushed_mtimes = self._scan_mtimes()
        #  Reset the backoff curve on the first success.
        self._consecutive_errors = 0
        self._skip_remaining = 0
        self.pushed.emit(
            result.last_push_at,
            result.edits_applied,
            result.edits_skipped_in_sync,
        )
        if result.hard_reset:
            #  Sheet was mangled — surface loudly via the error
            #  channel so operators see it in the status bar without
            #  the success animation distracting from the warning.
            issues_one_line = " | ".join(result.integrity_issues[:3])
            extra = f" (+{len(result.integrity_issues) - 3} more)" \
                if len(result.integrity_issues) > 3 else ""
            self.error.emit(
                f"Sheet integrity check failed — hard reset issued. "
                f"{issues_one_line}{extra}"
            )
            self.state_text.emit(
                f"HARD RESET at {result.last_push_at} — Sheet mangled, "
                f"canonical state rewritten over"
            )
            return
        suffix = ""
        if result.edits_applied:
            suffix = f" · merged {result.edits_applied} Sheet edit(s)"
        self.state_text.emit(
            f"pushed at {result.last_push_at}{suffix}"
        )

    def _scan_mtimes(self) -> dict[str, int]:
        """Cheap "what's changed" fingerprint.

        We hash on ``(path, mtime_ns)`` for each watched file + the
        joblock directory's own mtime (which bumps when locks land
        or get cleared — far cheaper than a per-file scan).  A
        missing file maps to ``0`` so a freshly-created file still
        shows up as a change on the next tick.
        """
        out: dict[str, int] = {}
        rl = self._repo_root / "run-lists"
        if rl.is_dir():
            for p in rl.iterdir():
                if p.is_file() and p.suffix == ".toml":
                    try:
                        out[str(p)] = p.stat().st_mtime_ns
                    except OSError:
                        out[str(p)] = 0
        try:
            #  joblock dir's mtime bumps on file create/delete inside
            #  it — bumps cheaply on every writer start/stop without
            #  needing to list every file.
            from . import joblock
            d = joblock.CACHE_DIR
            if d.is_dir():
                out[str(d)] = d.stat().st_mtime_ns
        except OSError:
            pass
        return out


__all__ = ["SheetsSyncWorker"]
