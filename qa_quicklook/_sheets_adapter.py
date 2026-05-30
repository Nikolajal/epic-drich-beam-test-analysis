"""Thin Google Sheets adapter — push + pull, lazy-imports the optional deps.

Lives in a separate module from ``sheets_sync`` so the deterministic
side of the feature (config, snapshot, render, reverse-merge logic)
imports cleanly on a vanilla dashboard install.  The Google client
libraries (``google-auth``, ``google-api-python-client``) are only
needed on the machine that actually pushes — typically one operator's
laptop or a beam-test-side service host.

Install on that machine::

    pip install google-api-python-client google-auth

Both adapter entry points (``push_snapshot``, ``pull_runs_worksheet``)
raise ``MissingDependencyError`` with a copy-pasteable hint when the
packages aren't available, so the failure mode is one clear line in
the worker status bar rather than a stack trace deep in the import.
"""

from __future__ import annotations

import os
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import sheets_sync


#  Narrowest scope that supports read + write on Sheets the SA has
#  been explicitly shared on.  Deliberately NOT ``drive`` — we don't
#  need Drive-wide enumeration, and a narrower scope is one less
#  thing to explain in the IAM review.
SHEETS_SCOPE = "https://www.googleapis.com/auth/spreadsheets"


# ---------------------------------------------------------------------------
# Error types — let the worker / CLI distinguish "not installed" from
# "credentials wrong" from "Sheet permission denied" without parsing
# stack traces.
# ---------------------------------------------------------------------------


class SheetsAdapterError(RuntimeError):
    """Base class for any push/pull failure the worker should surface."""


class MissingDependencyError(SheetsAdapterError):
    """``google-api-python-client`` / ``google-auth`` not importable."""


class CredentialsError(SheetsAdapterError):
    """Service-account JSON missing, malformed, or revoked."""


class PermissionDeniedError(SheetsAdapterError):
    """The SA doesn't have Editor access on the target Sheet."""


class NotFoundError(SheetsAdapterError):
    """``spreadsheet_id`` doesn't resolve to a Sheet the SA can see."""


@dataclass
class PushResult:
    """Outcome of one ``push_snapshot`` call.

    ``last_push_at`` is the local ISO timestamp written to the
    ``meta`` worksheet.  ``updated_ranges`` is a flat list of
    ``A1`` notations the Sheets API confirmed it wrote, for log /
    debug surfacing.  ``rows_changed`` counts the row-level diff
    cardinality — zero rows changed means "no-op push, only
    formatting / metadata touched".
    """

    last_push_at: str
    updated_ranges: list[str]
    rows_changed: int = 0
    worksheets_skipped: int = 0


def _col_letter(n: int) -> str:
    """Convert a 1-based column index to A1-notation letter.

    ``1 → "A"``, ``26 → "Z"``, ``27 → "AA"``.  Used by the diff-aware
    push path to address row ranges without hardcoding a column cap.
    Defensive against ``n <= 0`` (returns ``"A"`` — the caller's
    bug, but better to over-update one cell than to throw mid-batch).
    """
    if n <= 0:
        return "A"
    s = ""
    while n > 0:
        n, r = divmod(n - 1, 26)
        s = chr(65 + r) + s
    return s


# ---------------------------------------------------------------------------
# Lazy imports + credential plumbing.
# ---------------------------------------------------------------------------


def _import_google():
    """Import the Google client libs on demand.

    Two packages — ``google.oauth2.service_account`` (from
    ``google-auth``) and ``googleapiclient.discovery`` (from
    ``google-api-python-client``) — wrapped in one try so a single
    install of either-or-both gives a focused hint.
    """
    try:
        from google.oauth2 import service_account  # noqa: F401
        from googleapiclient import discovery  # noqa: F401
        from googleapiclient.errors import HttpError  # noqa: F401
    except ImportError as exc:
        raise MissingDependencyError(
            "google client libs not installed — run "
            "`pip install google-api-python-client google-auth` "
            "on the machine that runs the pusher.  Original error: "
            f"{exc}"
        ) from exc
    return service_account, discovery, HttpError


