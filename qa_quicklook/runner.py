"""QProcess wrapper for spawning writer subprocesses.

The Run Manager spawns writers (lightdata_writer, recodata_writer,
…) and streams their stdout/stderr into a live log dock.  This
module owns the QProcess and exposes a few small Qt signals the UI
listens to.

Why not Python's ``subprocess``: ``QProcess`` integrates with Qt's
event loop, so we can read output line-by-line without blocking the
GUI thread or needing a worker thread.  Same idiom the original
console-first dashboard used (the module was destroyed in the
reset; this is the trimmed re-implementation for Run Manager v1 —
no ``[QA] {json}`` event parsing yet, just a clean log stream).
"""

from __future__ import annotations

import os
import shlex
import signal
import subprocess
import time
from pathlib import Path
from typing import Iterable, Optional

from . import joblock

try:
    from PySide6 import QtCore
except ImportError:  # pragma: no cover — venv pins PySide6
    QtCore = None  # type: ignore


LOG_CACHE = Path.home() / ".cache" / "qa_quicklook" / "logs"


def writer_log_path(writer: str, run: str) -> Path:
    LOG_CACHE.mkdir(parents=True, exist_ok=True)
    safe_writer = "".join(c for c in writer if c.isalnum() or c in "-_")
    safe_run = "".join(c for c in run if c.isalnum() or c in "-_")
    return LOG_CACHE / f"{safe_writer}_{safe_run}.log"


def _line_buffered_wrap(argv: list[str]) -> list[str]:
    """Prefix ``stdbuf -oL -eL`` so the writer's stdout flushes per line.

    Without this, C++ writers' stdout is fully buffered when
    redirected (not a TTY) and our log tail sees nothing until the
    buffer fills (or the process exits).  ``stdbuf`` is GNU coreutils
    on Linux and ``brew install coreutils`` (or ``gstdbuf``) on
    macOS.  If neither is on PATH we fall back to the unprefixed
    argv — the dashboard still works, the progress bar just doesn't
    render live.
    """
    import shutil as _sh
    for candidate in ("stdbuf", "gstdbuf"):
        tool = _sh.which(candidate)
        if tool:
            return [tool, "-oL", "-eL", *argv]
    return list(argv)


def shell_quote_argv(argv: Iterable[str]) -> str:
    """Cosmetic helper used by the confirm dialog and the log header."""
    return " ".join(shlex.quote(part) for part in argv)


