"""Cross-shifter sync — push the dashboard's run state to a Google Sheet.

Design choice ledger (see ``qa_quicklook/DISCUSSION.md`` → "Cross-
shifter sync" for the full rationale):

  - **Push direction is authoritative**: the local ``run-lists/*.toml``
    + per-(writer, run) joblock files in ``~/.cache/qa_quicklook/jobs/``
    are the source of truth.  The Sheet is the *observable* surface
    that operators glance at from anywhere.
  - **Reverse sync via cell-diff snapshot**: after every successful
    push we persist what we wrote, keyed by ``(run, field)``, under
    ``~/.cache/qa_quicklook/sheets_last_pushed.json``.  On the next
    tick we compare the Sheet's current state against that snapshot;
    cells that differ AND don't match the current local TOML are
    treated as Sheet-side edits and replayed through
    ``rundb.update_run_field(..., source="sheet")`` so they pick up
    the same audit log + Show-history surface as a dashboard edit.
    Last-write-wins on conflicts.
  - **Auth: service-account JSON key**, default path
    ``~/.config/qa_quicklook/sheets-sa.json``.  Never committed.
    Distributed by hand (or a shared 1Password vault) to the operators
    who run the pusher.  The Sheet is shared explicitly with the SA
    email as Editor — that's the write-permission grant.
  - **Optional dependency**: ``google-api-python-client`` +
    ``google-auth``.  Lazy-imported by the push / pull adapters in
    ``_adapter.py`` so the dashboard runs untouched on machines that
    don't carry the extras.

This module owns the *deterministic* side of the feature — config
parsing, on-disk snapshotting, worksheet rendering, cell-diff
reconciliation logic.  No Qt, no network.  The actual Google calls
live in the lazily-imported adapter so this module can be unit-tested
without either dependency.
"""

from __future__ import annotations

import getpass
import json
import os
import re
import socket
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

if sys.version_info >= (3, 11):
    import tomllib as _tomllib
else:  # pragma: no cover
    import tomli as _tomllib  # type: ignore


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------


#  Snapshot of "what we last pushed", keyed by worksheet -> rows.  Lives
#  alongside the joblock files so a single ``~/.cache/qa_quicklook``
#  wipe is enough to fully reset operator-side state.
SNAPSHOT_PATH = Path.home() / ".cache" / "qa_quicklook" / "sheets_last_pushed.json"

#  Reverse-merge safety brake.  When more than this many cells differ
#  between the Sheet and the local snapshot, ``sync_once`` refuses to
#  apply them and raises ``ReverseMergeBrake`` instead.  Operators trip
#  this when the snapshot file is missing or stale — every Sheet cell
#  then looks like an edit to a database row that wasn't actually
#  edited.  The 2026-05-29 incident pushed 260 spurious "edits" into
#  the audit log + corrupted ``v_bias`` floats before we'd shipped this
#  brake; the limit defaults intentionally low so the next near-miss
#  fails loud instead of mangling data.
MAX_REVERSE_EDITS_PER_TICK = 20


class ReverseMergeBrake(RuntimeError):
    """Raised by ``sync_once`` when the reverse-merge crosses the safety brake.

    ``edit_count`` is the number of cells that would have been
    replayed through ``rundb.update_run_field``.  ``threshold`` is the
    configured limit.  The caller (CLI / worker) surfaces a one-liner
    with both numbers + the hint to use ``--force-reverse-merge`` if
    the bulk edit is genuinely intended (rare).
    """

    def __init__(self, edit_count: int, threshold: int, sample: list = None) -> None:
        self.edit_count = edit_count
        self.threshold = threshold
        self.sample = sample or []
        sample_str = ""
        if sample:
            preview = ", ".join(
                f"{e.run_id}.{e.field}" for e in sample[:5]
            )
            extra = f" (+{len(sample) - 5} more)" if len(sample) > 5 else ""
            sample_str = f"  Sample: {preview}{extra}."
        super().__init__(
            f"Reverse-merge brake tripped: {edit_count} Sheet-side edits "
            f"detected, threshold is {threshold}.  This almost always means "
            f"the local snapshot is stale or missing (a wipe of "
            f"~/.cache/qa_quicklook will do it), NOT that the Sheet really "
            f"changed that much.{sample_str}  Investigate; re-run with "
            f"--force-reverse-merge only if you're sure."
        )


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------


@dataclass
class SheetsConfig:
    """Parsed ``[sheets_sync]`` table from ``qa_quicklook.toml``.

    ``enabled = False`` is the documented disabled sentinel — the
    worker stays silent and we never even attempt to import the
    optional Google libs.  ``is_configured`` requires the SA file
    and the spreadsheet id on top of that, so a half-filled config
    fails fast with a pointed message rather than crashing in the
    middle of a push.
    """

    enabled: bool = False
    service_account: str = "~/.config/qa_quicklook/sheets-sa.json"
    spreadsheet_id: str = ""
    push_interval_s: int = 30
    operator_tag: str = ""
    #  Sweep audit (2026-05-30): surface TOML parse errors in
    #  disabled_reason() instead of silently falling back to defaults.
    #  An operator with a typo in qa_quicklook.toml otherwise sees
    #  sheets sync stop working with no log signal.
    parse_error: str = ""

    @property
    def service_account_path(self) -> Path:
        """Resolve ``~`` so the path is usable regardless of launch CWD."""
        return Path(os.path.expanduser(self.service_account))

    @property
    def is_configured(self) -> bool:
        return (
            self.enabled
            and bool(self.spreadsheet_id.strip())
            and self.service_account_path.is_file()
        )

    def disabled_reason(self) -> str:
        """One-liner explaining why ``is_configured`` is False.

        Surface this in the dashboard status bar / CLI so an operator
        flipping the switch knows what's still missing without having
        to source-dive.
        """
        #  Sweep audit (2026-05-30): parse error takes priority — if
        #  the TOML didn't load cleanly, the dataclass holds defaults
        #  and the "enabled = false" / "spreadsheet_id empty" reasons
        #  would mislead the operator.
        if self.parse_error:
            return f"[sheets_sync] config parse error: {self.parse_error}"
        if not self.enabled:
            return "[sheets_sync] enabled = false"
        if not self.spreadsheet_id.strip():
            return "[sheets_sync] spreadsheet_id is empty"
        sa = self.service_account_path
        if not sa.is_file():
            return f"service-account file not found at {sa}"
        return ""


def load_config(dashboard_config: Path) -> SheetsConfig:
    """Read ``[sheets_sync]`` from ``dashboard_config``.

    Returns a default-constructed ``SheetsConfig`` (which reads as
    ``enabled = False`` and ``is_configured = False``) when the file
    is missing, unparseable, or has no ``[sheets_sync]`` section.
    No exception — callers branch on ``is_configured`` / ``disabled_reason``.
    """
    if not dashboard_config.is_file():
        return SheetsConfig()
    try:
        with dashboard_config.open("rb") as fh:
            data = _tomllib.load(fh)
    except _tomllib.TOMLDecodeError as e:
        #  Sweep audit (2026-05-30): capture the parse error message
        #  so disabled_reason() can surface it (was: silent fallback
        #  to defaults; operator with a typo had no clue).
        return SheetsConfig(parse_error=f"{dashboard_config.name}: {e}")
    except OSError as e:
        return SheetsConfig(parse_error=f"{dashboard_config.name}: {e}")
    s = data.get("sheets_sync") or {}
    if not isinstance(s, dict):
        return SheetsConfig()
    defaults = SheetsConfig()
    return SheetsConfig(
        enabled=bool(s.get("enabled", defaults.enabled)),
        service_account=str(s.get("service_account") or defaults.service_account),
        spreadsheet_id=str(s.get("spreadsheet_id") or ""),
        push_interval_s=max(5, int(s.get("push_interval_s") or defaults.push_interval_s)),
        operator_tag=str(s.get("operator_tag") or ""),
    )


def resolve_operator_tag(configured: str) -> str:
    """Fall back to ``$USER@$HOSTNAME`` when no explicit tag is set.

    The configured value wins verbatim when present — operators sometimes
    want a friendly label (``"Mario, shifter 11/29 evening"``) rather than
    a unix identity string.
    """
    configured = (configured or "").strip()
    if configured:
        return configured
    try:
        user = getpass.getuser()
    except Exception:  # noqa: BLE001
        user = os.environ.get("USER") or os.environ.get("LOGNAME") or "?"
    host = socket.gethostname() or "?"
    return f"{user}@{host}"


# ---------------------------------------------------------------------------
# Year auto-detection
# ---------------------------------------------------------------------------


_YEAR_RE = re.compile(r"^(20\d{2})\.database\.toml$")

#  Strict run-id timestamp pattern.  Used as a typo-guard before the
#  reverse-merge auto-appends a row for a previously-unknown run id —
#  anything that doesn't look like ``YYYYMMDD-HHMMSS`` (8 digits +
#  dash + 6 digits) is rejected so a stray cell value on the Sheet
#  can't spawn a phantom ``[runs."typo"]`` block in the TOML.
_RUN_ID_STRICT_RE = re.compile(r"^\d{8}-\d{6}$")


