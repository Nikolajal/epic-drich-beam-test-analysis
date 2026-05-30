"""qa_quicklook/retention.py — two-tier disk retention sweep.

Operator-designed policy (2026-05-29 audit, see BACKLOG row "qa_pipeline:
two-tier auto-cleanup"):

    Tier 1 — Full-keep (last ``full_keep_n`` runs)
        Keep EVERYTHING: raw decoded device files, lightdata.root,
        recodata.root, recotrackdata.root, PDFs, manifests.  These are
        the runs the shifter is most likely to inspect deeply.

    Tier 2 — QA-only buffer (older than ``full_keep_n`` but within ``qa_keep_n``)
        Keep only the QA artefacts: ``recodata.root`` + ``qa/<step>/*.pdf``
        + manifests, and optionally ``recotrackdata.root``.  The
        bulk-on-disk per-device ``decoded/*.root`` and ``lightdata.root``
        are pruned — that's where the disk savings come from.

    Beyond tier 2
        Whole run directory removed.  Anything beyond the QA-only buffer
        is fully re-acquirable from the DAQ host, so this tier is the
        safety net against unbounded disk growth across a campaign.

API
---

``sweep(data_dir, full_keep_n, qa_keep_n, keep_recotrackdata) -> plan``
    PURE function (read-only on the filesystem — only ``iterdir`` is
    called).  Returns a plan dict the caller can inspect / log before
    any destructive action.

``apply(plan) -> report``
    Executes a plan.  Returns a report dict with bytes freed, paths
    deleted, and any errors.

Calling ``sweep`` without ``apply`` is the "dry-run" mode — the plan
dict is self-describing and easy to surface in the log dock.

Caller responsibilities
-----------------------

The module is intentionally policy-only.  Callers (currently
``app.py`` and ``runmanager._on_download``) are responsible for:

    - Filtering out runs with live joblock entries before applying
      (a sweep mid-write would corrupt the in-flight ROOT file).
    - Surfacing the plan to the operator before ``apply()`` runs in
      destructive contexts.  Every UI surface should carry the
      "still re-downloadable from the DAQ host" reassurance.

Run-id convention
-----------------

Only directories matching ``YYYYMMDD-HHMMSS`` are considered.  Other
directories (``lost+found``, ``cache``, …) are ignored.  This matches
the convention enforced by the remote SSH listing script in
``download.list_remote_runs``.
"""

from __future__ import annotations

import re
import shutil
from pathlib import Path
from typing import Any

#  YYYYMMDD-HHMMSS — same regex shape as the remote-side ls filter so
#  the local sweep can't accidentally prune a non-run directory.
_RUN_ID_RE = re.compile(r"^\d{8}-\d{6}$")

#  Three-state retention model (sweep audit 2026-05-30).  Markers are
#  hidden zero-byte files at the run-dir root:
#    .qa_managed     — auto-downloaded by the dashboard's Download
#                      flow; eligible for the tier rules below.
#    .qa_persistent  — operator-pinned baseline (right-click pin or
#                      `python -m qa_quicklook.retention pin <RUN>`).
#                      Exempt from sweep even when .qa_managed is also
#                      present.
#    (neither)       — user-managed run (manual rsync / cp); totally
#                      out of QA's purview.  Never appears in any
#                      plan bucket when ``enforce_qa_managed`` is True.
QA_MANAGED_MARKER: str = ".qa_managed"
QA_PERSISTENT_MARKER: str = ".qa_persistent"


def is_qa_managed(run_dir: Path) -> bool:
    """True iff the run dir carries the auto-download marker."""
    return (run_dir / QA_MANAGED_MARKER).exists()


def is_persistent(run_dir: Path) -> bool:
    """True iff the run dir is pinned as a persistent baseline.

    Independent of ``is_qa_managed``: a user-managed run could in
    principle be pinned too (e.g. operator does `pin` on a manually-
    rsync'd run to make the pin status survive a later auto-download
    that would otherwise convert it to qa-managed).
    """
    return (run_dir / QA_PERSISTENT_MARKER).exists()


