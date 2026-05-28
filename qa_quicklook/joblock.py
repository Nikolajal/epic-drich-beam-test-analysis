"""Per-(writer, run) lock + status files.

When the Run Manager spawns a writer, it drops a JSON lock under
``~/.cache/qa_quicklook/jobs/`` recording the PID, argv, and current
state.  Any dashboard instance (including a fresh one launched after
the previous was killed) can:

  - tell whether a writer is *currently* running for a given run, by
    reading the lock and checking the PID is still alive;
  - distinguish *idle* (no lock), *running* (PID alive, state=running),
    *killed/abandoned* (state=running but PID gone — the operator
    killed the dashboard mid-run), *success*, *error*.

The lock file is the single source of truth across dashboard
sessions.  No central daemon, no IPC — just a JSON file per
``(writer, run)`` pair.  The cost is one file write per state
transition, which is nothing compared to the time the writers take.

Lock filename: ``<writer>_<run_id>.json`` under
``~/.cache/qa_quicklook/jobs/``.  Sticking to ASCII so it sorts and
greps cleanly in the cache dir.

Lock schema (every field present after the first write):

  ``writer``        str    short writer tag, e.g. "lightdata".
  ``run``           str    run id (YYYYMMDD-HHMMSS).
  ``pid``           int    subprocess PID.
  ``argv``          list   shell-quoted argv for forensic context.
  ``started_at``    str    ISO timestamp.
  ``state``         str    one of ``running`` / ``success`` /
                           ``error`` / ``killed``.
  ``exit_code``     int|n  ``None`` while running.
  ``finished_at``   str|n  ``None`` while running.

State transitions are write-then-replace: every update rewrites the
whole file atomically (``write_text`` to ``<file>.tmp`` then
``os.replace``).
"""

from __future__ import annotations

import json
import os
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional


CACHE_DIR = Path.home() / ".cache" / "qa_quicklook" / "jobs"


# ---------------------------------------------------------------------------
# Dataclass mirror of the lock schema.
# ---------------------------------------------------------------------------


@dataclass
class JobLock:
    writer: str
    run: str
    pid: int
    argv: list
    started_at: str
    state: str = "running"
    exit_code: Optional[int] = None
    finished_at: Optional[str] = None

    @classmethod
    def from_dict(cls, data: dict) -> "JobLock":
        return cls(
            writer=str(data.get("writer", "")),
            run=str(data.get("run", "")),
            pid=int(data.get("pid", 0)),
            argv=list(data.get("argv", [])),
            started_at=str(data.get("started_at", "")),
            state=str(data.get("state", "running")),
            exit_code=data.get("exit_code"),
            finished_at=data.get("finished_at"),
        )

    def to_dict(self) -> dict:
        return asdict(self)


# ---------------------------------------------------------------------------
# File helpers
# ---------------------------------------------------------------------------


def ensure_cache_dir() -> Path:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    return CACHE_DIR


def lock_path(writer: str, run: str) -> Path:
    """Return the absolute path of the lock file for ``(writer, run)``."""
    ensure_cache_dir()
    safe_writer = "".join(c for c in writer if c.isalnum() or c in "-_")
    safe_run = "".join(c for c in run if c.isalnum() or c in "-_")
    return CACHE_DIR / f"{safe_writer}_{safe_run}.json"


def write_lock(lock: JobLock) -> Path:
    """Atomically write ``lock`` to its file.  Returns the path."""
    path = lock_path(lock.writer, lock.run)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(lock.to_dict(), indent=2))
    os.replace(tmp, path)
    return path


def update_lock(writer: str, run: str, **fields) -> Optional[JobLock]:
    """Patch the existing lock with ``fields`` and rewrite.

    Returns the updated ``JobLock`` or ``None`` if the lock doesn't
    exist (the caller probably wants ``write_lock`` instead).
    """
    existing = read_lock(writer, run)
    if existing is None:
        return None
    for k, v in fields.items():
        setattr(existing, k, v)
    write_lock(existing)
    return existing


def read_lock(writer: str, run: str) -> Optional[JobLock]:
    path = lock_path(writer, run)
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    return JobLock.from_dict(data)


def clear_lock(writer: str, run: str) -> None:
    path = lock_path(writer, run)
    try:
        path.unlink()
    except FileNotFoundError:
        pass


def list_locks() -> list[JobLock]:
    """All locks currently on disk (any writer, any run)."""
    if not CACHE_DIR.is_dir():
        return []
    out: list[JobLock] = []
    for p in CACHE_DIR.glob("*.json"):
        try:
            data = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        out.append(JobLock.from_dict(data))
    return out


# ---------------------------------------------------------------------------
# PID liveness
# ---------------------------------------------------------------------------


def pid_alive(pid: int) -> bool:
    """True iff a process with this PID currently exists.

    Uses the POSIX ``kill(pid, 0)`` trick — sends no signal but
    raises if the PID is gone.  Works on macOS + Linux without
    additional dependencies.
    """
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # PID exists but is owned by another user — still "alive".
        return True
    return True


def now_iso() -> str:
    """Current local time as ``YYYY-MM-DDTHH:MM:SS``."""
    return time.strftime("%Y-%m-%dT%H:%M:%S")


# ---------------------------------------------------------------------------
# Effective state — combines what's on disk with PID liveness.
# ---------------------------------------------------------------------------


EFFECTIVE_RUNNING = "running"
EFFECTIVE_SUCCESS = "success"
EFFECTIVE_ERROR = "error"
EFFECTIVE_KILLED = "killed"
EFFECTIVE_ABANDONED = "abandoned"   # lock says running but PID is gone
EFFECTIVE_IDLE = "idle"             # no lock at all


def effective_state(lock: Optional[JobLock]) -> str:
    """Translate a lock + liveness check into the dashboard's status palette.

    ``lock=None`` → ``"idle"``.
    ``state="running"`` AND PID alive → ``"running"``.
    ``state="running"`` AND PID gone   → ``"abandoned"`` (orphan).
    Anything else  → the lock's recorded ``state``.
    """
    if lock is None:
        return EFFECTIVE_IDLE
    if lock.state == EFFECTIVE_RUNNING:
        return EFFECTIVE_RUNNING if pid_alive(lock.pid) else EFFECTIVE_ABANDONED
    return lock.state


__all__ = [
    "CACHE_DIR",
    "EFFECTIVE_ABANDONED",
    "EFFECTIVE_ERROR",
    "EFFECTIVE_IDLE",
    "EFFECTIVE_KILLED",
    "EFFECTIVE_RUNNING",
    "EFFECTIVE_SUCCESS",
    "JobLock",
    "clear_lock",
    "effective_state",
    "ensure_cache_dir",
    "list_locks",
    "lock_path",
    "now_iso",
    "pid_alive",
    "read_lock",
    "update_lock",
    "write_lock",
]