def auto_year(repo_root: Path) -> Optional[str]:
    """Pick the newest ``20YY.database.toml`` under ``run-lists/``.

    Kept for callers that genuinely want a single year (CLI ``--year``
    default).  ``build_snapshot`` since Phase B treats *all* discovered
    years as first-class — there's one ``runs (YYYY)`` /
    ``runlists (YYYY)`` / ``qa_results (YYYY)`` worksheet per year on
    the Sheet, mirroring the on-disk layout.
    """
    years = all_years(repo_root)
    return max(years) if years else None


def all_years(repo_root: Path) -> list[str]:
    """Every ``20YY`` for which ``run-lists/20YY.database.toml`` exists.

    Returned sorted (chronological by year, which is also lexical).
    Empty list when no matching file is found — caller's responsibility
    to surface that as a clean "nothing to do" rather than crashing.
    """
    rl = repo_root / "run-lists"
    if not rl.is_dir():
        return []
    years: set[str] = set()
    for p in rl.iterdir():
        m = _YEAR_RE.match(p.name)
        if m:
            years.add(m.group(1))
    return sorted(years)


# ---------------------------------------------------------------------------
# Snapshot — point-in-time view of the local state.
# ---------------------------------------------------------------------------


@dataclass
class RadiatorCatalogEntry:
    """One unique radiator (de-duped across years) + its usage stats.

    ``runs_ranges`` (Phase D.16) is the list of contiguous (first, last)
    run-id ranges this radiator was actually used in.  Replaces the
    earlier single ``first_run``/``last_run`` pair which implied
    continuous usage and lied when the radiator was swapped out and
    back (e.g. ``X`` for ten runs → ``Y`` for two → ``X`` again).
    A single contiguous run gets one range ``[(rid, rid)]``; a
    range of identical first/last endpoints renders compactly as
    just the one run id.
    """

    id: int
    type: str
    refindex: Any
    tag: str
    depth: Any
    side: str
    n_runs: int
    runs_ranges: list[tuple[str, str]] = field(default_factory=list)


@dataclass
class Snapshot:
    """Multi-year point-in-time view of the local state.

    Every ``*_by_year`` map is keyed by the campaign year (``"2025"``,
    ``"2026"``, …) — auto-discovered from the ``run-lists/`` directory.
    The Sheet ends up with one ``runs (YYYY)`` / ``runlists (YYYY)``
    / ``qa_results (YYYY)`` worksheet per year, mirroring the on-disk
    layout so an operator who works on multiple campaigns doesn't
    have to wade through a mixed table.

    ``radiators_rows`` is a single FLAT list across years — the
    derived "one row per ``(year, run_id, idx)`` tuple" view of the
    per-run ``radiators`` arrays.  Going flat (rather than nested
    per-year) lets shifters filter by tag across the whole campaign
    history.  When we ship the radiator-catalog model later this
    worksheet swaps to a pure tag-pool view.

    ``audit_tail`` is also cross-year and capped (default 30) — the
    Sheet shows the most-recent edits across every campaign, not a
    per-year tail.  The full per-year audit files stay on disk
    untouched.

    ``meta`` keys are operator+host telemetry consumed by the
    banner cell on every worksheet's row 1 (no separate ``meta``
    tab since Phase B).
    """

    years: list[str] = field(default_factory=list)
    runs_by_year: dict[str, list[dict]] = field(default_factory=dict)
    runlists_by_year: dict[str, dict[str, dict]] = field(default_factory=dict)
    results_by_year: dict[str, dict] = field(default_factory=dict)
    schema_extras_by_year: dict[str, list[str]] = field(default_factory=dict)
    audit_tail: list[dict] = field(default_factory=list)
    radiators_catalog: list[RadiatorCatalogEntry] = field(default_factory=list)
    #  Maps ``(year, run_id) -> [catalog_id, …]`` so the runs sheets
    #  can show "this run used radiators 1+2" in a single cell.
    radiator_ids_by_run: dict[tuple[str, str], list[int]] = field(default_factory=dict)
    meta: dict = field(default_factory=dict)

    # ---- back-compat shims for callers that knew the single-year shape
    @property
    def year(self) -> Optional[str]:
        """Newest discovered year — kept for back-compat with tools that
        wanted "what's the current campaign?" without iterating."""
        return max(self.years) if self.years else None


#  Canonical column order for the ``runs`` worksheet.  Matches the
#  documented database schema (run-lists/2025.database.toml header
#  comment) so a shifter reading the Sheet sees fields in the same
#  order as the dashboard's runcard.  Fields not in this list slide
#  to the end in alphabetical order, then ``schema_extras`` at the
#  very end so user-added columns are easy to spot.
_CANONICAL_RUN_FIELDS: tuple[str, ...] = (
    "mirror_mm",
    "beam_status",
    "particle",
    "polarity",
    "beam_energy",
    "rdo_firmware",
    "timing_firmware",
    "temperature",
    "v_bias",
    "calibration",
    "dcr_scan",
    "n_spills",
    "radiators",
    "quality",
    "notes",
)


#  Fields we render as a JSON-encoded cell because they're nested
#  structures (lists of dicts, arrays).  Reverse-merge skips these —
#  reconciling structural edits via a single cell is brittle and the
#  Sheet is the wrong tool to edit them in the first place.
#
#  ``radiators`` is in this set defensively even though Phase B moved
#  the field to a dedicated worksheet (see ``_RUNS_EXCLUDED_COLUMNS``)
#  — if a future schema change adds it back to the runs sheet, the
#  reverse-merge stays safe.
_NON_SCALAR_FIELDS: frozenset[str] = frozenset({"radiators"})


#  Fields suppressed from the per-year ``runs (YYYY)`` worksheet
#  because they're rendered in a dedicated tab.  Keep them in the
#  database, keep them in the canonical column list — we just don't
#  emit a column for them on the runs tab.  Operators see this data
#  on the ``radiators`` tab instead, which is queryable + filterable.
_RUNS_EXCLUDED_COLUMNS: frozenset[str] = frozenset({"radiators"})


# ---------------------------------------------------------------------------
# Human-readable display names.
#
# Database field keys (snake_case) drive the TOML schema; the Sheet
# shows them with their display name in the header row.  Two-way
# mapping so the reverse-merge can recover the storage key from a
# column header before calling ``rundb.update_run_field``.
#
# Add new fields here when the schema gains one; missing entries fall
# back to the raw field name (``display() → str``), so an unmapped
# column still renders, just unprettied.
# ---------------------------------------------------------------------------


FIELD_DISPLAY: dict[str, str] = {
    #  Runs columns
    "run_id":             "Run ID",
    "mirror_mm":          "Mirror A (mm)",   # aerogel mirror
    "mirror_mm_gas":      "Mirror G (mm)",   # gas mirror
    #  Phase D.7: drop redundant group prefixes from column names
    #  since the group band above already says "Beam".  ``Status`` /
    #  ``Energy (GeV)`` read clean once visually nested.
    "beam_status":        "Status",
    "particle":           "Particle",
    "polarity":           "Polarity",
    "collimators":        "Collimators",
    "beam_energy":        "Energy (GeV)",
    "trigger":            "Trigger",
    "rdo_firmware":       "RDO firmware",
    "timing_firmware":    "Timing firmware",
    "temperature":        "Temp (°C)",
    "v_bias":             "V_bias (V)",
    "calibration":        "Calibration run",
    "dcr_scan":           "DCR scan run",
    "n_spills":           "N spills",
    "gem":                "GEM",
    "altai":              "ALTAI",
    "available_triggers": "Triggers",
    "quality":            "Quality",
    "notes":              "Notes",
    "radiator_ids":       "Radiator IDs",
    "radiator_desc":      "Radiator description",
    "radiator_gas":       "Radiator gas",
    "deltathr":           "DThr",
    "opmode":             "Op",
    #  Radiators catalog
    "id":         "ID",
    "type":       "Type",
    "refindex":   "Refractive index",
    "tag":        "Tag",
    "depth":      "Depth (cm)",
    "side":       "Side",
    "n_runs":      "N runs",
    "runs_ranges": "Runs ranges",
    "first_run":   "First run",   # legacy fallback
    "last_run":    "Last run",    # legacy fallback
    #  Audit
    "at":         "At",
    "source":     "Source",
    "run":        "Run",
    "field":      "Field",
    "old_value":  "Old value",
    "new_value":  "New value",
    #  Runlists (row labels for the vertical layout)
    "name":       "Name",
    "campaign":   "Campaign",
    "count":      "N runs",
}


_DISPLAY_TO_FIELD: dict[str, str] = {v: k for k, v in FIELD_DISPLAY.items()}


def display(field: str) -> str:
    """Convert a snake_case storage key into its operator-facing label.

    Unmapped fields fall through verbatim so a freshly-added column
    still appears on the Sheet without crashing.  When that happens
    add an entry to ``FIELD_DISPLAY`` (and the reverse-map will
    self-update at module import time).
    """
    return FIELD_DISPLAY.get(field, field)


def field_of(label: str) -> str:
    """Reverse-map an operator-facing label back to its storage key.

    Falls through verbatim when the label doesn't match a known
    display name — happens when the column was unmapped (see
    ``display`` above), so the column name IS the field name.
    """
    return _DISPLAY_TO_FIELD.get(label, label)


