"""Acquisition wrapper for the Run Manager's Download button.

Scheme-aware: one ``[source].address`` picks the transfer backend (see
:func:`detect_backend` / :func:`build_argv`):

  - ``user@host:path``       → rsync over ssh (the DAQ dock; default)
  - ``/abs/path`` / ``file://…`` → local copy (a mounted sandbox)
  - ``https://…``            → online store via curl/wget (STUB — the
                               per-run delivery shape isn't wired yet)

No magic — a hand-built wrapper that surfaces explicit errors when the
source isn't configured (or the backend isn't implemented), leaving the
operator in control of the actual transfer.

The Run Manager streams the transfer output through the same
``JobRunner`` that drives writers, so progress lines + per-run Stop +
the Active-runs panel all work the same way regardless of backend.  The
synthetic "writer" tag for the lock file is just ``"download"``.

Config (``qa_quicklook/qa_quicklook.toml``):

  - ``[source].address``        → the scheme-dispatched source (above).
  - ``[source].local_data_dir`` → project-relative local ``Data/`` mirror
                                   (resolved against ``repo_root``); falls
                                   back to ``[rsync].local_data_dir``.
  - ``[source].extra_args``     → flags forwarded to the transfer tool;
                                   falls back to ``[rsync].extra_args``.

Legacy ``[rsync].remote_host`` + ``remote_data_dir`` are still honoured:
when ``[source].address`` is blank they're synthesised into the ssh form
``<remote_host>:<remote_data_dir>``.  A blank source (neither form
configured) is the "disabled" sentinel — the button raises a clear
error rather than silently doing nothing.
"""

from __future__ import annotations

import re
import shlex
import sys
import time
from dataclasses import dataclass
from pathlib import Path


#  Acquisition backends — the transfer tool chosen from the source
#  address scheme (see :func:`detect_backend`).
BACKEND_RSYNC = "rsync"   # user@host:path  →  rsync over ssh (the DAQ dock)
BACKEND_HTTPS = "https"   # http(s)://…     →  curl/wget (online store; stub)
BACKEND_LOCAL = "local"   # file://… or /abs/path  →  local copy


def detect_backend(address: str) -> str:
    """Pick the acquisition backend from a source address.

    The operator configures ONE ``[source].address`` and the transfer
    tool is inferred from its shape — exactly the "I give you the
    address, you figure out how to fetch it" model:

      * ``https://…`` / ``http://…``     → :data:`BACKEND_HTTPS`
      * ``file://…`` or a bare path      → :data:`BACKEND_LOCAL`
      * ``user@host:path`` / ``host:path`` → :data:`BACKEND_RSYNC`

    The rsync form is recognised by a ``:`` whose left side carries no
    ``/`` (i.e. an ssh ``host:path``, not a URL path or a Unix path).
    Empty / unrecognised → ``BACKEND_RSYNC`` (the historical default).
    """
    a = (address or "").strip()
    if a.startswith(("https://", "http://")):
        return BACKEND_HTTPS
    if a.startswith("file://"):
        return BACKEND_LOCAL
    head = a.split(":", 1)[0]
    if ":" in a and "/" not in head:
        return BACKEND_RSYNC
    if a.startswith(("/", "./", "../")):
        return BACKEND_LOCAL
    return BACKEND_RSYNC


@dataclass
class RsyncConfig:
    """Parsed acquisition config from ``qa_quicklook.toml``.

    Backed by ``[source].address`` (the scheme-dispatched form) when
    present, falling back to the legacy ``[rsync].remote_host`` +
    ``remote_data_dir`` pair for back-compat.  Name kept as
    ``RsyncConfig`` so existing callers don't churn.
    """

    remote_host: str = ""
    remote_data_dir: str = ""
    local_data_dir: str = "Data"
    extra_args: str = "-av"
    source_address: str = ""   # [source].address — overrides remote_host/dir

    def resolved_address(self) -> str:
        """The effective source address: ``[source].address`` if set,
        else the ssh ``host:dir`` synthesised from the legacy ``[rsync]``
        pair.  Empty when neither is configured."""
        if self.source_address.strip():
            return self.source_address.strip()
        host = self.remote_host.strip()
        base = self.remote_data_dir.strip()
        if host and base:
            return f"{host}:{base}"
        return ""

    @property
    def backend(self) -> str:
        """Acquisition backend inferred from :meth:`resolved_address`."""
        return detect_backend(self.resolved_address())

    def rsync_host_dir(self) -> tuple[str, str]:
        """``(host, dir)`` for the ssh/rsync backend, derived from the
        resolved address (so a ``[source].address``-only config works,
        not just the legacy ``[rsync]`` fields).  Returns ``("", "")``
        when the backend isn't rsync or the address has no ``host:dir``
        split — the remote-listing path uses this to refuse non-ssh
        sources cleanly."""
        if self.backend != BACKEND_RSYNC:
            return "", ""
        addr = self.resolved_address()
        if ":" not in addr:
            return "", ""
        host, _, base = addr.partition(":")
        return host.strip(), base.strip()

    @property
    def is_configured(self) -> bool:
        """True iff a source address is available (either form)."""
        return bool(self.resolved_address())