def _check_sa_file(cfg: sheets_sync.SheetsConfig) -> Path:
    """Validate the service-account file exists + is private-ish.

    Mode check is best-effort — we *warn* (via stderr) on world-
    readable files but don't refuse, because some operator
    environments mount ``~`` from a shared FS where the bit isn't
    settable.  Returns the resolved path so the adapter can hand it
    straight to ``from_service_account_file``.
    """
    path = cfg.service_account_path
    if not path.is_file():
        raise CredentialsError(
            f"service-account file not found at {path} — "
            "see qa_quicklook/README.md → 'Cross-shifter sync setup'."
        )
    try:
        mode = path.stat().st_mode & 0o777
        if mode & (stat.S_IROTH | stat.S_IWOTH):
            #  Print, don't raise — see docstring.
            import sys
            print(
                f"[sheets_sync] WARNING: {path} is world-readable "
                f"(mode {oct(mode)}).  Run `chmod 600 {path}` to "
                "lock it down.",
                file=sys.stderr,
            )
    except OSError:
        pass
    return path


def _build_service(cfg: sheets_sync.SheetsConfig):
    """Authenticate and return a ``sheets.spreadsheets()`` resource.

    ``cache_discovery=False`` because googleapiclient's discovery
    cache fights with read-only filesystems (some operator boxes
    mount the home dir noexec) and the cache buys nothing here — we
    open exactly one service per push tick.
    """
    service_account, discovery, _ = _import_google()
    path = _check_sa_file(cfg)
    try:
        creds = service_account.Credentials.from_service_account_file(
            str(path), scopes=[SHEETS_SCOPE],
        )
    except (ValueError, OSError) as exc:
        raise CredentialsError(
            f"could not load service-account JSON at {path}: {exc}. "
            "Re-generate the key in the GCP console (IAM → Service "
            "Accounts → Keys → Add key) and redistribute."
        ) from exc
    service = discovery.build(
        "sheets", "v4",
        credentials=creds,
        cache_discovery=False,
    )
    return service.spreadsheets()


def _translate_http_error(exc, cfg: sheets_sync.SheetsConfig) -> SheetsAdapterError:
    """Map an ``HttpError`` to the typed error the caller wants."""
    status = getattr(exc, "status_code", None)
    if status is None:
        resp = getattr(exc, "resp", None)
        status = getattr(resp, "status", None)
    try:
        status = int(status) if status is not None else None
    except (TypeError, ValueError):
        status = None
    msg = str(exc)
    if status == 401:
        return CredentialsError(
            "401 Unauthorized — the service-account JSON key was "
            "rejected by Google.  Likely revoked or rotated; "
            "generate a fresh key and update "
            f"{cfg.service_account_path}.  ({msg})"
        )
    if status == 403:
        return PermissionDeniedError(
            "403 Permission denied — share the Sheet with the "
            "service-account email as Editor (the email is in the "
            f"client_email field of {cfg.service_account_path}).  "
            f"({msg})"
        )
    if status == 404:
        return NotFoundError(
            "404 Not found — check [sheets_sync].spreadsheet_id in "
            "qa_quicklook.toml.  Sheet may have been deleted, or "
            "never shared with the service account.  "
            f"({msg})"
        )
    return SheetsAdapterError(f"Sheets API error: {msg}")


# ---------------------------------------------------------------------------
# Push
# ---------------------------------------------------------------------------