def mark_qa_managed(run_dir: Path) -> None:
    """Drop the ``.qa_managed`` marker.  Idempotent.

    Called by the dashboard's Download flow right after rsync launches
    so that even a failed/partial download lands in the qa-managed
    bucket (and thus stays subject to retention — partial downloads
    should be cleaned, not preserved).  The marker is a zero-byte
    file; ``touch`` semantics are sufficient.
    """
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / QA_MANAGED_MARKER).touch(exist_ok=True)


def pin_persistent(run_dir: Path) -> None:
    """Drop the ``.qa_persistent`` marker — pin the run as a baseline.

    Idempotent.  After this call ``sweep()`` will exempt the run from
    all deletion buckets (when ``enforce_qa_managed`` is True).
    """
    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / QA_PERSISTENT_MARKER).touch(exist_ok=True)


def unpin_persistent(run_dir: Path) -> None:
    """Remove the ``.qa_persistent`` marker.  Idempotent; no error if
    the marker doesn't exist."""
    marker = run_dir / QA_PERSISTENT_MARKER
    try:
        marker.unlink()
    except FileNotFoundError:
        pass

#  Files/dirs that survive the QA-only demotion.  Everything else
#  inside a tier-2 run dir is pruned.  ``recotrackdata.root`` is
#  conditional (see ``keep_recotrackdata`` in ``sweep``).
_QA_KEEP_ALWAYS: frozenset[str] = frozenset({
    "qa",                         # qa/<step>/*.pdf + manifests
    "recodata.root",              # the smaller analysed product
    "manifest.json",              # generic manifest (if writer emits)
    "qa_pipeline_manifest.json",  # qa_pipeline-emitted manifest
})


def list_run_dirs(data_dir: Path) -> list[Path]:
    """Return all run directories under ``data_dir``, newest-first.

    A "run directory" is a child of ``data_dir`` whose name matches the
    ``YYYYMMDD-HHMMSS`` convention.  The lex sort doubles as a
    chronological sort because the convention is timestamp-prefixed.
    """
    if not data_dir.is_dir():
        return []
    runs = [
        p for p in data_dir.iterdir()
        if p.is_dir() and _RUN_ID_RE.match(p.name)
    ]
    return sorted(runs, key=lambda p: p.name, reverse=True)


def _qa_only_prune_targets(run_dir: Path, keep_recotrackdata: bool) -> list[Path]:
    """Files/dirs to delete when demoting ``run_dir`` to the QA-only tier.

    Everything that isn't in ``_QA_KEEP_ALWAYS`` (plus optionally
    ``recotrackdata.root``) gets queued for deletion.  Notably
    ``lightdata.root`` and all per-device dirs (``rdo-NNN/``, ``alcdaq-N/``,
    …) are pruned — they're the bulk-on-disk contributors.
    """
    keep = set(_QA_KEEP_ALWAYS)
    if keep_recotrackdata:
        keep.add("recotrackdata.root")
    targets: list[Path] = []
    try:
        for child in run_dir.iterdir():
            if child.name in keep:
                continue
            targets.append(child)
    except OSError:
        #  Race against an external delete or permission issue — the
        #  apply() path will surface the failure in its error list.
        pass
    return targets


