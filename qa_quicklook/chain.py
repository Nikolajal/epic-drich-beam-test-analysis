"""Sequenced writer chain for the "Full reco" button.

The shift cockpit's *full reco* button runs ``lightdata_writer`` then
``recodata_writer`` on the same run.  We keep this in its own module
so the orchestration logic — second stage only runs if the first
exited 0, single started/finished pair surfaced to the GUI, Stop
aborts the in-flight stage and skips the rest — is testable on its
own without dragging the whole console widget into the test.

The chain owns a single :class:`qa_quicklook.runner.JobRunner` and
re-launches it on the runner's ``finished`` signal.  Each stage's
output streams through the runner's ``log_line`` / ``event`` / ``rows``
signals exactly as a single-stage launch would, so the console's
existing wiring just works.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Optional

try:
    from PySide6 import QtCore
except ImportError:  # pragma: no cover — venv pins PySide6
    QtCore = None  # type: ignore

from .runner import JobRunner


@dataclass(frozen=True)
class ChainStep:
    """One entry in a writer chain."""

    name: str            # short tag emitted as default_source for metric rows
    argv: list[str]


if QtCore is not None:

    class WriterChain(QtCore.QObject):
        """Run :class:`ChainStep` instances back-to-back on a shared runner.

        Signals:

          ``stage_started(int, str)``   index, name — fires as each stage launches.
          ``stage_finished(int, str, int)`` index, name, exit code.
          ``chain_finished(int)``       exit code of the final stage that ran.
                                        ``0`` only when every stage exited 0;
                                        otherwise the failing stage's code.

        The runner is shared so the console's existing connections
        (log/metrics/rows) keep working unchanged.
        """

        stage_started = QtCore.Signal(int, str)
        stage_finished = QtCore.Signal(int, str, int)
        chain_finished = QtCore.Signal(int)

        def __init__(
            self,
            runner: JobRunner,
            parent: QtCore.QObject | None = None,
        ) -> None:
            super().__init__(parent)
            self._runner = runner
            self._steps: list[ChainStep] = []
            self._index = -1
            self._stopped = False
            self._last_exit = 0
            self._runner.finished.connect(self._on_runner_finished)

        # --- public API ---------------------------------------------------

        def start(self, steps: list[ChainStep]) -> None:
            """Begin running ``steps`` in order.  Discards any pending tail."""
            if not steps:
                self.chain_finished.emit(0)
                return
            self._steps = list(steps)
            self._index = -1
            self._stopped = False
            self._last_exit = 0
            self._launch_next()

        def stop(self) -> None:
            """Abort the in-flight stage; skip any remaining stages.

            The ``finished`` signal from the runner will still fire once
            (with the killed exit code); the handler sees ``_stopped``
            and emits ``chain_finished`` without launching the next.
            """
            self._stopped = True
            self._runner.stop()

        def is_running(self) -> bool:
            return 0 <= self._index < len(self._steps) and self._runner.is_running()

        # --- internal -----------------------------------------------------

        def _launch_next(self) -> None:
            self._index += 1
            if self._index >= len(self._steps):
                self.chain_finished.emit(self._last_exit)
                return
            step = self._steps[self._index]
            self.stage_started.emit(self._index, step.name)
            self._runner.launch(step.argv, default_source=step.name)

        def _on_runner_finished(self, exit_code: int) -> None:
            if self._index < 0 or self._index >= len(self._steps):
                # Runner finished for something we didn't launch — ignore.
                return
            step = self._steps[self._index]
            self.stage_finished.emit(self._index, step.name, int(exit_code))
            self._last_exit = int(exit_code)
            if self._stopped or exit_code != 0:
                self.chain_finished.emit(self._last_exit)
                self._index = len(self._steps)  # stops _on_runner_finished re-entry
                return
            self._launch_next()

else:  # pragma: no cover — only triggered when PySide6 missing in tests

    WriterChain = None  # type: ignore


__all__ = ["ChainStep", "WriterChain"]