def _fetch_sheet_metadata(svc, cfg: sheets_sync.SheetsConfig) -> dict:
    """Fetch the per-worksheet metadata we need for formatting.

    Returns::

        {
            "sheet_id_by_title": {title: sheetId, …},
            "bandings_by_sheet": {sheetId: [bandedRangeId, …], …},
            "conditional_rule_count_by_sheet": {sheetId: int, …},
        }

    The conditional-rule count is what ``sheets_format_requests`` needs
    to emit the right number of ``deleteConditionalFormatRule``
    requests before re-adding fresh ones.
    """
    _, _, HttpError = _import_google()
    fields = (
        "sheets(properties(sheetId,title),"
        "bandedRanges(bandedRangeId),"
        "conditionalFormats,"
        "merges)"
    )
    try:
        meta = svc.get(
            spreadsheetId=cfg.spreadsheet_id,
            fields=fields,
        ).execute()
    except HttpError as exc:
        raise _translate_http_error(exc, cfg) from exc
    out = {
        "sheet_id_by_title": {},
        "bandings_by_sheet": {},
        "conditional_rule_count_by_sheet": {},
        "merges_by_sheet": {},
    }
    for s in meta.get("sheets", []):
        props = s.get("properties", {})
        title = props.get("title")
        sheet_id = props.get("sheetId")
        if title is None or sheet_id is None:
            continue
        out["sheet_id_by_title"][title] = sheet_id
        bandings = s.get("bandedRanges", []) or []
        if bandings:
            out["bandings_by_sheet"][sheet_id] = [
                b["bandedRangeId"] for b in bandings if "bandedRangeId" in b
            ]
        n_rules = len(s.get("conditionalFormats", []) or [])
        if n_rules:
            out["conditional_rule_count_by_sheet"][sheet_id] = n_rules
        merges = s.get("merges", []) or []
        if merges:
            out["merges_by_sheet"][sheet_id] = merges
    return out


def _ensure_worksheets(svc, cfg: sheets_sync.SheetsConfig,
                       desired_titles: list[str],
                       *,
                       existing_titles: set[str] | None = None) -> None:
    """Add any missing worksheets to the target Sheet.

    Cheap: one ``batchUpdate`` per missing title.  Idempotent — once
    all five worksheets exist this is a no-op.  ``existing_titles``
    can be passed by callers that already fetched the spreadsheet
    metadata, to avoid a redundant ``spreadsheets.get`` call.
    """
    _, _, HttpError = _import_google()
    if existing_titles is None:
        try:
            meta = svc.get(spreadsheetId=cfg.spreadsheet_id,
                           fields="sheets(properties(title))").execute()
        except HttpError as exc:
            raise _translate_http_error(exc, cfg) from exc
        existing_titles = {
            s["properties"]["title"] for s in meta.get("sheets", [])
        }
    missing = [t for t in desired_titles if t not in existing_titles]
    if not missing:
        return
    requests = [{"addSheet": {"properties": {"title": t}}} for t in missing]
    try:
        svc.batchUpdate(
            spreadsheetId=cfg.spreadsheet_id,
            body={"requests": requests},
        ).execute()
    except HttpError as exc:
        raise _translate_http_error(exc, cfg) from exc