def sweep(
    data_dir: Path,
    full_keep_n: int,
    qa_keep_n: int,
    keep_recotrackdata: bool = True,
    enforce_qa_managed: bool = False,
) -> dict[str, Any]:
    """Compute a retention plan.  Read-only on the filesystem.

    Parameters
    ----------
    data_dir
        Path containing run directories (typically ``Data/`` relative
        to the project root, or whatever ``rsync.local_data_dir`` points to).
    full_keep_n
        Number of most-recent runs that keep everything.  Must be ≥ 0.
        ``0`` means no full-keep tier — every run is QA-only or pruned.
    qa_keep_n
        Total number of runs to keep on disk (inclusive of the
        ``full_keep_n``).  Must be ≥ ``full_keep_n`` and ≥ 0.  ``0``
        means delete every run that isn't in the full-keep tier.
    keep_recotrackdata
        Whether ``recotrackdata.root`` survives the QA-only demotion.
    enforce_qa_managed
        When True, the sweep filters by the three-state marker model
        before applying the tier rules:
          * runs without ``.qa_managed`` → ``user_managed`` (untouched)
          * runs with ``.qa_persistent`` → ``persistent`` (kept whole)
          * remaining qa-managed runs → subject to tier rules
        Default False keeps the legacy behaviour (every run dir is in
        scope) so existing tests + back-compat configs continue to work.
        Production callers (dashboard startup sweep, pre-download
        sweep) flip this on once they're confident every run dir has
        an explicit marker.

    Returns
    -------
    dict
        ``{"data_dir", "full_keep_n", "qa_keep_n", "keep_recotrackdata",``
        ``"enforce_qa_managed",``
        ``"full_keep": [run_dir, ...], "qa_only": [{"run", "delete": [...]}, ...],``
        ``"fully_pruned": [run_dir, ...],``
        ``"persistent": [run_dir, ...], "user_managed": [run_dir, ...]}``.

        ``full_keep`` entries are untouched; ``qa_only`` entries get
        their ``delete`` list removed by ``apply()``; ``fully_pruned``
        entries get ``rm -rf``-ed; ``persistent`` and ``user_managed``
        are reported for transparency but never modified.

    Notes
    -----
    Defensive against bad input — ``full_keep_n > qa_keep_n`` is
    clamped to ``full_keep_n == qa_keep_n`` (no QA-only tier).  Negative
    values are clamped to 0.
    """
    full_keep_n = max(0, int(full_keep_n))
    qa_keep_n = max(full_keep_n, int(qa_keep_n))

    all_runs = list_run_dirs(data_dir)
    plan: dict[str, Any] = {
        "data_dir": str(data_dir),
        "full_keep_n": full_keep_n,
        "qa_keep_n": qa_keep_n,
        "keep_recotrackdata": keep_recotrackdata,
        "enforce_qa_managed": enforce_qa_managed,
        "full_keep": [],
        "qa_only": [],
        "fully_pruned": [],
        "persistent": [],
        "user_managed": [],
    }

    #  Three-state partition (sweep audit 2026-05-30).
    #  When `enforce_qa_managed` is False (default), the partition
    #  collapses to "everything is qa-managed-transient" — preserves
    #  the legacy behaviour the tests already exercise.
    if enforce_qa_managed:
        runs_in_scope: list[Path] = []
        for run in all_runs:
            if not is_qa_managed(run):
                plan["user_managed"].append(str(run))
            elif is_persistent(run):
                plan["persistent"].append(str(run))
            else:
                runs_in_scope.append(run)
    else:
        runs_in_scope = all_runs

    for idx, run in enumerate(runs_in_scope):
        if idx < full_keep_n:
            plan["full_keep"].append(str(run))
        elif idx < qa_keep_n:
            targets = _qa_only_prune_targets(run, keep_recotrackdata)
            #  Skip the demote entry if nothing would be pruned —
            #  keeps the plan tidy when a run is already QA-only-clean
            #  (e.g. a previous sweep already ran).
            if targets:
                plan["qa_only"].append({
                    "run": str(run),
                    "delete": [str(t) for t in targets],
                })
        else:
            plan["fully_pruned"].append(str(run))
    return plan


def _du(path: Path) -> int:
    """Best-effort byte size of ``path`` (file or directory).

    Returns 0 on any OSError — used for the report's bytes-freed
    estimate, not for any decision-making.
    """
    try:
        if path.is_file():
            return path.stat().st_size
        if path.is_dir():
            return sum(
                f.stat().st_size for f in path.rglob("*") if f.is_file()
            )
    except OSError:
        return 0
    return 0


#  TOCTOU mitigation: refuse to delete anything modified within this
#  many seconds of "now".  A run that's actively being rsync'd or
#  written to would otherwise be deleted mid-write by an unlucky
#  sweep — `mtime` jumping forward right before the unlink is the
#  one signal we can read cheaply without a separate joblock check.
#  60 s gives ample headroom for "I just kicked off a download".
RETENTION_MTIME_GRACE_S = 60.0


