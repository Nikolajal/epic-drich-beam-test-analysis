"""Background writes for rundb to keep the GUI responsive.

Why this exists
---------------
The dashboard's TOML writes go through ``tomlkit.dumps`` for
comment preservation — which on a ~260-entry production database
takes ~1.7 s.  Running that on the GUI thread freezes the dashboard
for the duration of every quality-tag edit, every quick-quality
change in the Run Manager, every append-runs, every delete-runs.

This worker fixes that by:

  1. Running each write on a dedicated background thread (the GUI
     thread keeps repainting).
  2. **FIFO-serialising** all writes through a single queue so two
     rapid edits never race the read-modify-write cycle on disk.
     (Two writes from the GUI in quick succession naturally enqueue
     in order; the second only starts once the first has flushed.)
  3. Emitting a Qt signal back when each write completes so the
     calling view can reload + repaint.

The signal carries a free-form ``tag`` so consumers can correlate
which submission finished — useful when several views are sharing
the worker (the Run Manager's quick-quality dropdown + the Database
tab's runcard both write through here).
"""

from __future__ import annotations

import queue
import threading
import traceback
from typing import Callable, Optional

from PySide6 import QtCore


class DbWorker(QtCore.QObject):
    """Single-thread FIFO dispatcher for blocking ``rundb`` writes.

    Lives for the dashboard's lifetime.  Construct once and share
    via a parent (typically the ``MainWindow``).
    """

    #: ``done(tag, ok, error_text)`` — emitted on the GUI thread once
    #: a submitted callable finishes.  ``ok`` is False iff an exception
    #: was raised; ``error_text`` is a human-readable trace then.
    done = QtCore.Signal(str, bool, str)

    def __init__(self, parent: Optional[QtCore.QObject] = None) -> None:
        super().__init__(parent)
        self._queue: queue.Queue[tuple[str, Callable[[], None]]] = queue.Queue()
        self._thread = threading.Thread(
            target=self._loop, name="qaq-dbworker", daemon=True,
        )
        self._thread.start()

    def submit(self, tag: str, fn: Callable[[], None]) -> None:
        """Enqueue ``fn`` for serial background execution.

        Returns immediately.  ``done(tag, ok, err)`` will fire on the
        GUI thread once ``fn`` returns or raises.
        """
        self._queue.put((tag, fn))

    def pending(self) -> int:
        """Approximate count of queued + in-flight writes (best-effort)."""
        return self._queue.qsize()

    # ----- internals -----------------------------------------------------

    def _loop(self) -> None:
        while True:
            tag, fn = self._queue.get()
            try:
                fn()
                self.done.emit(tag, True, "")
            except Exception as exc:  # noqa: BLE001
                err = f"{type(exc).__name__}: {exc}\n{traceback.format_exc()}"
                self.done.emit(tag, False, err)
            finally:
                self._queue.task_done()


__all__ = ["DbWorker"]