def load_config(dashboard_config: Path) -> RsyncConfig:
    """Read the source config from ``dashboard_config``.

    Prefers the ``[source]`` table and falls back to the legacy
    ``[rsync]`` table.

    Returns a default-constructed ``RsyncConfig`` (which reads as
    ``is_configured = False``) when the file is missing, unparseable,
    or has neither section.  No exception — the Run Manager
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
    rs = data.get("rsync")
    rs = rs if isinstance(rs, dict) else {}
    src = data.get("source")
    src = src if isinstance(src, dict) else {}

    #  [source] is the new scheme-dispatched form; [rsync] is the legacy
    #  fallback for each field, so old configs keep working untouched.
    def _pick(key: str, default: str) -> str:
        return str(src.get(key) or rs.get(key) or default)

    return RsyncConfig(
        remote_host=str(rs.get("remote_host", "") or ""),
        remote_data_dir=str(rs.get("remote_data_dir", "") or ""),
        local_data_dir=_pick("local_data_dir", "Data"),
        extra_args=_pick("extra_args", "-av"),
        source_address=str(src.get("address", "") or ""),
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
    """Compose the acquisition argv for a single run directory.

    Dispatches on ``cfg.backend`` (inferred from the source address):

      * ``rsync`` — ``rsync <extra> <addr>/<run_id> <local>/`` (the DAQ
        dock; current behaviour).
      * ``local`` — ``rsync <extra> <path>/<run_id> <local>/`` against a
        local/file:// path (no host), so a mounted sandbox works today.
      * ``https`` — STUB: raises ``NotImplementedError`` until the online
        store exists and its per-run delivery shape is decided.

    No trailing slash on the source so the *run_id* subdir is copied as
    a whole under the local dir (``<local>/<run_id>/…``).

    Raises:
        ValueError: ``cfg`` not configured or ``run_id`` blank.
        NotImplementedError: the resolved backend has no implementation
            yet (https).
    """
    if not cfg.is_configured:
        raise ValueError(
            "acquisition source is not configured — set "
            "[source].address (or the legacy [rsync].remote_host + "
            "remote_data_dir) in qa_quicklook/qa_quicklook.toml "
            "(Settings tab)."
        )
    run_id = run_id.strip()
    if not run_id:
        raise ValueError("run id is empty")

    local_dir = Path(cfg.local_data_dir)
    if not local_dir.is_absolute():
        local_dir = repo_root / local_dir
    local_arg = str(local_dir).rstrip("/") + "/"

    backend = cfg.backend
    address = cfg.resolved_address()

    if backend == BACKEND_HTTPS:
        #  Online-store backend — intentionally a stub.  The dispatch +
        #  rsync path are live; the https fetch shape (tarball-per-run
        #  vs recursive vs manifest) is deferred until the store exists.
        raise NotImplementedError(
            f"https acquisition backend not implemented yet for "
            f"{address!r} — only ssh/rsync (user@host:path) and local "
            f"paths are supported today.  The online-store fetch shape "
            f"is still to be decided."
        )

    if backend == BACKEND_LOCAL:
        src_root = address[len("file://"):] if address.startswith("file://") else address
        source = f"{src_root.rstrip('/')}/{run_id}"
        argv: list[str] = ["rsync"]
        argv.extend(shlex.split(cfg.extra_args) if cfg.extra_args else [])
        #  Download ONLY *.root: skip the online-processing tree, recurse
        #  every other dir, keep *.root, drop the rest.  Independent of the
        #  user's extra_args — mirrors scripts/download_run.sh.
        argv += ["--exclude=process-online/", "--include=*/",
                 "--include=*.root", "--exclude=*"]
        argv.append(source)
        argv.append(local_arg)
        return argv

    #  Default: rsync over ssh (user@host:path).
    source = f"{address.rstrip('/')}/{run_id}"
    argv = ["rsync"]
    argv.extend(shlex.split(cfg.extra_args) if cfg.extra_args else [])
    #  Download ONLY *.root (see above).
    argv += ["--exclude=process-online/", "--include=*/",
             "--include=*.root", "--exclude=*"]
    argv.append(source)
    argv.append(local_arg)
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
            "acquisition source is not configured — set [source].address "
            "(or [rsync].remote_host + remote_data_dir) in "
            "qa_quicklook/qa_quicklook.toml."
        )

    #  Remote listing is an ssh operation — only meaningful for the
    #  rsync/ssh backend.  Derive host+dir from the resolved address so
    #  a [source].address-only config works (not just the legacy
    #  [rsync] fields).
    if cfg.backend != BACKEND_RSYNC:
        return [], (
            f"remote run listing is only available for ssh/rsync sources; "
            f"the configured source is a '{cfg.backend}' target — type the "
            f"run id manually."
        )
    remote_host, remote_dir = cfg.rsync_host_dir()
    remote_dir = remote_dir.rstrip("/")
    if not remote_host or not remote_dir:
        return [], "source address is not a valid ssh host:dir"

    #  shlex.quote so a directory with spaces or shell metacharacters
    #  in [rsync].remote_data_dir doesn't tear the script open.
    script = _LIST_SCRIPT % {"dir": shlex.quote(remote_dir)}

    try:
        proc = subprocess.run(
            ["ssh",
             "-o", "BatchMode=yes",
             "-o", f"ConnectTimeout={max(2, timeout_s // 2)}",
             "-o", "StrictHostKeyChecking=accept-new",
             remote_host, script],
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
