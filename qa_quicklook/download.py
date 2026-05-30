"""Rsync wrapper for the Run Manager's Download button.

Reads ``[rsync]`` from ``qa_quicklook/qa_quicklook.toml`` and builds
the argv to fetch a single run directory from the DAQ machine.
No magic — a hand-built wrapper that surfaces explicit errors when
the remote isn't configured, leaving the operator in control of the
actual transfer.

The Run Manager streams the rsync output through the same
``JobRunner`` that drives writers, so progress lines + per-run Stop
+ the Active-runs panel all work the same way.  The synthetic
"writer" tag for the lock file is just ``"download"`` — the lock
model doesn't care that it isn't a real writer binary.

Convention for the remote layout:

  - ``[rsync].remote_host``      → ssh-addressable host
                                   (e.g. ``"drich-daq.local"``)
  - ``[rsync].remote_data_dir``  → absolute path under which run
                                   subdirectories live on the DAQ host
  - ``[rsync].local_data_dir``   → project-relative path for the local
                                   ``Data/`` mirror; resolved against
                                   ``repo_root`` if not absolute
  - ``[rsync].extra_args``       → extra flags forwarded verbatim
                                   to rsync (defaults to ``-av``)

A blank ``remote_host`` is the documented "disabled" sentinel — the
button raises a clear error rather than silently doing nothing.
"""

from __future__ import annotations

import re
import shlex
import sys
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass
class RsyncConfig:
    """Parsed ``[rsync]`` section from ``qa_quicklook.toml``."""

    remote_host: str = ""
    remote_data_dir: str = ""
    local_data_dir: str = "Data"
    extra_args: str = "-av"

    @property
    def is_configured(self) -> bool:
        """True iff the remote endpoint is fully filled in."""
        return bool(self.remote_host.strip()) and bool(self.remote_data_dir.strip())


def load_config(dashboard_config: Path) -> RsyncConfig:
    """Read the ``[rsync]`` table from ``dashboard_config``.

    Returns a default-constructed ``RsyncConfig`` (which reads as
    ``is_configured = False``) when the file is missing, unparseable,
    or has no ``[rsync]`` section.  No exception — the Run Manager
    surfaces the "not configured" state via the Download button's
    tooltip + a clear error dialog when clicked.
    """
    if not dashboard_config.is_file():
        return RsyncConfig()
    if sys.version_info >= (3, 11):
        import tomllib
    else:  # pragma: no cover
        import tomli as tomllib  # type: ignore
    try:
        with dashboard_config.open("rb") as fh:
            data = tomllib.load(fh)
    except Exception:  # noqa: BLE001
        return RsyncConfig()
    rs = data.get("rsync") or {}
    if not isinstance(rs, dict):
        return RsyncConfig()
    return RsyncConfig(
        remote_host=str(rs.get("remote_host", "") or ""),
        remote_data_dir=str(rs.get("remote_data_dir", "") or ""),
        local_data_dir=str(rs.get("local_data_dir", "Data") or "Data"),
        extra_args=str(rs.get("extra_args", "-av") or "-av"),
    )


def probe_ssh_keyauth(remote_host: str, timeout_s: int = 6) -> tuple[bool, str]:
    """Test whether passwordless SSH works against ``remote_host``.

    Runs ``ssh -o BatchMode=yes -o ConnectTimeout=N <host> true`` —
    ``BatchMode=yes`` disables every interactive prompt (password,
    passphrase, keyboard-interactive, host-key approval), so the
    command can only succeed when key auth is fully set up and
    cached.  Any prompt → instant failure, which is exactly the
    test we want.

    Returns ``(ok, message)``:
      - ``ok = True``  → silent rsync should work from now on.
      - ``ok = False`` → ``message`` carries the first non-empty
        stderr line from ssh, which is usually enough to point at
        the fix (``Permission denied (publickey)`` → run
        ``ssh-copy-id``; ``Connection timed out`` → host wrong;
        ``Could not resolve hostname`` → DNS / VPN; …).

    The dashboard surfaces both states via the "Test SSH" button on
    the address-prompt dialog so operators can verify their
    ``ssh-copy-id`` actually landed without leaving the GUI.
    """
    import subprocess
    host = (remote_host or "").strip()
    if not host:
        return False, "no remote_host configured"
    try:
        proc = subprocess.run(
            ["ssh",
             "-o", "BatchMode=yes",
             "-o", f"ConnectTimeout={timeout_s}",
             "-o", "StrictHostKeyChecking=accept-new",
             host, "true"],
            capture_output=True, text=True,
            timeout=timeout_s + 2,
        )
    except FileNotFoundError:
        return False, "ssh binary not found on PATH"
    except subprocess.TimeoutExpired:
        return False, f"ssh timed out after {timeout_s}s"
    if proc.returncode == 0:
        return True, "ok — key auth works"
    err = (proc.stderr or proc.stdout or "").strip().splitlines()
    first = err[0] if err else f"ssh exited {proc.returncode}"
    return False, first


