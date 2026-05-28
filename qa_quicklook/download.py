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

import shlex
import sys
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


__all__ = [
    "RsyncConfig",
    "build_argv",
    "expected_local_path",
    "load_config",
    "probe_ssh_keyauth",
    "save_address",
]