# ---------------------------------------------------------------------------
# Per-field value transforms — for cells whose stored form differs
# from how the operator wants to see them on the Sheet.
#
# Today only ``beam_status`` is normalised (lowercase ``on``/``off``
# in the database → uppercase ``ON``/``OFF`` for the dropdown +
# colour-coding).  Add transforms here if more fields gain similar
# polish requirements.
# ---------------------------------------------------------------------------


def display_value(field: str, value: Any) -> Any:
    """Transform a stored value for visual display.

    The reverse-merge inverts this via ``storage_value`` so an
    operator picking ``"ON"`` (or ``"Pos"``) from the dropdown lands
    back in the database as the canonical lowercase long form.
    """
    if field == "beam_status":
        s = str(value or "").strip().lower()
        if s == "on":  return "ON"
        if s == "off": return "OFF"
    if field == "polarity":
        s = str(value or "").strip().lower()
        if s == "positive": return "Pos"
        if s == "negative": return "Neg"
    return value


def storage_value(field: str, value: Any) -> Any:
    """Inverse of ``display_value`` — normalise sheet-side back to TOML form."""
    if field == "beam_status":
        s = str(value or "").strip().upper()
        if s == "ON":  return "on"
        if s == "OFF": return "off"
    if field == "polarity":
        s = str(value or "").strip()
        if s.lower() == "pos": return "positive"
        if s.lower() == "neg": return "negative"
    return value


def build_snapshot(
    repo_root: Path,
    *,
    year: Optional[str] = None,
    audit_tail_n: int = 30,
    operator_tag: str = "",
    app_version: str = "",
    last_push_at: str = "",
) -> Snapshot:
    """Read every campaign year's state into a ``Snapshot`` for rendering.

    Inputs (all under ``repo_root``):

      - ``run-lists/<year>.database.toml``        → merged ``RunRecord`` list per year
      - ``run-lists/<year>.runlists.toml``        → named selections + campaign tag per year
      - ``run-lists/<year>.database.audit.toml``  → audit entries per year (concatenated, tailed cross-year)
      - ``<repo_root>/standard_results.toml``     → derived QA values, split by run-id year prefix

    The ``year`` parameter accepts:

      - ``None`` (default) — discover and snapshot every year present
      - ``"<year>"``        — single-year snapshot (legacy callers /
                              CLI ``--year 2025``)

    ``audit_tail_n`` bounds the *cross-year* audit tail that's
    pushed.  Default dropped from 100 → 30 per the Phase B design
    review.  Local audit files keep the full history regardless.
    """
    #  Lazy import so the snapshot module stays Qt-free + cycle-free.
    from . import rundb

    if year is None:
        years = all_years(repo_root)
    else:
        years = [year] if (repo_root / "run-lists" / f"{year}.database.toml").is_file() else []

    runs_by_year: dict[str, list[dict]] = {}
    runlists_by_year: dict[str, dict[str, dict]] = {}
    schema_extras_by_year: dict[str, list[str]] = {}
    results_by_year: dict[str, dict] = {}

    rl = repo_root / "run-lists"
    for y in years:
        db_path = rl / f"{y}.database.toml"
        if not db_path.is_file():
            continue
        records = rundb.load_database(db_path)
        runs: list[dict] = []
        for rec in records:
            row = {"run_id": rec.run_id}
            #  Merged view: forward-inheritance flattened so the
            #  Sheet shows what the dashboard would render in the
            #  runcard, not just the run's own overrides.
            row.update(rec.merged)
            runs.append(row)
        runs_by_year[y] = runs
        schema_extras_by_year[y] = rundb.read_schema_extras(db_path)

        rls_path = rl / f"{y}.runlists.toml"
        runlists_by_year[y] = rundb.read_runlists_meta(rls_path)

    #  Cross-year audit tail.  Concatenate every year's entries,
    #  sort by ``at`` descending (newest first), tail at N.  Audit
    #  entries are tiny dicts; even with 5+ years of history the
    #  concatenated list is well under any meaningful memory bound.
    audit_pool: list[dict] = []
    for y in years:
        audit_path = rl / f"{y}.database.audit.toml"
        if audit_path.is_file():
            audit_pool.extend(_read_audit_tail(audit_path, tail_n=10_000))
    audit_pool.sort(key=lambda e: e.get("at") or "", reverse=True)
    audit_tail = audit_pool[:audit_tail_n]

    #  Derived QA results.  ``standard_results.toml`` is repo-wide;
    #  split by the year prefix of each run id so each year's qa_results
    #  worksheet only carries its own data.
    results_all = rundb.results_load(repo_root / "standard_results.toml")
    for run_id, payload in results_all.items():
        y = run_id[:4] if len(run_id) >= 4 and run_id[:4].isdigit() else ""
        if y in years:
            results_by_year.setdefault(y, {})[run_id] = payload

    radiators_catalog, radiator_ids_by_run = _build_radiators_catalog(
        runs_by_year,
    )

    meta = {
        "operator_tag": resolve_operator_tag(operator_tag),
        "hostname": socket.gethostname() or "?",
        "app_version": app_version or "qa_quicklook",
        "years": ", ".join(years),
        "taken_at": _iso_now(),
        #  Caller passes the predicted push timestamp so the banner
        #  row 1 carries it on first render — saves a second
        #  batchUpdate to patch banners after the values push.
        "last_push_at": last_push_at,
    }

    return Snapshot(
        years=years,
        runs_by_year=runs_by_year,
        runlists_by_year=runlists_by_year,
        results_by_year=results_by_year,
        schema_extras_by_year=schema_extras_by_year,
        audit_tail=audit_tail,
        radiators_catalog=radiators_catalog,
        radiator_ids_by_run=radiator_ids_by_run,
        meta=meta,
    )


def _radiator_key(rad: dict) -> tuple:
    """Stable identity key for a radiator instance.

    Tuple of every distinguishing field — two radiators that match on
    all of these are the same physical piece.  Refindex and depth
    can be int/float/str, so we stringify for a consistent hash
    across the cross-year merge.
    """
    return (
        str(rad.get("type", "")),
        str(rad.get("refindex", "")),
        str(rad.get("tag", "")),
        str(rad.get("depth", "")),
        str(rad.get("side", "")),
    )


def _build_radiators_catalog(
    runs_by_year: dict[str, list[dict]],
) -> tuple[list[RadiatorCatalogEntry], dict[tuple[str, str], list[int]]]:
    """De-duplicate radiators across runs + collect usage stats.

    Two outputs in one walk (efficient + keeps catalog ids stable):

      1. **Catalog** — one ``RadiatorCatalogEntry`` per unique
         radiator key, with ``n_runs`` / ``first_run`` / ``last_run``
         summarising usage.  Catalog ids start at 1 (so the runs
         sheet can show ``"1, 2"`` without zero-confusion).  Order
         is "first seen", so a stable id survives across pushes
         as long as the database doesn't gain a run earlier than
         the catalog entry's first user.
      2. **Per-run reference map** — ``(year, run_id) → [catalog_id, …]``.
         The runs sheet looks up its own list with one ``.get()``.

    Replaces the old flat ``[(year, run_id, idx, type, …)]`` view
    that copied every radiator's physical fields onto every run that
    used it.  The "drich aerogel 1.021 AG22-J001" combo went from
    520 redundant rows to one catalog entry + 250 per-run references.
    """
    by_key: dict[tuple, RadiatorCatalogEntry] = {}
    ref_map: dict[tuple[str, str], list[int]] = {}
    next_id = 1
    #  Track which radiators were used on the IMMEDIATELY preceding
    #  run (cross-year — radiators don't reset between campaigns).
    #  Used to decide whether to extend the open range or start a new
    #  one when this run reuses a radiator that was off for a while.
    prev_run_keys: set[tuple] = set()
    #  Flatten + chronologically sort across years so the range walk
    #  sees a single timeline.  Lexical sort works since run ids are
    #  zero-padded ``YYYYMMDD-HHMMSS`` timestamps.
    flat: list[tuple[str, dict]] = []
    for year, runs in runs_by_year.items():
        for r in runs:
            flat.append((year, r))
    flat.sort(key=lambda t: t[1].get("run_id", ""))

    for year, r in flat:
        radiators = r.get("radiators") or []
        if not isinstance(radiators, list):
            prev_run_keys = set()
            continue
        run_id = r["run_id"]
        ids_for_run: list[int] = []
        current_keys: set[tuple] = set()
        for rad in radiators:
            if not isinstance(rad, dict):
                continue
            key = _radiator_key(rad)
            current_keys.add(key)
            entry = by_key.get(key)
            if entry is None:
                entry = RadiatorCatalogEntry(
                    id=next_id,
                    type=str(rad.get("type", "")),
                    refindex=rad.get("refindex", ""),
                    tag=str(rad.get("tag", "")),
                    depth=rad.get("depth", ""),
                    side=str(rad.get("side", "")),
                    n_runs=0,
                )
                by_key[key] = entry
                next_id += 1
            entry.n_runs += 1
            #  Range tracking: extend the most recent (first, last)
            #  if this radiator was ALSO used on the immediately
            #  preceding run; otherwise open a new range.  Captures
            #  swap-and-swap-back as two separate ranges rather than
            #  a single misleading first..last span.
            if entry.runs_ranges and key in prev_run_keys:
                first, _last = entry.runs_ranges[-1]
                entry.runs_ranges[-1] = (first, run_id)
            else:
                entry.runs_ranges.append((run_id, run_id))
            ids_for_run.append(entry.id)
        prev_run_keys = current_keys
        if ids_for_run:
            ref_map[(year, run_id)] = ids_for_run
    catalog = sorted(by_key.values(), key=lambda e: e.id)
    return catalog, ref_map