def save_address(dashboard_config: Path,
                 *,
                 remote_host: str,
                 remote_data_dir: str) -> None:
    """Persist ``remote_host`` + ``remote_data_dir`` back to ``dashboard_config``.

    Uses ``tomlkit`` so existing comments, ordering, and the rest of
    the file (``[ui]``, ``[detector]``, …) survive the rewrite.
    Creates the file with a minimal ``[rsync]`` table if missing.

    Atomic write: temp sibling + ``os.replace`` so a half-written
    config is never observable to the watcher.

    This is the "fix it on first use" entry point the Run Manager's
    Download button calls when the user fills in the missing address
    dialog — symmetric counterpart of ``load_config``.  Settings tab
    still owns the durable editing flow.
    """
    import os

    import tomlkit

    if dashboard_config.is_file():
        doc = tomlkit.parse(dashboard_config.read_text())
    else:
        doc = tomlkit.document()

    rsync = doc.get("rsync")
    if rsync is None:
        rsync = tomlkit.table()
        doc["rsync"] = rsync

    rsync["remote_host"]     = remote_host.strip()
    rsync["remote_data_dir"] = remote_data_dir.strip()

    tmp = dashboard_config.with_suffix(dashboard_config.suffix + ".tmp")
    tmp.write_text(tomlkit.dumps(doc))
    os.replace(tmp, dashboard_config)


def build_argv(cfg: RsyncConfig, run_id: str, repo_root: Path) -> list[str]:
    """Compose the rsync argv for a single run directory.

    Raises:
        ValueError: when ``cfg`` is not fully configured or ``run_id``
                    is empty / whitespace.

    Layout::

        rsync <extra_args> <remote_host>:<remote_data_dir>/<run_id> <local_data_dir>/

    No trailing slash on the source so rsync copies the *run_id*
    subdir as a whole under the local dir (i.e. you get
    ``<local_data_dir>/<run_id>/...``, not the contents merged into
    ``<local_data_dir>``).
    """
    if not cfg.is_configured:
        raise ValueError(
            "rsync remote is not configured — set "
            "[rsync].remote_host and [rsync].remote_data_dir in "
            "qa_quicklook/qa_quicklook.toml (Settings tab)."
        )
    run_id = run_id.strip()
    if not run_id:
        raise ValueError("run id is empty")

    local_dir = Path(cfg.local_data_dir)
    if not local_dir.is_absolute():
        local_dir = repo_root / local_dir

    remote_path = f"{cfg.remote_data_dir.rstrip('/')}/{run_id}"
    remote = f"{cfg.remote_host}:{remote_path}"

    argv: list[str] = ["rsync"]
    argv.extend(shlex.split(cfg.extra_args) if cfg.extra_args else [])
    argv.append(remote)
    argv.append(str(local_dir).rstrip("/") + "/")
    return argv


def expected_local_path(cfg: RsyncConfig, run_id: str, repo_root: Path) -> Path:
    """Where will the run directory land on disk after rsync completes."""
    local_dir = Path(cfg.local_data_dir)
    if not local_dir.is_absolute():
        local_dir = repo_root / local_dir
    return local_dir / run_id


