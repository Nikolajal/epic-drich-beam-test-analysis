"""Per-run overview reader.

Reads ``Data/<run>/recodata.root`` with **uproot** (pure-Python, no
PyROOT in the venv) and returns a tiny dataclass the GUI renders as
the run-overview card: σ (ring resolution), tree entries, trigger
count.  Pure function of a directory path; no GUI imports.

Why uproot, not ``btana-dump``: the dump binary prints summary lines
but does not expose per-bin contents of the ``h_peak_sigma_summary``
histogram (it would need a new flag and a rebuild).  uproot reads
the TH1 directly with one open call.

Why this lives separate from ``catalog.py``: the catalog is concerned
with *stage state* (is the file there, is it stale).  The summary is
*content* (numbers from inside the file).  Different cost profile —
catalog inventory is one ``stat`` per run, summary is one ROOT open
per *currently-selected* run.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# Bin in ``Rings/h_peak_sigma_summary`` we surface as the headline σ.
# The histogram is bin-labelled with the eight ring variants; "first"
# is the primary refined ring on mask-tagged hits and the figure the
# operator actually watches during a shift.  All eight values come
# along inside ``Summary.sigma_by_bin`` for the tooltip / detail view.
HEADLINE_SIGMA_BIN = "first"


@dataclass
class Summary:
    """Tiny per-run summary.  Every field is optional so we can
    distinguish "missing" from "zero"."""

    run_id: str
    has_recodata: bool = False
    sigma_mm: Optional[float] = None                # headline σ_first
    sigma_by_bin: dict[str, float] = field(default_factory=dict)
    n_entries: Optional[int] = None                 # recodata TTree
    n_triggers: Optional[int] = None                # Triggers/h_trigger_qa
    error: Optional[str] = None                     # populated when read failed


def read_summary(run_dir: Path, run_id: str | None = None) -> Summary:
    """Read the per-run summary from ``run_dir/recodata.root``.

    Returns a ``Summary`` with ``has_recodata=False`` when the file is
    not there — the GUI renders that as the "run recodata first"
    placeholder, no exception.  Read failures populate ``error`` so the
    GUI can show *why* the card is empty instead of just leaving it
    blank.
    """
    rid = run_id or run_dir.name
    reco = run_dir / "recodata.root"
    if not reco.is_file():
        return Summary(run_id=rid, has_recodata=False)

    # Import uproot lazily so the catalog/CLI paths don't pull it in.
    try:
        import uproot  # type: ignore
    except ImportError as exc:  # pragma: no cover — venv is bootstrapped
        return Summary(run_id=rid, has_recodata=True, error=f"uproot import failed: {exc}")

    try:
        f = uproot.open(reco)
    except Exception as exc:  # noqa: BLE001 — uproot raises a wide tree
        return Summary(run_id=rid, has_recodata=True, error=f"open failed: {exc}")

    summary = Summary(run_id=rid, has_recodata=True)
    try:
        summary.sigma_by_bin = _read_sigma_summary(f)
        summary.sigma_mm = summary.sigma_by_bin.get(HEADLINE_SIGMA_BIN)
        summary.n_entries = _read_recodata_entries(f)
        summary.n_triggers = _read_trigger_count(f)
    except Exception as exc:  # noqa: BLE001
        summary.error = f"parse failed: {exc}"
    return summary


# ---------------------------------------------------------------------------
# uproot helpers — small enough to keep inline.  Each is forgiving of
# the histogram being absent (older recodata.root files predate the
# bin-labelled summaries) and returns ``None`` / ``{}`` in that case.
# ---------------------------------------------------------------------------


def _read_sigma_summary(f) -> dict[str, float]:
    """Return ``{bin_label: sigma_value}`` for ``Rings/h_peak_sigma_summary``.

    The histogram is a ``TH1F`` with one bin per ring variant; we pair
    the axis labels with the bin values.  Unlabelled / zero-content
    bins are filtered out so the caller doesn't render noise.
    """
    h = _safe_get(f, "Rings/h_peak_sigma_summary")
    if h is None:
        return {}
    axis = h.axis()
    labels = list(axis.labels() or [])
    values = h.values().tolist()
    if not labels or len(labels) != len(values):
        return {}
    return {lbl: float(v) for lbl, v in zip(labels, values) if v != 0.0}


def _read_recodata_entries(f) -> Optional[int]:
    """TTree entry count for the ``recodata`` tree."""
    t = _safe_get(f, "recodata")
    if t is None:
        return None
    return int(t.num_entries)


def _read_trigger_count(f) -> Optional[int]:
    """Entry count of the trigger-QA histogram.

    Each trigger contributes one entry to this 2-D histogram, so the
    entry count is the per-run trigger total.  When ``h_trigger_qa``
    is absent we return ``None`` (the older recodata files don't
    include it).
    """
    h = _safe_get(f, "Triggers/h_trigger_qa")
    if h is None:
        return None
    return int(h.member("fEntries"))


def _safe_get(f, key: str):
    """``f[key]`` that returns ``None`` instead of raising on a missing key.

    uproot raises ``uproot.exceptions.KeyInFileError`` (a subclass of
    ``KeyError``) for missing keys, but older versions raised a plain
    ``KeyError``.  We catch the base class so the helper works across
    uproot 4 and 5.
    """
    try:
        return f[key]
    except KeyError:
        return None