# ---------------------------------------------------------------------------
# Source-file readers — kept narrow so each one is unit-testable.
# ---------------------------------------------------------------------------


def _read_audit_tail(path: Path, *, tail_n: int) -> list[dict]:
    """Return the last ``tail_n`` ``[[entry]]`` blocks, newest first."""
    if not path.is_file():
        return []
    try:
        with path.open("rb") as fh:
            doc = _tomllib.load(fh)
    except (OSError, _tomllib.TOMLDecodeError):
        return []
    entries = doc.get("entry")
    if not isinstance(entries, list):
        return []
    #  ``rundb._append_audit_entry`` appends chronologically so the
    #  file is naturally oldest-first.  Reverse to surface "what
    #  changed recently" at the top of the worksheet.
    rev = list(reversed(entries))
    return [
        {
            "at": str(e.get("at", "")),
            "source": str(e.get("source", "")),
            "run": str(e.get("run", "")),
            "field": str(e.get("field", "")),
            "old_value": e.get("old_value", ""),
            "new_value": e.get("new_value", ""),
        }
        for e in rev[:tail_n]
        if isinstance(e, dict)
    ]


# ---------------------------------------------------------------------------
# Worksheet rendering — Snapshot → {sheet_name: [[row], ...]}
#
# Phase B layout (one worksheet per campaign year for the data-taking
# tables; one cross-year tab for audit + radiators):
#
#   runs (YYYY)      — one row per run, canonical field columns
#   runlists (YYYY)  — VERTICAL: one column per runlist, with name /
#                       campaign / count metadata rows on top, then a
#                       block of run-id rows beneath
#   qa_results (YYYY)— derived QA values; (run_id, sensor, quantity, value, error)
#                       row 1 banner makes the "derived from writers"
#                       provenance explicit so operators don't confuse
#                       these with data-taking metadata
#   radiators        — flat: (year, run_id, idx, type, refindex, tag, depth, side)
#   audit            — cross-year tail (default 30 rows), newest first
#
# Every worksheet's row 1 is a banner ("Last push: 2026-05-29 …")
# and row 2 is the column header.  ``frozen_rows = 2`` keeps both
# visible while scrolling, and the diff-aware push handles the
# banner-changes-every-tick cheaply.
# ---------------------------------------------------------------------------


def render_worksheets(snap: Snapshot) -> dict[str, list[list[Any]]]:
    """Turn a ``Snapshot`` into the cell arrays the Sheets API consumes.

    Worksheet ORDER (preserved by ``dict`` insertion order) is the
    visual tab order on the Sheet: data-taking first (runs YYYY, then
    runlists YYYY, then qa_results YYYY), then the cross-year
    radiators + audit tables.  Each year's three primary tabs are
    grouped together so a shifter on 2026 doesn't have to hop past
    every 2025 tab to find theirs.
    """
    out: dict[str, list[list[Any]]] = {}
    last_push_at = snap.meta.get("last_push_at", "")
    operator = snap.meta.get("operator_tag", "")

    #  Reverse-chronological tab order — newest campaign first so the
    #  active year is the landing page when the operator opens the
    #  Sheet.  ``snap.years`` is sorted ascending; ``reversed`` flips
    #  that.  Insertion order into ``out`` is the tab order on the
    #  Sheet (Phase C ``sheets_format_requests`` writes per-tab
    #  ``index`` values that match this ordering, so existing tabs get
    #  shuffled into place on the next push too).
    for year in reversed(snap.years):
        out[f"Runs ({year})"] = _render_runs_year(
            snap, year,
            banner=_banner(last_push_at, operator, note=None),
        )
        #  QA results: per the design call on 2026-05-29, derived QA
        #  values belong as COLUMNS on the runs tab (one per quantity),
        #  not as a separate worksheet — they're per-run, not per-
        #  entity.  Today the repo has no ``standard_results.toml`` so
        #  there's nothing to render; when QA data lands we'll pivot
        #  it into ``Qa_<quantity>`` columns on ``Runs (YYYY)`` rather
        #  than reviving the standalone tab.

    #  Cross-year Runlists — single tab sorted newest-campaign-first
    #  (year descending, then alphabetical by name within each year).
    #  Phase D.4 design call: operators care more about "what runlists
    #  are active right now" than per-year segregation.  The Year row
    #  in the metadata stack makes the campaign provenance explicit.
    out["Runlists"] = _render_all_runlists(
        snap,
        banner=_banner(last_push_at, operator,
                       note="All runlists, newest campaign first"),
    )

    #  Cross-year auxiliary tabs land at the end of the strip.
    out["Radiators"] = _render_radiators(
        snap,
        banner=_banner(last_push_at, operator,
                       note=("De-duplicated catalog — runs reference these by "
                             "id in their `radiator_ids` column")),
    )
    out["Audit"] = _render_audit(
        snap,
        banner=_banner(last_push_at, operator,
                       note=f"Cross-year tail ({len(snap.audit_tail)} most-recent edits)"),
    )
    return out


def _banner(last_push_at: str, operator_tag: str, *, note: Optional[str]) -> str:
    """Build the row-1 banner string.

    Format: ``Last push: <iso> — <operator> [· <note>]``.  Stays
    under ~120 chars so it doesn't visually overflow more than a
    few columns at default width.  ``last_push_at == ""`` on dry-run
    so the banner reads ``Last push: (pending)`` instead of a stale
    timestamp.
    """
    when = last_push_at or "(pending)"
    op = operator_tag or "?"
    base = f"Last push: {when} — {op}"
    return f"{base} · {note}" if note else base


def _pad_row(banner: str, ncols: int) -> list[Any]:
    """Render the banner row with ``ncols`` cells (banner in A1, blanks elsewhere).

    Padding the row matters for the diff-aware push: ``rows == old_rows``
    only when the row lengths match exactly, so an unpadded banner
    would falsely diff against a previously-padded one and force
    a needless update.
    """
    return [banner] + [""] * max(0, ncols - 1)


# ----- runs ---------------------------------------------------------


def _runs_columns(snap: Snapshot, year: str) -> list[str]:
    """Per-year column order for the ``runs`` worksheet.

    Phase D.15.1 — **same format everywhere, with backward compat.**

    Every field declared in ``COLUMN_GROUPS`` appears on every
    ``Runs (YYYY)`` tab, regardless of whether this year's data
    actually carries it.  Runs that predate a column show as
    blank cells in that column — that's the documented "new column
    = null for historical runs" rule.  This guarantees the column
    layout (and the group bands above it) is identical across
    years, which Sheet-side operators can rely on for cross-year
    filters / formulas / pivot tables.

    Deprecated fields (present in some past year's data but never
    catalogued in ``COLUMN_GROUPS``) get tail-appended per year so
    historical observations aren't lost.  They show up unattributed —
    a visual cue that they were removed from the schema and either
    need readding to a group or formally retiring.

    ``schema_extras`` (per-year fields registered via
    ``rundb.add_schema_extra`` from the dashboard) that don't yet
    appear in ``COLUMN_GROUPS`` also tail-append per year, so the
    dashboard's "+Field" button works end-to-end even before the
    field gets a permanent group home in code.
    """
    runs = snap.runs_by_year.get(year, [])
    extras = snap.schema_extras_by_year.get(year, [])
    seen: set[str] = {"run_id"}
    for row in runs:
        seen.update(
            k for k in row.keys()
            if k not in _RUNS_EXCLUDED_COLUMNS
        )
    extras_set = set(extras)
    cols: list[str] = []
    used: set[str] = set()
    #  Walk in group order.  Phase D.15.1 — include every documented
    #  group field unconditionally; backward-compat blanks are the
    #  point.  Excluded fields (today: ``radiators``, which has its
    #  own tab) still get filtered.
    for _group_name, group_cols in COLUMN_GROUPS:
        for f in group_cols:
            if f in _RUNS_EXCLUDED_COLUMNS:
                continue
            cols.append(f)
            used.add(f)
    #  Deprecated / unattributed data fields (present in this year's
    #  runs, missing from COLUMN_GROUPS): tail-append per year so the
    #  data stays visible.
    other = sorted(c for c in seen - used - extras_set)
    cols.extend(other)
    #  Schema-extras (operator-registered fields) not yet catalogued
    #  in COLUMN_GROUPS: also tail-append.  When the field gets a
    #  group, this branch becomes a no-op for it.
    for x in extras:
        if x not in used and x not in _RUNS_EXCLUDED_COLUMNS:
            cols.append(x)
            used.add(x)
    return cols


def _runs_group_row(cols: list[str]) -> list[str]:
    """Phase D.5 group-band row — one cell per column.

    The cell holds the group label for the start-of-group column and
    an empty string for every continuation column.  The actual visual
    merge is handled by the formatting pass (``sheets_format_requests``
    emits ``mergeCells`` requests; the xlsx writer calls
    ``ws.merge_cells``).  Storing the label only in the first cell
    keeps the diff-aware push cheap (a banded change in the group row
    only re-emits a single cell, not the whole row).
    """
    out: list[str] = []
    prev_group = None
    for c in cols:
        g = _FIELD_TO_GROUP.get(c, "")
        if g and g != prev_group:
            out.append(g)
        else:
            out.append("")
        prev_group = g
    return out