def apply(plan: dict[str, Any],
          mtime_grace_s: float = RETENTION_MTIME_GRACE_S) -> dict[str, Any]:
    """Execute a sweep plan.

    Parameters
    ----------
    plan
        Output of ``sweep()``.  Safe to mutate before passing in — the
        caller may want to filter out joblock-protected runs.
    mtime_grace_s
        TOCTOU guard window: a target whose ``mtime`` is within this
        many seconds of "now" is skipped (likely an in-flight write).
        Defaults to ``RETENTION_MTIME_GRACE_S`` (60 s).  Tests and
        callers that have a stronger consistency signal (joblock,
        external scheduling) can pass ``0`` to disable the grace.

    Returns
    -------
    dict
        ``{"bytes_freed": int, "n_deleted": int, "errors": [{"path", "error"}, ...], "skipped": [...]}``
        ``bytes_freed`` is the best-effort total of byte sizes
        ``apply()`` saw at delete time.  Errors don't abort the run —
        each failed deletion is captured and ``apply()`` continues.
        ``skipped`` lists targets the TOCTOU grace window kept alive
        (empty when ``mtime_grace_s == 0``).

    Notes
    -----
    Each target is re-stat'd inside ``apply()`` right before deletion
    (TOCTOU mitigation, sweep audit 2026-05-30).  A target whose
    ``mtime`` is within ``mtime_grace_s`` seconds of now is skipped —
    that's the cheap signal for "this run is being written right now"
    (e.g. an in-flight rsync).  A target that no longer exists at
    apply time still goes into ``errors`` (preserves the contract
    callers exercise via ``test_errors_captured_not_raised``).
    Callers should ALSO filter out joblock-protected runs before
    calling ``apply()`` — the joblock signal is the authoritative one;
    the mtime grace is the last-mile guard.
    """
    import time

    bytes_freed = 0
    n_deleted = 0
    errors: list[dict[str, str]] = []
    skipped: list[dict[str, str]] = []
    now = time.time()

    def _delete(target: Path) -> None:
        nonlocal bytes_freed, n_deleted
        try:
            #  TOCTOU re-check: stat right before delete.  The mtime
            #  branch below is the new behaviour (sweep audit 2026-05-30);
            #  the legacy "FileNotFoundError → errors" path is preserved.
            if mtime_grace_s > 0.0:
                try:
                    st = target.lstat()
                except FileNotFoundError:
                    #  Vanished between sweep() and apply().  Fall
                    #  through to the legacy delete attempt so it
                    #  still ends up in ``errors`` (preserves the
                    #  contract).
                    pass
                else:
                    mtime_age_s = now - st.st_mtime
                    if mtime_age_s < mtime_grace_s:
                        skipped.append({"path": str(target),
                                        "reason": (
                                            f"mtime too recent "
                                            f"({mtime_age_s:.1f}s < "
                                            f"{mtime_grace_s}s grace) "
                                            "— likely in-flight write")})
                        return
            size = _du(target)
            if target.is_dir():
                shutil.rmtree(target)
            else:
                target.unlink()
            bytes_freed += size
            n_deleted += 1
        except OSError as e:
            errors.append({"path": str(target), "error": str(e)})

    #  QA-only demotion: per-target deletions inside each run dir.
    for entry in plan.get("qa_only", []):
        for target_str in entry.get("delete", []):
            _delete(Path(target_str))

    #  Fully-pruned: rm -rf the whole run dir.
    for run_str in plan.get("fully_pruned", []):
        _delete(Path(run_str))

    return {
        "bytes_freed": bytes_freed,
        "n_deleted": n_deleted,
        "errors": errors,
        "skipped": skipped,
    }


def format_plan_summary(plan: dict[str, Any]) -> str:
    """One-line human summary of a plan.  Used by callers logging.

    Example: ``"retention: keep 5 full, demote 12 to QA-only, prune 3 runs"``.
    """
    base = (
        "retention: keep {n_full} full, demote {n_qa} to QA-only, "
        "prune {n_pruned} runs"
    ).format(
        n_full=len(plan.get("full_keep", [])),
        n_qa=len(plan.get("qa_only", [])),
        n_pruned=len(plan.get("fully_pruned", [])),
    )
    #  Surface the marker-state buckets when enforce_qa_managed is on
    #  so operators see "5 user-managed + 2 baseline runs exempt" rather
    #  than wonder why the plan looks shorter than they expected.
    if plan.get("enforce_qa_managed"):
        extras = []
        if plan.get("user_managed"):
            extras.append(f"{len(plan['user_managed'])} user-managed")
        if plan.get("persistent"):
            extras.append(f"{len(plan['persistent'])} baseline-pinned")
        if extras:
            base += "  ·  exempt: " + ", ".join(extras)
    return base