if QtCore is not None:

    class JobRunner(QtCore.QObject):
        # NOTE: this implementation spawns the writer **detached** —
        # ``subprocess.Popen(..., start_new_session=True)`` puts the
        # child in its own process group so killing the dashboard does
        # NOT take the writer with it.  Stdout/stderr are tee'd into a
        # log file under ``~/.cache/qa_quicklook/logs/`` which the
        # dashboard tails via a 200 ms ``QTimer``.  A second 500 ms
        # timer polls ``Popen.poll()`` (or PID liveness when attached
        # to an external process) to spot exit.
        #
        # The previous QProcess-as-child implementation died on
        # dashboard close, which also leaked a writer the lock file
        # said was "running" forever.  This new shape is harder but
        # actually survives.
        """Wrap a ``QProcess``, emit text events.

        Signals
        -------
        ``log_line(str)``       every ``\\n``-terminated line of the
                                merged stdout/stderr stream.
        ``log_overwrite(str)``  every ``\\r``-terminated chunk — used
                                for in-place progress bars (the log
                                widget overwrites its last visible
                                line instead of appending).
        ``started(str)``        shell-quoted argv just as the process
                                spawns.
        ``finished(int)``       exit code, fired exactly once per
                                launch.

        Only one process at a time.  ``launch()`` while a job is
        running first stops the existing one.  ``stop()`` sends
        SIGTERM, then SIGKILL after ``kill_grace_ms`` if still alive.
        """

        log_line = QtCore.Signal(str)
        log_overwrite = QtCore.Signal(str)
        started = QtCore.Signal(str)
        finished = QtCore.Signal(int)

        kill_grace_ms = 3000

        def __init__(self, parent: QtCore.QObject | None = None) -> None:
            super().__init__(parent)
            self._popen: subprocess.Popen | None = None
            self._external_pid: Optional[int] = None  # when attaching to a survivor
            self._buffer = b""
            self._lock_key: Optional[tuple[str, str]] = None
            self._log_path: Optional[Path] = None
            self._log_offset = 0
            self._stop_requested = False
            self._tail_timer = QtCore.QTimer(self)
            self._tail_timer.setInterval(200)
            self._tail_timer.timeout.connect(self._tail_log)
            self._poll_timer = QtCore.QTimer(self)
            self._poll_timer.setInterval(500)
            self._poll_timer.timeout.connect(self._check_alive)

        # ---- public API --------------------------------------------------

        def launch(
            self,
            argv: list[str],
            default_source: str = "",
            *,
            writer: str = "",
            run: str = "",
        ) -> None:
            """Spawn ``argv`` **detached**.

            The child process runs in its own process group via
            ``start_new_session=True`` so it survives the dashboard
            being closed.  Stdout/stderr go to a log file under
            ``~/.cache/qa_quicklook/logs/<writer>_<run>.log`` which
            this object tails for the live log dock; another
            dashboard instance can attach to the same file later.
            """
            if self.is_running():
                self.stop(wait=True)
            self._buffer = b""
            self._log_offset = 0
            self._stop_requested = False
            self._external_pid = None
            self._lock_key = (writer.strip(), run.strip()) if (writer and run) else None
            self._log_path = (
                writer_log_path(self._lock_key[0], self._lock_key[1])
                if self._lock_key is not None
                else None
            )

            # Open the log file fresh — each launch starts a new log.
            # The child inherits this fd; we close it immediately
            # afterwards (POSIX dup means the child keeps writing).
            if self._log_path is not None:
                log_fd = open(self._log_path, "wb")
            else:
                log_fd = subprocess.DEVNULL

            env = os.environ.copy()
            env.setdefault("PYTHONUNBUFFERED", "1")
            # Force line-buffered stdout when redirecting to a file:
            # C++ writers' stdout is *fully* buffered by libc when
            # not a TTY, so progress bars (\r-overwrite) never flush
            # until the buffer fills.  Prefix ``stdbuf -oL -eL`` if
            # available so the tail timer actually sees each write.
            spawn_argv = _line_buffered_wrap(argv)
            try:
                self._popen = subprocess.Popen(
                    spawn_argv,
                    stdout=log_fd,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,  # detach from dashboard's group
                    env=env,
                    bufsize=0,
                )
            finally:
                if isinstance(log_fd, int):
                    pass
                else:
                    try:
                        log_fd.close()
                    except Exception:
                        pass

            # Drop the lock file with the new PID *after* spawn so
            # we know the real PID.
            if self._lock_key is not None:
                joblock.write_lock(joblock.JobLock(
                    writer=self._lock_key[0],
                    run=self._lock_key[1],
                    pid=int(self._popen.pid),
                    argv=list(argv),
                    started_at=joblock.now_iso(),
                    state=joblock.EFFECTIVE_RUNNING,
                ))

            self._tail_timer.start()
            self._poll_timer.start()
            self.started.emit(shell_quote_argv(argv))

        def attach_external(self, writer: str, run: str, pid: int) -> None:
            """Connect to a writer started by a *previous* dashboard run.

            Used when ``joblock`` reports a writer is still running
            and we want the new dashboard to tail the same log and
            wait for it to exit.  We don't own the process — Stop
            still signals it, but the exit is reaped by ``waitpid`` in
            another shell (the original parent died), so we detect
            exit via PID liveness.
            """
            self._lock_key = (writer, run)
            self._log_path = writer_log_path(writer, run)
            self._log_offset = 0
            self._popen = None
            self._external_pid = int(pid)
            self._stop_requested = False
            self._tail_timer.start()
            self._poll_timer.start()
            self.started.emit(f"(attached to PID {pid})")

        def stop(self, wait: bool = False) -> None:
            """SIGTERM the writer; SIGKILL after the grace if still alive."""
            pid = self._current_pid()
            if pid is None:
                return
            self._stop_requested = True
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                return
            # Don't busy-wait here — the poll timer will spot the exit
            # and fire ``finished`` cleanly.  ``wait`` is best-effort.
            if wait:
                deadline = time.monotonic() + self.kill_grace_ms / 1000.0
                while time.monotonic() < deadline and joblock.pid_alive(pid):
                    time.sleep(0.05)
                if joblock.pid_alive(pid):
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

        def is_running(self) -> bool:
            pid = self._current_pid()
            return pid is not None and joblock.pid_alive(pid)

        def _current_pid(self) -> Optional[int]:
            if self._popen is not None and self._popen.poll() is None:
                return self._popen.pid
            if self._external_pid is not None and joblock.pid_alive(self._external_pid):
                return self._external_pid
            return None

        # ---- internals ---------------------------------------------------

        def _tail_log(self) -> None:
            if self._log_path is None or not self._log_path.exists():
                return
            try:
                with open(self._log_path, "rb") as f:
                    f.seek(self._log_offset)
                    chunk = f.read()
            except OSError:
                return
            if not chunk:
                return
            self._log_offset += len(chunk)
            self._buffer += chunk
            # Split on whichever terminator comes first: ``\n`` is a
            # real new line (append to log); ``\r`` is a progress
            # update (overwrite the last visible line in the log).
            while True:
                nl = self._buffer.find(b"\n")
                cr = self._buffer.find(b"\r")
                # No terminator yet — leave the partial line in the
                # buffer for the next chunk.
                if nl < 0 and cr < 0:
                    break
                # Pick the earlier of the two.
                if nl < 0:
                    cut, kind = cr, "cr"
                elif cr < 0:
                    cut, kind = nl, "nl"
                else:
                    cut, kind = (cr, "cr") if cr < nl else (nl, "nl")
                raw = self._buffer[:cut]
                self._buffer = self._buffer[cut + 1:]
                text = raw.decode("utf-8", errors="replace")
                # ``\r\n`` pairs: the ``\r`` lands first, emits an
                # empty overwrite, then the ``\n`` lands.  Skip empty
                # overwrites to avoid a flicker.
                if kind == "cr":
                    if text:
                        self.log_overwrite.emit(text)
                else:
                    self.log_line.emit(text)

        def _check_alive(self) -> None:
            """500 ms timer — spots the writer exiting.

            For Popen-owned jobs we ``poll()`` and pick up the exit
            code directly.  For attached external PIDs we only know
            "alive" or "gone" — exit code defaults to -1 and the
            state stays whatever the writer left in the lock (this
            is by design: another shell already reaped the child).
            """
            if self._popen is not None:
                rc = self._popen.poll()
                if rc is None:
                    return
                self._on_exit(int(rc))
                return
            if self._external_pid is not None:
                if not joblock.pid_alive(self._external_pid):
                    self._on_exit(-1)

        def _on_exit(self, exit_code: int) -> None:
            self._tail_timer.stop()
            self._poll_timer.stop()
            # Final flush from the log file.
            self._tail_log()
            if self._buffer:
                tail = self._buffer.decode("utf-8", errors="replace")
                if tail.strip():
                    self.log_line.emit(tail)
                self._buffer = b""

            if self._lock_key is not None:
                writer, run = self._lock_key
                # External attach can't tell success from error — only
                # mark terminal states when we actually own the process.
                if self._popen is None:
                    state = joblock.EFFECTIVE_KILLED if self._stop_requested else joblock.EFFECTIVE_SUCCESS
                elif self._stop_requested:
                    state = joblock.EFFECTIVE_KILLED
                elif exit_code == 0:
                    state = joblock.EFFECTIVE_SUCCESS
                else:
                    state = joblock.EFFECTIVE_ERROR
                joblock.update_lock(
                    writer, run,
                    state=state,
                    exit_code=int(exit_code),
                    finished_at=joblock.now_iso(),
                )

            self._lock_key = None
            self._log_path = None
            self._popen = None
            self._external_pid = None
            self._stop_requested = False
            self.finished.emit(int(exit_code))

else:  # pragma: no cover — only when PySide6 missing in tests

    JobRunner = None  # type: ignore


__all__ = ["JobRunner", "shell_quote_argv"]