#  Synthetic column added to ``runs (YYYY)`` since the radiators
#  catalog refactor.  Holds the comma-joined catalog ids for the run
#  (e.g. ``"1, 2"`` for a two-radiator stack).  Operators cross-
#  reference into the ``radiators`` worksheet for physical fields.
_RADIATOR_IDS_COLUMN = "radiator_ids"


#  Columns that are computed at render time rather than read from the
#  database.  They always appear on the ``Runs (YYYY)`` worksheet
#  regardless of whether any run mentions them, so the group-band row
#  + the formatter's column-width spec can rely on a stable position.
_RUNS_SYNTHETIC_COLUMNS: frozenset[str] = frozenset({_RADIATOR_IDS_COLUMN})


# ---------------------------------------------------------------------------
# Column groupings for the runs worksheets — operator-facing taxonomy
# from the 2026-05-29 design call.
#
# Rendered as a "group band" row above the column header (row 1 in the
# Phase D.5 layout: banner / group / header / data ...).  Empty group
# label = column stays ungrouped (only ``Run ID`` today).  Order of
# groups + columns within each group is preserved verbatim — that's
# the visual order on the Sheet.
#
# Add new fields here when the database schema gains one; missing
# fields fall through to the catch-all "Others" trailing block via
# the dedicated extras path in ``_runs_columns``.
# ---------------------------------------------------------------------------


COLUMN_GROUPS: list[tuple[str, list[str]]] = [
    #  Phase D.9: "Run" group is the always-visible identity block —
    #  Run ID + the two summary fields shifters look at most often
    #  (N spills + Quality).  Pinned via ``frozen_cols`` so they stay
    #  on screen when horizontally scrolling through the wide
    #  Detector + Electronics + Calibration columns.
    ("Run",         ["run_id", "n_spills", "quality"]),
    ("Beam",        ["beam_status", "particle", "polarity",
                     "beam_energy", "collimators"]),
    ("Detector",    ["mirror_mm", "mirror_mm_gas",
                     "radiator_ids", "radiator_desc", "radiator_gas",
                     "temperature", "v_bias"]),
    ("Tracking",    ["gem", "altai"]),
    ("Electronics", ["rdo_firmware", "timing_firmware",
                     "trigger", "available_triggers",
                     "deltathr", "opmode"]),
    ("Calibration", ["calibration", "dcr_scan"]),
    ("Others",      ["notes"]),
]


_FIELD_TO_GROUP: dict[str, str] = {
    f: g for g, fields in COLUMN_GROUPS for f in fields
}


def _render_runs_year(snap: Snapshot, year: str, *, banner: str) -> list[list[Any]]:
    cols = _runs_columns(snap, year)
    #  Phase D.5 layout: banner / group row / column header / data.
    #  Header row uses the operator-facing display names; the storage
    #  keys live only inside ``cols``.  ``runs_cell_index`` reverses
    #  the display → field mapping when reading back from the Sheet
    #  so reverse-merge still writes the right TOML key.
    display_header = [display(c) for c in cols]
    group_row = _runs_group_row(cols)
    rows: list[list[Any]] = [
        _pad_row(banner, len(cols)),
        group_row,
        display_header,
    ]
    for row in snap.runs_by_year.get(year, []):
        out: list[Any] = []
        for c in cols:
            if c == _RADIATOR_IDS_COLUMN:
                ids = snap.radiator_ids_by_run.get((year, row.get("run_id", "")), [])
                out.append(", ".join(str(i) for i in ids))
            else:
                #  ``display_value`` normalises per-field for visual
                #  polish (e.g. ``"on"`` → ``"ON"``); ``_cell`` handles
                #  type coercion + JSON encoding for nested structures.
                out.append(display_value(c, _cell(row.get(c, ""), col_name=c)))
        rows.append(out)
    return rows


# ----- runlists (vertical layout) -----------------------------------


def _runlists_row_label(field: str) -> str:
    """Human-readable label for a runlists row in the vertical layout."""
    return display(field)


def _render_all_runlists(snap: Snapshot, *, banner: str) -> list[list[Any]]:
    """Single cross-year runlists tab, vertical layout, newest-first.

    Each runlist becomes one column.  Sort key: ``(-year, name)`` so
    2026 runlists land left-most and 2023 right-most; within a year
    runlists are alphabetical.  The metadata stack carries four rows:

        Row 2: Name      — runlist key (e.g. ``aerogel_mirror_scan``)
        Row 3: Year      — derived from the runlists.toml filename
        Row 4: Campaign  — explicit ``campaign = "..."`` from the TOML
        Row 5: N runs    — convenience count of the run id list
        Rows 6+: run ids — padded to ``max_run_count`` for rect shape

    Frozen at row 6 so the metadata stack stays visible while
    scrolling through long campaigns.  See FORMATS["runlists"]
    for the actual frozen_rows value.
    """
    #  Flatten years × runlists, keyed by ``(year, name)``.  Sort so
    #  newer years come first; within a year, alphabetical names.
    triples: list[tuple[str, str, dict]] = []
    for year, runlists in snap.runlists_by_year.items():
        for name, entry in runlists.items():
            triples.append((year, name, entry))
    triples.sort(key=lambda t: (-int(t[0]), t[1]))

    if not triples:
        return [[banner], [_runlists_row_label("name")]]

    ncols = 1 + len(triples)
    max_run_count = max(len(t[2]["runs"]) for t in triples)

    rows: list[list[Any]] = [_pad_row(banner, ncols)]
    rows.append([_runlists_row_label("name")]     + [t[1] for t in triples])
    rows.append(["Year"]                          + [t[0] for t in triples])
    rows.append([_runlists_row_label("campaign")] + [t[2]["campaign"] for t in triples])
    rows.append([_runlists_row_label("count")]    + [len(t[2]["runs"]) for t in triples])
    for i in range(max_run_count):
        row: list[Any] = [""]
        for _, _, entry in triples:
            ids = entry["runs"]
            row.append(ids[i] if i < len(ids) else "")
        rows.append(row)
    return rows


# ----- qa_results ---------------------------------------------------


def _render_qa_results_year(snap: Snapshot, year: str, *, banner: str) -> list[list[Any]]:
    """Derived QA values from ``standard_results.toml`` for one year.

    Wide-table shape: one row per ``(run_id, sensor, quantity)``.
    Pivoting per-quantity into columns would explode the column count
    (~20+ quantities × 8 sensors) and the schema isn't stable enough
    to bake in.  Long-form lets shifters filter / pivot in Sheets
    themselves.
    """
    cols = ["run_id", "sensor", "quantity", "value", "error"]
    rows: list[list[Any]] = [_pad_row(banner, len(cols)), list(cols)]
    payload = snap.results_by_year.get(year, {})
    for run_id in sorted(payload.keys()):
        sensors = payload[run_id]
        for sensor in sorted(sensors.keys()):
            qmap = sensors[sensor]
            for quantity in sorted(qmap.keys()):
                entry = qmap[quantity]
                rows.append([run_id, sensor, quantity, entry.value, entry.error])
    return rows


# ----- radiators ----------------------------------------------------


def _format_runs_ranges(ranges: list[tuple[str, str]]) -> str:
    """Render a list of contiguous (first, last) ranges to one cell.

    Single-run ranges collapse to just the run id (``"R1"`` rather
    than ``"R1..R1"``); multi-run ranges use ``..`` so the dotted
    notation reads at a glance.  Joined with ``, `` so operators can
    spot discontinuities — a catalog row with three commas had two
    swap-back events for that radiator.
    """
    parts: list[str] = []
    for first, last in ranges:
        parts.append(first if first == last else f"{first}..{last}")
    return ", ".join(parts)


def _render_radiators(snap: Snapshot, *, banner: str) -> list[list[Any]]:
    """Render the de-duplicated radiator catalog.

    One row per unique radiator (matched on
    ``(type, refindex, tag, depth, side)``).  Phase D.16: the
    usage column now carries the full list of contiguous
    ``first..last`` ranges, so a swap-and-swap-back pattern reads
    correctly as two ranges instead of one misleading span.
    """
    cols = ["id", "type", "refindex", "tag", "depth", "side",
            "n_runs", "runs_ranges"]
    rows: list[list[Any]] = [_pad_row(banner, len(cols)),
                             [display(c) for c in cols]]
    for entry in snap.radiators_catalog:
        rows.append([
            entry.id, entry.type, entry.refindex, entry.tag, entry.depth,
            entry.side, entry.n_runs,
            _format_runs_ranges(entry.runs_ranges),
        ])
    return rows


# ----- audit --------------------------------------------------------


def _render_audit(snap: Snapshot, *, banner: str) -> list[list[Any]]:
    cols = ["at", "source", "run", "field", "old_value", "new_value"]
    rows: list[list[Any]] = [_pad_row(banner, len(cols)),
                             [display(c) for c in cols]]
    for e in snap.audit_tail:
        rows.append([_cell(e.get(c, ""), col_name=c) for c in cols])
    return rows


