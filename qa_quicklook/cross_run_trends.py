"""Cross-run trend extraction for the QA General tab.

Source: ``<data_repository>/standard_results.toml`` (i.e.
``Data/standard_results.toml``, NOT the git repo root) — the mirror of
the C++ ``AnalysisResults`` store (see ``include/analysis_results.h``).
The TOML schema is::

    [results."<run>"."<sensor>"]
    "<quantity>" = { value = X, error = Y }   # error optional

This module is the General-tab consumer.  It

  1. Walks the loaded results dict and pulls one (sensor, quantity)
     time series at a time — what each tile on the General tab
     plots horizontally across runs.
  2. Sorts runs by their lexicographic id (run ids are dated
     ``YYYYMMDD-HHMMSS`` strings, so lex-sort == time-sort) and keeps
     the most recent ``N``.
  3. Supports synthetic / derived metrics — ``DCR rate per event``
     for instance is the ratio of two stored quantities.  The derive
     hook lives in :class:`MetricSpec` so a tile doesn't have to know
     the difference.

Kept deliberately pure: no Qt, no matplotlib, no AnalysisResults C++
bindings.  The QA-tab code imports this module and renders the
returned :class:`TrendSeries` lists into matplotlib tiles.  Tests in
``tests/test_cross_run_trends.py`` exercise this module against a
synthetic TOML without touching Qt or the real data dir.

The default metric list (:data:`DEFAULT_METRICS`) reflects the
observables we cleared with the operators: photon yield
(``full.n_gamma``), photon-yield width (``full.sigma``), mean dark-count
rate (``lightdata.dcr_mean_khz``, kHz), and the readout-resilience
lane-failure fraction (``lightdata.lane_failure_rate``).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Optional

if False:  # pragma: no cover — type-check only
    from . import rundb

#  Default trend-window size.  Twenty runs is the lower bound at which
#  a drift on the scale of "one shift" (≈10 runs) is visually
#  obvious without the tile becoming a forest of points.  Operator-
#  overridable via ``[qa_general] trend_runs_n``.
DEFAULT_TREND_RUNS_N = 20


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class TrendPoint:
    """One ``(run_id, value, error)`` reading.

    ``error == 0.0`` is the documented "no uncertainty" sentinel —
    matches ``rundb.ResultEntry``.  The tile renderer falls back to a
    plain marker (no errorbar) for those points.
    """

    run_id: str
    value: float
    error: float = 0.0


@dataclass(frozen=True)
class TrendSeries:
    """All points emitted for one metric across the requested run window.

    ``points`` is ordered oldest → newest (lex-sorted on ``run_id``),
    so the General-tab plot reads left-to-right as time.  ``missing``
    lists run ids in the window that had no datum for this metric —
    surfaced as a footer line on the tile so the operator can tell
    "no value yet" from "value happens to be zero".
    """

    metric: "MetricSpec"
    points: list[TrendPoint] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)


#  Custom derive: takes the ``{quantity: ResultEntry}`` map for one
#  (run, sensor) and returns ``(value, error)`` or ``None`` if the
#  inputs are missing.  ResultEntry is duck-typed (``.value`` /
#  ``.error``) so tests can pass plain ``SimpleNamespace`` and skip
#  importing rundb.
DeriveFn = Callable[[dict], Optional[tuple[float, float]]]


@dataclass(frozen=True)
class MetricSpec:
    """How one tile pulls its series out of ``results_load`` output.

    ``quantity``     — the leaf key (e.g. ``"full.n_gamma"``).  Ignored
                       when ``derive`` is set.
    ``sensor``       — sensor slice (default ``"all"`` matches what the
                       writers publish for run-level scalars).
    ``derive``       — optional alternative to ``quantity``.  When set,
                       the loader passes the whole per-(run, sensor)
                       quantity map and the function picks/computes
                       the scalar.  Returns ``None`` to signal "data
                       missing" — that run goes into ``TrendSeries.missing``.
    ``unit``         — short label drawn on the y-axis.  Optional.
    ``y_floor_zero`` — when True the tile forces the y-axis lower bound
                       to 0 (rates, counts).  Off for fit parameters
                       like σ_photon where ``min(values) - δ`` is more
                       useful than ``0``.
    """

    key: str
    label: str
    sensor: str = "all"
    quantity: Optional[str] = None
    derive: Optional[DeriveFn] = None
    unit: str = ""
    y_floor_zero: bool = True

    def __post_init__(self) -> None:
        # Validate at construction so a malformed metric list raises
        # immediately, not on first render.
        if (self.quantity is None) == (self.derive is None):
            raise ValueError(
                f"MetricSpec {self.key!r}: exactly one of `quantity` "
                "or `derive` must be set"
            )


#  Default trend tiles — kept narrow on purpose.  The General tab is
#  meant to be the "fast triage" surface; deep dives live in Full plots.
DEFAULT_METRICS: tuple[MetricSpec, ...] = (
    MetricSpec(
        key="n_photons",
        label="Photon yield  ⟨N_γ⟩",
        quantity="full.n_gamma",
        unit="γ / frame",
        y_floor_zero=True,
    ),
    MetricSpec(
        key="sigma_photons",
        label="Photon-yield σ",
        quantity="full.sigma",
        unit="γ / frame",
        y_floor_zero=False,
    ),
    MetricSpec(
        key="dcr_rate",
        label="DCR rate  ⟨kHz⟩",
        quantity="lightdata.dcr_mean_khz",
        unit="kHz",
        y_floor_zero=True,
    ),
    MetricSpec(
        key="lane_failure_rate",
        label="Lane-failure rate",
        quantity="lightdata.lane_failure_rate",
        unit="fraction",
        y_floor_zero=True,
    ),
)


# ---------------------------------------------------------------------------
# Window selection
# ---------------------------------------------------------------------------


def select_recent_runs(run_ids: Iterable[str], n: int) -> list[str]:
    """Return the ``n`` most-recent run ids, oldest-to-newest.

    Run ids are dated ``YYYYMMDD-HHMMSS`` strings, so a plain
    lexicographic sort orders them chronologically.  We sort
    descending, slice the head, then reverse so the trend tiles read
    left-to-right as time.

    Non-dated ids (legacy fixtures, manual ``test`` runs) are still
    sortable — they just cluster at one end of the window.  ``n <= 0``
    yields an empty list — used as a defensive guard so a misconfigured
    knob can't make the dashboard explode.
    """
    if n <= 0:
        return []
    sortable = sorted({str(r) for r in run_ids}, reverse=True)
    head = sortable[:n]
    head.reverse()
    return head


# ---------------------------------------------------------------------------
# Series extraction
# ---------------------------------------------------------------------------


def extract_series(
    results: dict,
    metric: MetricSpec,
    run_ids: Iterable[str],
) -> TrendSeries:
    """Pull one metric across ``run_ids`` from ``results_load`` output.

    ``results`` is the nested dict from :func:`rundb.results_load`.
    Missing runs / sensors / quantities are recorded in
    :attr:`TrendSeries.missing` so the tile can footnote how complete
    the window is — silent omissions would let a slowly-failing writer
    look like steady-state data.

    Pure function: no I/O, no caching, no Qt.  Calling it twice with
    the same args returns identical lists, which is what the tests
    rely on for snapshot-style assertions.
    """
    points: list[TrendPoint] = []
    missing: list[str] = []
    for run_id in run_ids:
        sensors = results.get(run_id)
        if not isinstance(sensors, dict):
            missing.append(run_id)
            continue
        qmap = sensors.get(metric.sensor)
        if not isinstance(qmap, dict):
            missing.append(run_id)
            continue
        if metric.derive is not None:
            derived = metric.derive(qmap)
            if derived is None:
                missing.append(run_id)
                continue
            value, error = derived
        else:
            entry = qmap.get(metric.quantity)  # type: ignore[arg-type]
            if entry is None:
                missing.append(run_id)
                continue
            value = float(entry.value)
            error = float(entry.error)
        points.append(TrendPoint(run_id=run_id, value=value, error=error))
    return TrendSeries(metric=metric, points=points, missing=missing)


def load_trends(
    standard_results_path: Path,
    metrics: Iterable[MetricSpec] = DEFAULT_METRICS,
    n_runs: int = DEFAULT_TREND_RUNS_N,
    *,
    results_loader: Optional[Callable[[Path], dict]] = None,
) -> list[TrendSeries]:
    """Top-level convenience: read the file, pick the window, extract series.

    ``results_loader`` is injected so tests can hand in a fake reader
    without monkey-patching ``rundb``.  Production callers leave it at
    the default (``rundb.results_load``).

    Returns one :class:`TrendSeries` per metric, in input order.
    Empty list when the file is missing — the General tab treats this
    the same as "no runs yet have published trend metrics".
    """
    if results_loader is None:
        from . import rundb  # local import: keep this module Qt-free
        results_loader = rundb.results_load

    results = results_loader(standard_results_path)
    if not results:
        return []
    run_ids = select_recent_runs(results.keys(), n_runs)
    return [extract_series(results, m, run_ids) for m in metrics]


# ---------------------------------------------------------------------------
# Settings hook
# ---------------------------------------------------------------------------


def read_trend_runs_n(
    qa_quicklook_toml: Path,
    default: int = DEFAULT_TREND_RUNS_N,
) -> int:
    """Read ``[qa_general] trend_runs_n`` from the dashboard config.

    Returns ``default`` on any failure (missing file, parse error,
    missing section, non-integer value) — the General tab must never
    fail to render because of a typo in the TOML.  Negative / zero
    values clamp to 1 so the tile still attempts to draw something
    (one point is a degenerate but valid trend).
    """
    if not qa_quicklook_toml.is_file():
        return default
    try:
        import sys
        if sys.version_info >= (3, 11):
            import tomllib
        else:  # pragma: no cover
            import tomli as tomllib  # type: ignore
        with qa_quicklook_toml.open("rb") as fh:
            doc = tomllib.load(fh)
    except Exception:  # noqa: BLE001
        return default
    section = doc.get("qa_general")
    if not isinstance(section, dict):
        return default
    raw = section.get("trend_runs_n", default)
    try:
        n = int(raw)
    except (TypeError, ValueError):
        return default
    return max(1, n)


__all__ = [
    "DEFAULT_METRICS",
    "DEFAULT_TREND_RUNS_N",
    "MetricSpec",
    "TrendPoint",
    "TrendSeries",
    "extract_series",
    "load_trends",
    "read_trend_runs_n",
    "select_recent_runs",
]