# ---------------------------------------------------------------------------
# CLI — sweep audit 2026-05-30
#
# Minimal entry point so an operator can pin / unpin a run from the
# command line without spelunking the dashboard.  Three verbs:
#
#     python -m qa_quicklook.retention pin   <DATA_DIR> <RUN_ID> [...]
#     python -m qa_quicklook.retention unpin <DATA_DIR> <RUN_ID> [...]
#     python -m qa_quicklook.retention status <DATA_DIR> [<RUN_ID> ...]
#
# DATA_DIR defaults to ``Data`` relative to the current dir if omitted
# (matches the dashboard's default ``[rsync].local_data_dir``).
# ---------------------------------------------------------------------------


def _cli_status_line(run_dir: Path) -> str:
    """One line per run: ``YYYYMMDD-HHMMSS  [state]``."""
    if not run_dir.is_dir():
        return f"{run_dir.name:<16}  [MISSING]"
    if not is_qa_managed(run_dir):
        return f"{run_dir.name:<16}  [user-managed]"
    if is_persistent(run_dir):
        return f"{run_dir.name:<16}  [qa-managed · BASELINE PIN]"
    return f"{run_dir.name:<16}  [qa-managed · transient]"


def _resolve_data_dir(arg: str | None) -> Path:
    """``arg`` may be a directory, or empty → ``Data`` relative to CWD."""
    if arg:
        return Path(arg)
    return Path("Data")


def _cli_main(argv: list[str]) -> int:
    """Tiny argparse-free CLI — verbs match the BACKLOG row's contract."""
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(
            "Usage:\n"
            "  python -m qa_quicklook.retention pin   <DATA_DIR> <RUN_ID> [...]\n"
            "  python -m qa_quicklook.retention unpin <DATA_DIR> <RUN_ID> [...]\n"
            "  python -m qa_quicklook.retention status <DATA_DIR> [<RUN_ID> ...]\n"
            "\n"
            "DATA_DIR defaults to ./Data when omitted.\n"
        )
        return 0 if argv else 2

    verb = argv[0]
    rest = argv[1:]
    if verb not in {"pin", "unpin", "status"}:
        print(f"unknown verb: {verb!r}  (try --help)")
        return 2

    #  Heuristic: if the first positional looks like a run-id, the
    #  operator omitted DATA_DIR — fall back to ./Data.
    if rest and _RUN_ID_RE.match(rest[0]):
        data_dir = _resolve_data_dir(None)
        run_ids = rest
    else:
        data_dir = _resolve_data_dir(rest[0] if rest else None)
        run_ids = rest[1:]

    if not data_dir.is_dir():
        print(f"DATA_DIR not found or not a directory: {data_dir}")
        return 2

    if verb == "status":
        if run_ids:
            for rid in run_ids:
                print(_cli_status_line(data_dir / rid))
        else:
            #  No run-ids → list everything in the data dir.
            runs = list_run_dirs(data_dir)
            if not runs:
                print(f"(no runs under {data_dir})")
            for r in runs:
                print(_cli_status_line(r))
        return 0

    if not run_ids:
        print(f"{verb}: no run ids supplied")
        return 2

    rc = 0
    for rid in run_ids:
        run_dir = data_dir / rid
        if not run_dir.is_dir():
            print(f"{rid}: NOT FOUND under {data_dir}")
            rc = 1
            continue
        if verb == "pin":
            pin_persistent(run_dir)
            print(_cli_status_line(run_dir))
        else:  # unpin
            unpin_persistent(run_dir)
            print(_cli_status_line(run_dir))
    return rc


if __name__ == "__main__":
    import sys
    sys.exit(_cli_main(sys.argv[1:]))