def _cell(value: Any, *, col_name: str = "") -> Any:
    """Coerce a Python value into a Sheets-cell-safe scalar.

    ``None`` → ``""`` (Sheets has no null).  Lists / dicts get
    JSON-encoded so the structure round-trips visibly — readable for
    operators, parseable for tooling, and unambiguous when the
    reverse-merge skips them (see ``_NON_SCALAR_FIELDS``).  Scalars
    pass through unchanged so types are preserved (int / float /
    bool) — the Sheets API accepts those natively.
    """
    if value is None:
        return ""
    if isinstance(value, (list, dict)):
        try:
            return json.dumps(value, ensure_ascii=False, sort_keys=True)
        except (TypeError, ValueError):
            return repr(value)
    return value


# ---------------------------------------------------------------------------
# Cell-index helpers — the reverse-merge's per-(run, field) view.
# ---------------------------------------------------------------------------


def runs_cell_index(rendered_runs: list[list[Any]]) -> dict[tuple[str, str], Any]:
    """Map ``(run_id, column)`` → cell for a ``runs (YYYY)`` worksheet.

    Skips the Phase B banner row (row 0 — single non-blank "Last
    push: ..." cell), the header row, and the ``run_id`` column
    itself (it's the key, not a payload cell).  Skips
    ``_NON_SCALAR_FIELDS`` since the reverse-merge can't safely
    round-trip them.  Used both on the just-rendered local state
    and on the freshly-pulled Sheet state so the diff is apples-to-
    apples.
    """
    if not rendered_runs:
        return {}
    #  Banner detection: row 0 is the banner iff its first cell is a
    #  string that starts with "Last push:".  Skip it.  Defensive
    #  against the rare case where a worksheet pre-Phase-B is
    #  somehow fed in (no banner) — then we don't skip.
    rows = rendered_runs
    if (
        rows and rows[0] and isinstance(rows[0][0], str)
        and rows[0][0].startswith("Last push:")
    ):
        rows = rows[1:]
    #  Phase D.5: optional group-band row sits between the banner and
    #  the column header.  Heuristic: if the row immediately AFTER
    #  the current candidate has a recognisable ``Run ID`` cell at
    #  index 0, the current row is the group band — skip it.
    if len(rows) >= 2:
        next_first = field_of(str(rows[1][0])) if rows[1] else ""
        if next_first == "run_id":
            rows = rows[1:]
    if not rows:
        return {}
    header_storage = [field_of(str(c)) for c in rows[0]]
    try:
        id_idx = header_storage.index("run_id")
    except ValueError:
        return {}
    out: dict[tuple[str, str], Any] = {}
    for row in rows[1:]:
        if id_idx >= len(row):
            continue
        run_id = str(row[id_idx])
        if not run_id:
            continue
        for i, col in enumerate(header_storage):
            if col == "run_id" or col in _NON_SCALAR_FIELDS:
                continue
            if i >= len(row):
                continue
            out[(run_id, col)] = storage_value(col, row[i])
    return out


# ---------------------------------------------------------------------------
# Snapshot persistence — what we last pushed.
# ---------------------------------------------------------------------------