# ---------------------------------------------------------------------------
# Remote run listing — "what's available to download" for the picker dialog.
# ---------------------------------------------------------------------------


#  Run ids on disk are wall-clock timestamps from the DAQ — 8-digit
#  date + dash + 6-digit time.  Anchored so we don't match
#  partially-named or experimental directories.
_RUN_ID_RE = re.compile(r"^\d{8}-\d{6}$")


@dataclass
class RemoteRun:
    """One entry from the remote ``ls`` of run directories.

    ``mtime_epoch`` is the directory's POSIX mtime as reported by
    ``stat`` on the DAQ host — ``None`` when stat fell through (the
    remote shell still prints the row so the operator at least sees
    the id).  ``mtime_iso`` is the same value rendered in the local
    timezone for display; we keep both since the picker dialog sorts
    by epoch and shows the iso string.

    ``local_present`` flags runs already mirrored under
    ``local_data_dir/`` so the picker can grey-out / badge them.
    """

    run_id: str
    mtime_epoch: int | None = None
    mtime_iso: str | None = None
    local_present: bool = False


#  Sent to the DAQ host over ssh.  POSIX-only (``case`` glob + while
#  read + the stat fallback chain), so it works on Linux + BSD + macOS
#  without depending on GNU find -printf or coreutils -b.  Output is
#  one ``<run_id>\t<mtime_epoch_or_dash>`` line per matching dir; a
#  ``__ERR__:cd_failed`` sentinel on stdout lets the caller distinguish
#  "remote_data_dir is wrong" from "ssh exploded entirely".
_LIST_SCRIPT = r"""set -u
cd %(dir)s 2>/dev/null || { echo "__ERR__:cd_failed"; exit 2; }
ls -1A 2>/dev/null | while IFS= read -r d; do
    [ -d "$d" ] || continue
    case "$d" in
        [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9]) ;;
        *) continue ;;
    esac
    mtime=$(stat -c '%%Y' "$d" 2>/dev/null) \
        || mtime=$(stat -f '%%m' "$d" 2>/dev/null) \
        || mtime=
    printf '%%s\t%%s\n' "$d" "${mtime:--}"
done
"""


def _parse_listing(stdout: str) -> list[tuple[str, int | None]]:
    """Turn the remote script's stdout into ``[(run_id, mtime_epoch | None), …]``.

    Tolerant of trailing whitespace, blank lines, and the
    ``mtime=-`` sentinel that the remote uses when ``stat`` fell
    through.  Anything that doesn't match the run-id shape is
    silently skipped — the script already filters by case glob, but
    we re-validate here so a hostile / corrupted stream can't slip a
    bogus id past us.
    """
    out: list[tuple[str, int | None]] = []
    for raw in stdout.splitlines():
        line = raw.strip()
        if not line or line.startswith("__ERR__:"):
            continue
        parts = line.split("\t", 1)
        run_id = parts[0].strip()
        if not _RUN_ID_RE.match(run_id):
            continue
        mtime: int | None = None
        if len(parts) == 2:
            tok = parts[1].strip()
            if tok and tok != "-":
                try:
                    mtime = int(tok)
                except ValueError:
                    mtime = None
        out.append((run_id, mtime))
    return out


def _iso_local(epoch: int | None) -> str | None:
    """Render an epoch as ``YYYY-MM-DD HH:MM`` in the operator's local TZ."""
    if epoch is None:
        return None
    try:
        return time.strftime("%Y-%m-%d %H:%M", time.localtime(epoch))
    except (OverflowError, OSError, ValueError):
        return None