def push_snapshot(
    rendered: dict[str, list[list[Any]]],
    cfg: sheets_sync.SheetsConfig,
    *,
    apply_formatting: bool = True,
    last_pushed: dict[str, list[list[Any]]] | None = None,
) -> PushResult:
    """Write ``rendered`` worksheets to the configured Sheet.

    Three-phase, idempotent:

      1. ``_fetch_sheet_metadata`` + ``_ensure_worksheets`` — adds any
         of the canonical worksheets that don't exist yet, captures
         per-sheet ids + existing bandings / conditional rules so the
         formatting pass can replace them atomically.
      2. For each worksheet, ``values.clear`` then ``values.update``
         starting at ``A1`` — cheaper than a diff-aware batch update
         while the dataset fits in one round trip (260 runs is well
         inside the Sheets quota).
      3. ``sheets_format_requests`` — one consolidated batchUpdate
         that applies the look from ``_sheets_format.FORMATS`` (header
         style + freeze, column widths, banded rows, conditional
         formatting on quality / state / source columns, data
         validation dropdown on quality, tab colours).  Skippable via
         ``apply_formatting=False`` for the rare push where values
         changed but the layout didn't.

    The ``meta`` worksheet's ``last_push_at`` cell is filled in here
    with the call's local timestamp so a reader can tell at a glance
    when they're looking at stale data.

    Raises one of the typed ``SheetsAdapterError`` subclasses on
    failure; the caller (worker / CLI) maps each to a status-bar
    line.
    """
    _, _, HttpError = _import_google()
    if not cfg.is_configured:
        raise CredentialsError(
            f"[sheets_sync] not configured: {cfg.disabled_reason()}"
        )
    svc = _build_service(cfg)

    titles = list(rendered.keys())

    #  Single ``spreadsheets.get`` up-front pays for both
    #  ``_ensure_worksheets`` and the formatting-request builder
    #  below — no second round trip.
    meta = _fetch_sheet_metadata(svc, cfg)
    existing_titles = set(meta["sheet_id_by_title"].keys())
    _ensure_worksheets(svc, cfg, titles, existing_titles=existing_titles)

    #  If we just created any sheets, refresh metadata so the
    #  formatting pass sees their sheet ids.  Only when needed —
    #  the steady-state path skips this entirely.
    if not all(t in existing_titles for t in titles):
        meta = _fetch_sheet_metadata(svc, cfg)

    #  Phase D.9 fix: unmerge BEFORE the values push.  Writes into a
    #  merged cell's interior get silently swallowed by Sheets — only
    #  the top-left cell of the merge accepts a value.  If the new
    #  column layout shifts group boundaries (Phase D.9 added the
    #  ``Run`` group at the start, shifting every other group right by
    #  3), writes to the new chapter-label positions would land inside
    #  the OLD merges and disappear.  Unmerge first so values land
    #  cleanly; the format pass below re-merges with the new spans.
    pre_unmerge: list[dict] = []
    for _sid, merges in meta.get("merges_by_sheet", {}).items():
        for m in merges:
            pre_unmerge.append({"unmergeCells": {"range": m}})
    if pre_unmerge:
        try:
            svc.batchUpdate(
                spreadsheetId=cfg.spreadsheet_id,
                body={"requests": pre_unmerge},
            ).execute()
        except HttpError as exc:
            raise _translate_http_error(exc, cfg) from exc
        #  Mark as cleared so the format pass doesn't re-issue the
        #  same deletes (which would now be no-ops anyway).
        meta["merges_by_sheet"] = {}

    last_push_at = sheets_sync._iso_now()
    #  Stamp the meta worksheet so the consumer sees a fresh
    #  last_push_at after the push lands — purely cosmetic but the
    #  one piece of information operators ask for first ("when was
    #  this updated last?").
    if "meta" in rendered:
        rendered = {**rendered, "meta": _patch_meta_last_push(
            rendered["meta"], last_push_at,
        )}

    last_pushed = last_pushed or {}
    updated: list[str] = []
    total_rows_changed = 0
    skipped = 0

    for title, rows in rendered.items():
        snapshot_rows = last_pushed.get(title, [])
        #  Per-worksheet skip — the cheapest possible push for the
        #  steady-state case where nothing changed on this tab.
        #  Equality is row-list-of-row-list equality on the rendered
        #  payloads, which is exactly what we save to disk + compare
        #  against.  No false negatives.
        if rows == snapshot_rows:
            skipped += 1
            continue

        rows_changed, ranges = _diff_aware_update(svc, cfg, title, rows, snapshot_rows)
        updated.extend(ranges)
        total_rows_changed += rows_changed

    if apply_formatting:
        _apply_formatting(svc, cfg, rendered, meta)

    return PushResult(
        last_push_at=last_push_at,
        updated_ranges=updated,
        rows_changed=total_rows_changed,
        worksheets_skipped=skipped,
    )