def save_last_pushed(
    rendered: dict[str, list[list[Any]]],
    *,
    path: Path = SNAPSHOT_PATH,
) -> None:
    """Persist the rendered worksheet payload as the next reverse-merge
    baseline.

    Atomic write via ``write_text`` to a sibling ``.tmp`` + ``os.replace``
    so a half-written snapshot is never observable to a concurrent
    pull.  Creates the cache dir on the first call so an operator with
    a fresh ``~/.cache`` doesn't trip over a missing-dir error.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "saved_at": _iso_now(),
        "worksheets": rendered,
    }
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, ensure_ascii=False, default=str))
    os.replace(tmp, path)


def load_last_pushed(
    *,
    path: Path = SNAPSHOT_PATH,
) -> dict[str, list[list[Any]]]:
    """Read the last-pushed snapshot.

    Returns an empty dict on first run / missing file / unparseable
    file — the reverse-merge treats that as "no baseline yet, so
    every Sheet cell looks like a Sheet edit on the next pull" and
    the caller's policy decides whether to drop or apply.  In
    practice the worker writes a fresh snapshot at startup-with-no-
    edits to avoid that scenario entirely.
    """
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return {}
    if not isinstance(data, dict):
        return {}
    ws = data.get("worksheets")
    if not isinstance(ws, dict):
        return {}
    return {str(k): list(v) for k, v in ws.items() if isinstance(v, list)}


# ---------------------------------------------------------------------------
# Reverse-merge — diff Sheet vs snapshot, replay edits through rundb.
# ---------------------------------------------------------------------------


@dataclass
class SheetEdit:
    """One detected Sheet-side cell edit, ready to replay or report."""

    run_id: str
    field: str
    snapshot_value: Any
    local_value: Any
    sheet_value: Any
    action: str   # one of: "apply", "skip_already_in_sync", "skip_non_scalar"


def detect_reverse_edits(
    sheet_runs: list[list[Any]],
    local_runs_rendered: list[list[Any]],
    snapshot_runs: list[list[Any]],
) -> list[SheetEdit]:
    """Compare the Sheet's ``runs`` worksheet against the snapshot.

    Returns one ``SheetEdit`` per ``(run_id, field)`` cell where the
    Sheet differs from the snapshot.  ``action`` carries the policy
    decision:

      - ``"apply"``  — Sheet differs from snapshot **and** from the
        current local TOML.  This is a real Sheet-side edit that the
        caller should replay via ``rundb.update_run_field``.
      - ``"skip_already_in_sync"`` — Sheet differs from snapshot but
        matches the current local TOML (a dashboard edit landed
        since the last push; snapshot was stale).  Nothing to do.
      - ``"skip_non_scalar"`` — included for completeness, but
        ``runs_cell_index`` filters non-scalar columns out before
        this gets called.

    The function is purely deterministic — no I/O, no rundb call.
    The caller (``apply_reverse_edits``) owns the actual mutations.
    """
    sheet_cells = runs_cell_index(sheet_runs)
    local_cells = runs_cell_index(local_runs_rendered)
    snap_cells = runs_cell_index(snapshot_runs)

    edits: list[SheetEdit] = []
    for key, sheet_val in sheet_cells.items():
        snap_val = snap_cells.get(key, "")
        if _cells_equal(sheet_val, snap_val):
            continue  # no Sheet-side change since last push
        local_val = local_cells.get(key, "")
        if _cells_equal(sheet_val, local_val):
            #  Stale snapshot; the dashboard already landed this value.
            edits.append(SheetEdit(
                run_id=key[0], field=key[1],
                snapshot_value=snap_val, local_value=local_val,
                sheet_value=sheet_val,
                action="skip_already_in_sync",
            ))
            continue
        edits.append(SheetEdit(
            run_id=key[0], field=key[1],
            snapshot_value=snap_val, local_value=local_val,
            sheet_value=sheet_val,
            action="apply",
        ))
    return edits


def _cells_equal(a: Any, b: Any) -> bool:
    """Compare two cell values tolerantly of cross-side type drift.

    Even with ``valueRenderOption=UNFORMATTED_VALUE`` on the pull,
    types still drift across the boundary:

      - Integers come back as int; floats as float.  Local TOML may
        store ``45.0`` while the Sheet reads ``45`` (int) — different
        types, same number.
      - String-typed fields (notes, quality) round-trip cleanly.
      - Bool is rare in the run database but if it shows up the
        canonical-string compare handles it.

    Strategy: canonical-string compare first (catches strings + the
    bool case + most numeric matches).  When that fails AND both
    sides parse as numbers, fall back to numeric equality.  This is
    the fix for the 2026-05-29 incident where a single locale
    mismatch (Sheets returning ``"51,5"`` instead of ``51.5``)
    triggered a 260-row reverse-merge that corrupted the database.
    Now even if the locale path slipped past, the numeric fallback
    catches it.
    """
    if a is None:
        a = ""
    if b is None:
        b = ""
    if _canonical(a) == _canonical(b):
        return True
    fa = _try_float(a)
    fb = _try_float(b)
    if fa is not None and fb is not None:
        return fa == fb
    return False


def _try_float(v: Any) -> Optional[float]:
    """Parse ``v`` as a float, returning ``None`` when ambiguous.

    Treats ``bool`` as non-numeric on purpose — ``True == 1.0`` is
    a Python quirk we don't want propagating into a reverse-merge
    decision ("the operator changed ``quality`` from True to 1!").
    The empty string + ``None`` also return ``None`` so they fall
    back to canonical-string equality (where they both collapse to
    ``""``).
    """
    if v is None or isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return float(v)
    try:
        s = str(v).strip()
    except Exception:  # noqa: BLE001
        return None
    if not s:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def _canonical(v: Any) -> str:
    """Render ``v`` to the str form for first-pass equality.

    Booleans → ``TRUE``/``FALSE`` (Sheets's display form).  Floats
    keep their full precision (``repr``) so ``45.0`` doesn't collapse
    to ``"45"`` and mis-trigger a Sheet-edit detection.  Everything
    else → ``str(v)`` after a strip.
    """
    if isinstance(v, bool):
        return "TRUE" if v else "FALSE"
    if isinstance(v, float):
        return repr(v)
    return str(v).strip()


def apply_reverse_edits(
    edits: list[SheetEdit],
    repo_root: Path,
) -> tuple[int, int]:
    """Replay ``"apply"`` edits through ``rundb.update_run_field``.

    Returns ``(applied, skipped)``.  Each applied edit lands in the
    audit log with ``source="sheet"`` — the Show-history UI then
    surfaces those entries alongside dashboard edits, attributed to
    the Sheet rather than to a local operator.

    Since Phase B (multi-year on the Sheet), this function picks the
    target database per-edit by parsing the ``YYYY`` prefix off the
    run id — ``20251119-040749`` lands in ``2025.database.toml``,
    ``20260301-103022`` in ``2026.database.toml``.

    **Phase D.13 — permissive auto-append.**  A Sheet edit on a run
    id that doesn't yet exist in the local TOML triggers an
    ``rundb.append_runs`` for that id first, then the field edit lands
    on the fresh row.  Lets operators spontaneously add new runs from
    the Sheet without touching the dashboard.  Strict
    ``YYYYMMDD-HHMMSS`` guard via ``_RUN_ID_STRICT_RE`` prevents a
    typo cell from spawning a phantom ``[runs."typo"]`` block; ids
    that don't match the pattern are silently skipped.

    The function silently coerces the Sheet's str-typed value back to
    the type the local TOML expects (int / float / bool) when the
    conversion is unambiguous.  Ambiguous strings (e.g. ``"45 V"``)
    stay strings — TOML allows it, and a future push round-trips
    them visibly so the operator can see what happened.
    """
    from . import rundb
    applied = 0
    skipped = 0

    #  Cache existing run-id sets per database file so we don't
    #  re-parse the TOML for every edit (the production database
    #  has ~250 entries; we'd otherwise pay O(N²) reads).
    existing_by_db: dict[Path, set[str]] = {}

    def existing_ids(db_path: Path) -> set[str]:
        if db_path not in existing_by_db:
            existing_by_db[db_path] = {
                r.run_id for r in rundb.load_database(db_path)
            }
        return existing_by_db[db_path]

    for edit in edits:
        if edit.action != "apply":
            skipped += 1
            continue
        year = _year_of_run(edit.run_id)
        if not year:
            skipped += 1
            continue
        #  Strict timestamp shape — typo guard.  A Sheet cell that
        #  accidentally contains "20260601" or "2026 0601-103022"
        #  parses as year=2026 above but fails this match, so we
        #  skip rather than auto-append a phantom row.
        if not _RUN_ID_STRICT_RE.match(edit.run_id):
            skipped += 1
            continue
        db_path = repo_root / "run-lists" / f"{year}.database.toml"
        if not db_path.is_file():
            skipped += 1
            continue

        #  Auto-append if the run id is new to the local TOML.
        #  ``append_runs`` writes an empty ``[runs."<id>"]`` block
        #  at the chronologically-correct slot; the field edit
        #  below then fills the actual cell.  Cached so the next
        #  edit on the same run id doesn't re-check + re-append.
        ids = existing_ids(db_path)
        if edit.run_id not in ids:
            rundb.append_runs(db_path, [edit.run_id])
            ids.add(edit.run_id)

        coerced = _coerce_sheet_value(edit.sheet_value, edit.local_value)
        rundb.update_run_field(
            db_path,
            edit.run_id,
            edit.field,
            coerced,
            auto_pin=True,
            source="sheet",
        )
        applied += 1
    return applied, skipped


def _year_of_run(run_id: str) -> str:
    """Extract the ``YYYY`` prefix from a run id timestamp.

    Returns ``""`` when the prefix isn't four digits — the caller
    treats that as "skip this edit" rather than raising.
    """
    if len(run_id) >= 4 and run_id[:4].isdigit():
        return run_id[:4]
    return ""


def _coerce_sheet_value(sheet_val: Any, local_val: Any) -> Any:
    """Best-effort cast of ``sheet_val`` to the type ``local_val`` carries.

    Sheet reads come back as strings.  When the local TOML carries
    an ``int`` / ``float`` / ``bool`` for this field, attempt the
    obvious cast — keeps the database file shape stable across the
    round-trip.  Falls back to the raw string when the cast fails
    (an operator who typed ``"on-ish"`` over a ``"on"`` cell keeps
    their literal text).
    """
    if sheet_val is None:
        return ""
    if isinstance(local_val, bool):
        s = str(sheet_val).strip().upper()
        if s in {"TRUE", "1", "YES", "Y"}:
            return True
        if s in {"FALSE", "0", "NO", "N", ""}:
            return False
        return sheet_val
    if isinstance(local_val, int) and not isinstance(local_val, bool):
        try:
            return int(str(sheet_val).strip())
        except ValueError:
            return sheet_val
    if isinstance(local_val, float):
        try:
            return float(str(sheet_val).strip())
        except ValueError:
            return sheet_val
    return sheet_val


# ---------------------------------------------------------------------------
# Orchestration — one full sync tick.
# ---------------------------------------------------------------------------


@dataclass
class SyncResult:
    """Outcome of one ``sync_once`` call, for status-bar / CLI logging.

    ``dry_run`` is True when the adapter was skipped because the
    config is unconfigured or the caller forced it.  ``edits_applied``
    counts replayed Sheet-side edits; ``last_push_at`` is the
    timestamp written to the meta worksheet on a real push (empty in
    dry-run).  ``rows_changed`` + ``worksheets_skipped`` come from
    the diff-aware push and are the most useful single statistics
    for a steady-state "is this thing working" status bar.

    Phase D.14: ``hard_reset`` fires when the structural integrity
    check detected the Sheet was mangled (tabs deleted, columns
    renamed, etc.).  ``integrity_issues`` carries the human-readable
    list of what was wrong so the operator can verify it was actually
    mangling and not a legitimate schema migration.
    """

    dry_run: bool
    year: Optional[str]
    edits_applied: int = 0
    edits_skipped_in_sync: int = 0
    last_push_at: str = ""
    updated_ranges: list[str] = field(default_factory=list)
    rendered: dict[str, list[list[Any]]] = field(default_factory=dict)
    rows_changed: int = 0
    worksheets_skipped: int = 0
    hard_reset: bool = False
    integrity_issues: list[str] = field(default_factory=list)


#  ---------------------------------------------------------------
#  Integrity check (Phase D.14)
#  ---------------------------------------------------------------


def check_sheet_integrity(
    *,
    expected_titles: list[str],
    live_titles: set[str],
    local_rendered: dict[str, list[list[Any]]],
    sheet_rows_by_title: dict[str, list[list[Any]]],
    snapshot_rendered: dict[str, list[list[Any]]] | None = None,
) -> list[str]:
    """Detect structural mangling on the live Sheet.

    Returns a list of human-readable issues.  Empty list means the
    Sheet still matches the canonical layout — proceed with the
    normal reverse-merge + diff push.  Non-empty triggers ``sync_once``'s
    hard-reset path (skip reverse-merge, clear snapshot, rewrite
    every worksheet from canonical local state).

    What counts as mangling:

      - **Missing tab.**  One of our expected ``Runs (YYYY)`` / etc.
        worksheets isn't on the Sheet anymore.  Operator deleted it.
      - **Header drift.**  The column-header row (row 3 on Runs tabs:
        banner / group band / header) doesn't match the locally-
        rendered header.  Operator renamed, reordered, or removed
        columns.

    What we deliberately DON'T flag:

      - **Cell edits.**  Those flow through the reverse-merge.
      - **Extra rows.**  Operators may have left calculator notes
        below the data block; we just overwrite them on push if
        needed.
      - **Extra tabs.**  Operator-added scratch tabs are fine and
        stay untouched.
      - **Row count mismatch.**  The diff-aware push handles it.
    """
    issues: list[str] = []

    #  Missing tab — the canonical layout calls for it, the Sheet
    #  doesn't have it.
    for title in expected_titles:
        if title not in live_titles:
            issues.append(f"missing tab: {title!r}")

    #  Header drift on the runs worksheets.  Phase D.15.1: distinguish
    #  legitimate local schema migrations from operator mangling.
    #  A schema migration leaves the Sheet matching the LAST-PUSHED
    #  snapshot's header (the Sheet hasn't moved; we moved); the next
    #  push naturally rewrites it.  Operator mangling leaves the Sheet
    #  matching NEITHER the snapshot nor the current local render.
    for title, sheet_rows in sheet_rows_by_title.items():
        if not title.startswith("Runs ("):
            continue
        local = local_rendered.get(title)
        if not local or len(local) < 3:
            continue
        expected_header = [str(c) for c in local[2]]
        if not sheet_rows or len(sheet_rows) < 3:
            issues.append(
                f"{title} too short to carry a column header"
            )
            continue
        sheet_header = [str(c) for c in sheet_rows[2]]
        if sheet_header == expected_header:
            continue
        #  Sheet vs snapshot — if it matches the last-pushed state,
        #  the operator didn't touch headers; our schema moved.  No
        #  alarm; the diff push refreshes naturally.
        if snapshot_rendered is not None:
            snap_local = snapshot_rendered.get(title)
            if snap_local and len(snap_local) >= 3:
                snap_header = [str(c) for c in snap_local[2]]
                if sheet_header == snap_header:
                    continue
        #  Real operator drift — point at the first divergence.
        n = min(len(expected_header), len(sheet_header))
        diff_idx = n
        for i in range(n):
            if expected_header[i] != sheet_header[i]:
                diff_idx = i
                break
        exp = (expected_header[diff_idx]
               if diff_idx < len(expected_header) else "<missing>")
        got = (sheet_header[diff_idx]
               if diff_idx < len(sheet_header) else "<missing>")
        issues.append(
            f"header drift on {title} at column {diff_idx + 1}: "
            f"expected {exp!r}, got {got!r}"
        )
    return issues


def sync_once(
    cfg: SheetsConfig,
    repo_root: Path,
    *,
    dry_run: bool = False,
    year: Optional[str] = None,
    audit_tail_n: int = 100,
    snapshot_path: Path = SNAPSHOT_PATH,
    app_version: str = "",
    apply_formatting: bool = True,
    force_reverse_merge: bool = False,
    max_reverse_edits: int = MAX_REVERSE_EDITS_PER_TICK,
) -> SyncResult:
    """One full sync tick: reverse-merge then push.

    Sequence::

        1. pull_runs_worksheet   — current Sheet state (skipped in dry-run)
        2. snapshot               — what we last pushed (from disk)
        3. detect_reverse_edits   — diff Sheet vs snapshot
        4. apply_reverse_edits    — replay through rundb (audit-logged)
        5. build_snapshot         — re-read local TOML *after* the merge
        6. render_worksheets      — produce the cell payload
        7. push_snapshot          — write to the Sheet (skipped in dry-run)
        8. save_last_pushed       — refresh the snapshot baseline

    ``dry_run=True`` short-circuits steps 1, 4, 7, and 8 — useful for
    a ``python -m qa_quicklook.sheets_sync --dry-run`` smoke test that
    needs no network and no service-account file.

    Raises one of the typed ``_sheets_adapter`` errors on real-push
    failure; the caller maps each to a status-bar line.  Dry-run path
    is always exception-free past the snapshot build.
    """
    #  --- 1 / 2.  Sheet + snapshot baselines ----------------------------
    snap_persisted = load_last_pushed(path=snapshot_path)

    #  --- 3 / 4.  Per-year diff + audit-logged replay -------------------
    edits_applied = 0
    edits_skipped = 0
    integrity_issues: list[str] = []
    hard_reset = False
    if not dry_run:
        from . import _sheets_adapter
        local_pre = build_snapshot(
            repo_root,
            year=year, audit_tail_n=audit_tail_n,
            operator_tag=cfg.operator_tag, app_version=app_version,
        )
        #  Phase D.14: collect the sheet pulls + the per-year local
        #  renders up front so the integrity check can compare them
        #  before we replay anything.
        sheet_rows_by_title: dict[str, list[list[Any]]] = {}
        local_rendered_by_title: dict[str, list[list[Any]]] = {}
        for y in local_pre.years:
            sheet_name = f"Runs ({y})"
            sheet_rows = _sheets_adapter.pull_runs_worksheet(
                cfg, sheet_name=sheet_name,
            )
            sheet_rows_by_title[sheet_name] = sheet_rows
            local_rendered_by_title[sheet_name] = _render_runs_year(
                local_pre, y,
                banner=_banner(local_pre.meta.get("last_push_at", ""),
                               local_pre.meta.get("operator_tag", ""),
                               note=None),
            )

        #  Pre-flight integrity check.  Mangled Sheet → skip reverse-
        #  merge, clear snapshot in memory, push canonical state fresh.
        expected_titles = list(_expected_titles(local_pre))
        live_titles = {
            title for title, rows in sheet_rows_by_title.items()
            if rows  # absent / never-created tabs come back as []
        }
        #  Also probe the non-runs tabs we expect — pull is cheap, one
        #  row is enough to learn the tab exists.
        for t in expected_titles:
            if t in live_titles or t in sheet_rows_by_title:
                continue
            probe = _sheets_adapter.pull_runs_worksheet(cfg, sheet_name=t)
            if probe:
                live_titles.add(t)

        integrity_issues = check_sheet_integrity(
            expected_titles=expected_titles,
            live_titles=live_titles,
            local_rendered=local_rendered_by_title,
            sheet_rows_by_title=sheet_rows_by_title,
            snapshot_rendered=snap_persisted,
        )
        if integrity_issues:
            hard_reset = True
            #  Skip reverse-merge entirely — the Sheet's structure
            #  isn't trustworthy.  Clear the in-memory snapshot so
            #  the push goes through the no-baseline clear+rewrite
            #  path; the on-disk snapshot will be replaced after the
            #  push lands.
            snap_persisted = {}
        else:
            all_to_apply: list[SheetEdit] = []
            all_skipped_in_sync = 0
            for sheet_name, sheet_rows in sheet_rows_by_title.items():
                if not sheet_rows:
                    continue  # first push for this year, or worksheet empty
                local_rendered = local_rendered_by_title[sheet_name]
                snapshot_rows = snap_persisted.get(sheet_name, [])
                edits = detect_reverse_edits(
                    sheet_rows, local_rendered, snapshot_rows,
                )
                all_skipped_in_sync += sum(
                    1 for e in edits if e.action == "skip_already_in_sync"
                )
                all_to_apply.extend(e for e in edits if e.action == "apply")

            edits_skipped = all_skipped_in_sync
            #  Safety brake — sum across all years.  Cross-year bulk
            #  reverse-merges are even more suspicious than single-year
            #  ones (the snapshot can't be stale for multiple years at
            #  once unless a full ~/.cache wipe happened).
            if not force_reverse_merge and len(all_to_apply) > max_reverse_edits:
                raise ReverseMergeBrake(
                    edit_count=len(all_to_apply),
                    threshold=max_reverse_edits,
                    sample=all_to_apply,
                )
            edits_applied, _ = apply_reverse_edits(all_to_apply, repo_root)

    #  --- 5 / 6.  Re-read + render after the merge ----------------------
    #  Bake the predicted push timestamp into the banner now so the
    #  rendered payload is the same one we'll persist as the next
    #  reverse-merge baseline.  Dry-run leaves it blank → banner
    #  shows "Last push: (pending)".
    next_push_at = "" if dry_run else _iso_now()
    snap = build_snapshot(
        repo_root,
        year=year, audit_tail_n=audit_tail_n,
        operator_tag=cfg.operator_tag, app_version=app_version,
        last_push_at=next_push_at,
    )
    rendered = render_worksheets(snap)

    if dry_run:
        return SyncResult(
            dry_run=True,
            year=snap.year,
            edits_applied=0,
            edits_skipped_in_sync=0,
            last_push_at="",
            updated_ranges=[],
            rendered=rendered,
        )

    #  --- 7 / 8.  Push + refresh the snapshot baseline -----------------
    from . import _sheets_adapter
    result = _sheets_adapter.push_snapshot(
        rendered, cfg,
        apply_formatting=apply_formatting,
        #  Hand the persisted baseline to the diff-aware push so
        #  unchanged worksheets skip the API entirely.  Note this is
        #  the SAME ``snap_persisted`` we loaded at step 2 — applying
        #  reverse-merge between then and now doesn't change it.
        last_pushed=snap_persisted,
    )
    #  The banner timestamp was already baked into ``rendered`` at
    #  step 5, so persisting ``rendered`` directly is the right
    #  next-reverse-merge baseline — no patch step needed.
    save_last_pushed(rendered, path=snapshot_path)
    return SyncResult(
        dry_run=False,
        year=snap.year,
        edits_applied=edits_applied,
        edits_skipped_in_sync=edits_skipped,
        last_push_at=next_push_at,
        updated_ranges=result.updated_ranges,
        rendered=rendered,
        rows_changed=result.rows_changed,
        worksheets_skipped=result.worksheets_skipped,
        hard_reset=hard_reset,
        integrity_issues=integrity_issues,
    )


# ---------------------------------------------------------------------------
# Small utilities
# ---------------------------------------------------------------------------


def _iso_now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S")


def _expected_titles(snap: Snapshot) -> tuple[str, ...]:
    """Set of canonical worksheet titles for a given snapshot.

    Mirrors the iteration order in ``render_worksheets`` so the
    integrity check's "missing tab" message names exactly what the
    push would write.
    """
    out: list[str] = []
    for y in reversed(snap.years):
        out.append(f"Runs ({y})")
    out.append("Runlists")
    out.append("Radiators")
    out.append("Audit")
    return tuple(out)


#  ``python -m qa_quicklook.sheets_sync`` entry point — keeps argparse
#  + logging noise out of the module proper.  See ``_sheets_cli.py``.
if __name__ == "__main__":  # pragma: no cover
    from qa_quicklook._sheets_cli import main
    raise SystemExit(main())


__all__ = [
    "MAX_REVERSE_EDITS_PER_TICK",
    "SNAPSHOT_PATH",
    "ReverseMergeBrake",
    "SheetEdit",
    "SheetsConfig",
    "Snapshot",
    "SyncResult",
    "apply_reverse_edits",
    "auto_year",
    "build_snapshot",
    "detect_reverse_edits",
    "load_config",
    "load_last_pushed",
    "render_worksheets",
    "resolve_operator_tag",
    "runs_cell_index",
    "save_last_pushed",
    "sync_once",
]