def list_remote_runs(
    cfg: RsyncConfig,
    repo_root: Path,
    *,
    timeout_s: int = 20,
) -> tuple[list[RemoteRun], str]:
    """SSH to the DAQ host and list run directories under ``remote_data_dir``.

    Returns ``(entries, error_message)``:

      - ``error_message`` is empty on success (including the empty-
        listing case — zero runs is a valid answer, not an error).
      - ``entries`` is sorted newest-first (run ids are timestamps so
        lexical descending = chronological descending).
      - ``local_present`` is set against the resolved ``local_data_dir``
        so the picker can mark already-mirrored runs.

    Common error shapes the message tries to be helpful about:

      - rsync remote not configured (no ssh attempted)
      - ssh binary missing on PATH
      - host unreachable / DNS failure / auth refused (ssh stderr)
      - ``remote_data_dir`` doesn't exist on the host
        (script-emitted sentinel, not an ssh-level error)

    The caller (the Run Manager's picker dialog) surfaces the message
    inline and offers the typed-id fallback so the operator is never
    blocked when the listing path fails.
    """
    import subprocess
    if not cfg.is_configured:
        return [], (
            "rsync remote is not configured — set [rsync].remote_host "
            "and [rsync].remote_data_dir in qa_quicklook/qa_quicklook.toml."
        )

    remote_dir = cfg.remote_data_dir.rstrip("/")
    if not remote_dir:
        return [], "remote_data_dir is empty"

    #  shlex.quote so a directory with spaces or shell metacharacters
    #  in [rsync].remote_data_dir doesn't tear the script open.
    script = _LIST_SCRIPT % {"dir": shlex.quote(remote_dir)}

    try:
        proc = subprocess.run(
            ["ssh",
             "-o", "BatchMode=yes",
             "-o", f"ConnectTimeout={max(2, timeout_s // 2)}",
             "-o", "StrictHostKeyChecking=accept-new",
             cfg.remote_host, script],
            capture_output=True, text=True,
            timeout=timeout_s,
        )
    except FileNotFoundError:
        return [], "ssh binary not found on PATH"
    except subprocess.TimeoutExpired:
        return [], f"ssh listing timed out after {timeout_s}s"

    stdout = proc.stdout or ""
    if "__ERR__:cd_failed" in stdout:
        return [], (
            f"remote_data_dir {remote_dir!r} does not exist on "
            f"{cfg.remote_host} — check the Settings-tab value."
        )
    if proc.returncode != 0:
        err_lines = (proc.stderr or stdout or "").strip().splitlines()
        first = err_lines[0] if err_lines else f"ssh exited {proc.returncode}"
        return [], first

    pairs = _parse_listing(stdout)

    #  Build the local-present set off the resolved Data/ root.  An
    #  iterdir is cheaper than per-id is_dir checks against tens of
    #  candidates, and we already need the directory contents anyway.
    local_dir = Path(cfg.local_data_dir)
    if not local_dir.is_absolute():
        local_dir = repo_root / local_dir
    local_names: set[str] = set()
    if local_dir.is_dir():
        try:
            local_names = {p.name for p in local_dir.iterdir() if p.is_dir()}
        except OSError:
            local_names = set()

    entries = [
        RemoteRun(
            run_id=rid,
            mtime_epoch=mtime,
            mtime_iso=_iso_local(mtime),
            local_present=rid in local_names,
        )
        for rid, mtime in pairs
    ]
    #  Newest first.  Run ids are zero-padded YYYYMMDD-HHMMSS, so
    #  lexical sort matches chronological sort exactly.
    entries.sort(key=lambda e: e.run_id, reverse=True)
    return entries, ""


def newest_run_id(entries: list["RemoteRun"]) -> str | None:
    """Return the lex-newest (= chronologically latest) run id, or None.

    Pure helper — shared by the picker live-run guard and the live
    monitor.  Empty list → None.  Because run ids are YYYYMMDD-HHMMSS
    the lex max IS the chronologically latest.

    The "newest run is the dangerous one" check uses this:
    that one might still be acquiring on the DAQ; downloading it
    yields truncated files.  Anything older is sealed by construction
    (a new run dir on the remote means the previous run's files
    finished flushing).
    """
    if not entries:
        return None
    return max(e.run_id for e in entries)


__all__ = [
    "RemoteRun",
    "RsyncConfig",
    "build_argv",
    "expected_local_path",
    "list_remote_runs",
    "load_config",
    "newest_run_id",
    "probe_ssh_keyauth",
    "save_address",
]