def _diff_aware_update(
    svc,
    cfg: sheets_sync.SheetsConfig,
    title: str,
    rows: list[list[Any]],
    snapshot_rows: list[list[Any]],
) -> tuple[int, list[str]]:
    """Row-level diff update for one worksheet.

    Cheaper than ``clear + update`` once the worksheet stabilises:
    after the first push, typical edits touch 1-2 rows out of 260,
    and the resulting ``values.batchUpdate`` payload is ~150 B instead
    of ~80 KB.

    **No-baseline case** (``snapshot_rows == []``): we can't tell
    whether the worksheet had pre-existing rows past our new tail or
    not, so we issue a full ``values.clear`` first.  This catches the
    "post-cache-wipe push" scenario where prior content from before
    the snapshot was purged would otherwise ghost forever underneath
    a shorter new payload.  Net cost: one extra round-trip on the
    first push per worksheet; zero overhead steady-state.

    Removed rows (snapshot had more rows than the new render) get
    cleared individually so the trailing block doesn't ghost.

    Returns ``(rows_changed, ranges_updated)`` for the caller's log.
    """
    _, _, HttpError = _import_google()
    n_new = len(rows)
    n_old = len(snapshot_rows)
    if n_new == 0 and n_old == 0:
        return 0, []

    #  No-baseline guard: wipe the whole worksheet before pushing.
    #  The diff loop below then writes every row as "new", which is
    #  exactly what we want when there's nothing to diff against.
    if n_old == 0 and n_new > 0:
        try:
            svc.values().clear(
                spreadsheetId=cfg.spreadsheet_id,
                range=title,
            ).execute()
        except HttpError as exc:
            raise _translate_http_error(exc, cfg) from exc

    #  Per-row diff.  A row is "changed" if either its content
    #  differs OR it's beyond the snapshot's tail (i.e. new appendage).
    data: list[dict] = []
    for r_idx, new_row in enumerate(rows):
        old_row = snapshot_rows[r_idx] if r_idx < n_old else None
        if old_row == new_row:
            continue
        if not new_row:
            #  Empty row — nothing meaningful to push, but we want to
            #  clear the existing cells if there were any.
            end_col = _col_letter(len(old_row) if old_row else 1)
            rng = f"{title}!A{r_idx + 1}:{end_col}{r_idx + 1}"
            data.append({"range": rng, "values": [[]]})
            continue
        end_col = _col_letter(len(new_row))
        rng = f"{title}!A{r_idx + 1}:{end_col}{r_idx + 1}"
        data.append({"range": rng, "values": [new_row]})

    #  Trailing rows removed.  Clear cells from the new tail down to
    #  the old tail so they don't ghost on the Sheet.  One clear range
    #  is cheaper than one per row.
    if n_new < n_old:
        max_cols = max(len(r) for r in snapshot_rows) if snapshot_rows else 1
        end_col = _col_letter(max_cols)
        clear_range = f"{title}!A{n_new + 1}:{end_col}{n_old}"
        try:
            svc.values().clear(
                spreadsheetId=cfg.spreadsheet_id,
                range=clear_range,
            ).execute()
        except HttpError as exc:
            raise _translate_http_error(exc, cfg) from exc

    if not data:
        return 0, []

    try:
        resp = svc.values().batchUpdate(
            spreadsheetId=cfg.spreadsheet_id,
            body={
                "valueInputOption": "RAW",
                "data": data,
            },
        ).execute()
    except HttpError as exc:
        raise _translate_http_error(exc, cfg) from exc

    #  Sheets returns the per-range updated info — surface a couple
    #  of A1 hints for the log without bloating the result tuple.
    ranges = [d["range"] for d in data[:5]]
    if len(data) > 5:
        ranges.append(f"…+{len(data) - 5} more")
    return len(data), ranges


def _apply_formatting(
    svc,
    cfg: sheets_sync.SheetsConfig,
    rendered: dict[str, list[list[Any]]],
    meta: dict,
) -> None:
    """Apply the shared ``_sheets_format.FORMATS`` look to the live Sheet.

    Delegates request-construction to ``sheets_format_requests`` so
    the xlsx writer and the live Sheets path keep using the same
    spec.  Wraps the resulting list in a single ``batchUpdate`` —
    Sheets executes the requests in order, so existing bandings +
    conditional rules get deleted before fresh ones are added,
    keeping repeated pushes idempotent.

    Safe to skip entirely (``apply_formatting=False``) when only
    values need updating; the formatting survives between pushes.
    Cost on each call is one ``batchUpdate`` round trip, typically
    well under 1 s for the five-worksheet payload.
    """
    _, _, HttpError = _import_google()
    from . import _sheets_format

    requests = _sheets_format.sheets_format_requests(
        rendered,
        sheet_id_by_title=meta["sheet_id_by_title"],
        existing_bandings_by_sheet=meta["bandings_by_sheet"],
        existing_conditional_rules_by_sheet=meta["conditional_rule_count_by_sheet"],
        existing_merges_by_sheet=meta.get("merges_by_sheet", {}),
    )
    if not requests:
        return
    try:
        svc.batchUpdate(
            spreadsheetId=cfg.spreadsheet_id,
            body={"requests": requests},
        ).execute()
    except HttpError as exc:
        #  Formatting failures shouldn't fail the whole push — the
        #  values landed.  Translate so the caller surfaces a clear
        #  line but doesn't blow up; we re-raise as a typed adapter
        #  error so the worker can decide to keep going on the next
        #  tick.
        raise _translate_http_error(exc, cfg) from exc


def _patch_meta_last_push(meta_rows: list[list[Any]],
                          last_push_at: str) -> list[list[Any]]:
    """Return a copy of ``meta_rows`` with the ``last_push_at`` cell set."""
    out = [list(r) for r in meta_rows]
    for r in out[1:]:  # skip header
        if r and r[0] == "last_push_at":
            if len(r) >= 2:
                r[1] = last_push_at
            else:
                r.append(last_push_at)
            break
    return out


# ---------------------------------------------------------------------------
# Pull
# ---------------------------------------------------------------------------


def pull_runs_worksheet(
    cfg: sheets_sync.SheetsConfig,
    *,
    sheet_name: str = "runs",
) -> list[list[Any]]:
    """Fetch a runs-shaped worksheet as a 2D list of raw-typed cells.

    Phase B has one ``runs (YYYY)`` worksheet per campaign year.  The
    caller passes the worksheet name explicitly so this function
    doesn't have to know about multi-year semantics — ``sync_once``
    iterates over years and calls us once per.

    Critically uses ``valueRenderOption=UNFORMATTED_VALUE`` so Sheets
    returns the underlying types — int, float, bool, str — rather
    than locale-formatted display strings.  Without this flag, a
    European-locale Sheet renders ``51.5`` as the string ``"51,5"``
    on read; the reverse-merge then can't compare it against the
    local TOML's ``51.5`` (float) and mistakenly fires an "edit"
    for every numeric cell on every push.  Lesson learned the hard
    way on 2026-05-29 — the database had to be reverted from git.

    Returns ``[]`` when the worksheet is missing entirely (e.g.
    operator hasn't run a first push yet).
    """
    _, _, HttpError = _import_google()
    if not cfg.is_configured:
        raise CredentialsError(
            f"[sheets_sync] not configured: {cfg.disabled_reason()}"
        )
    svc = _build_service(cfg)
    try:
        resp = svc.values().get(
            spreadsheetId=cfg.spreadsheet_id,
            range=sheet_name,
            valueRenderOption="UNFORMATTED_VALUE",
        ).execute()
    except HttpError as exc:
        err = _translate_http_error(exc, cfg)
        #  A 400 "Unable to parse range" means the worksheet
        #  doesn't exist yet.  Treat as "no Sheet state to merge"
        #  rather than a hard failure — the next push creates it.
        if "Unable to parse range" in str(exc):
            return []
        raise err from exc
    return list(resp.get("values", []))


__all__ = [
    "CredentialsError",
    "MissingDependencyError",
    "NotFoundError",
    "PermissionDeniedError",
    "PushResult",
    "SHEETS_SCOPE",
    "SheetsAdapterError",
    "pull_runs_worksheet",
    "push_snapshot",
]
