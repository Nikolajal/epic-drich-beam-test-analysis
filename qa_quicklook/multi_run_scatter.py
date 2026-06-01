"""qa_quicklook/multi_run_scatter.py — multi-run scatter data layer.

Pulls one QA quantity (the Y axis) from ``standard_results.toml`` and a
beam-info field (the X axis, e.g. ``v_bias``) — plus an optional second
beam-info as the colour/Z dimension (e.g. ``mirror_mm``) — for every run
in a runlist, and joins them into scatter points.

Lets the analyser read a first-level QA quantity *against the conditions
that produced it*: "N_σ vs V_bias", "N_γ vs mirror position", coloured by
a third axis.  Multiple runs can share an X value (e.g. several runs at
the same V_bias) — :func:`jitter_x` spreads them slightly so the cluster
is visible, deterministically per run so the spread doesn't jump on every
repaint.

Kept deliberately pure: no Qt, no matplotlib, no file I/O.  The Y side
reuses :func:`cross_run_trends.extract_series` (the same `results_load`
slicing the trend tiles use); the X/Z side reads the merged view of each
:class:`rundb.RunRecord`.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from .cross_run_trends import MetricSpec, extract_series


#  Numeric run-card fields that make sense as an X or Z (colour) axis.
#  These are the "beam info" knobs the operator scans against — string
#  fields (beam_status, polarity, …) are excluded since a scatter axis
#  needs an ordering.  ``mirror_mm`` is the mirror position.
BEAM_AXIS_FIELDS: tuple[str, ...] = (
    "v_bias",
    "beam_energy",
    "temperature",
    "mirror_mm",
    "deltathr",
    "n_spills",
)


@dataclass(frozen=True)
class ScatterPoint:
    """One (run, x, y[, z]) datum on the multi-run scatter."""

    run_id: str
    x: float            # beam-info value (X axis)
    y: float            # QA quantity value (Y axis)
    y_err: Optional[float] = None
    z: Optional[float] = None   # optional colour dimension


def _coerce_float(value: object) -> Optional[float]:
    """Best-effort float; ``None`` for missing / non-numeric (incl. bool,
    which would otherwise sneak through as 0/1)."""
    if value is None or isinstance(value, bool):
        return None
    try:
        return float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None


def build_scatter(
    results: dict,
    records: list,
    run_ids: list[str],
    y_metric: MetricSpec,
    x_field: str,
    z_field: Optional[str] = None,
) -> tuple[list[ScatterPoint], list[str]]:
    """Join QA-quantity Y with beam-info X (and optional Z) per run.

    Parameters
    ----------
    results
        Nested dict from :func:`rundb.results_load`
        (``{run: {sensor: {quantity: ResultEntry}}}``).
    records
        ``list[rundb.RunRecord]`` from :func:`rundb.load_database` —
        carries each run's merged beam-info fields.
    run_ids
        The runlist to plot, in the order to consider them.
    y_metric
        Which QA quantity drives Y (a ``cross_run_trends.MetricSpec``).
    x_field, z_field
        Run-card field names for the X axis and the optional Z (colour).

    Returns ``(points, skipped)`` — ``skipped`` lists run ids dropped
    because they lacked a Y value or a numeric X (so the caller can
    footnote how complete the plot is rather than silently omitting).
    """
    rec_by_id = {r.run_id: r for r in records}

    #  Y values via the shared trend extractor (handles sensor slice +
    #  derived metrics + records its own missing-run list).
    y_series = extract_series(results, y_metric, run_ids)
    y_by_id = {p.run_id: (p.value, p.error) for p in y_series.points}

    points: list[ScatterPoint] = []
    skipped: list[str] = list(y_series.missing)  # runs with no Y datum
    for run_id in run_ids:
        if run_id not in y_by_id:
            continue  # already in skipped via y_series.missing
        rec = rec_by_id.get(run_id)
        x = _coerce_float(rec.get(x_field)) if rec is not None else None
        if x is None:
            skipped.append(run_id)
            continue
        z = (_coerce_float(rec.get(z_field))
             if (rec is not None and z_field) else None)
        y_val, y_err = y_by_id[run_id]
        points.append(ScatterPoint(run_id, x, y_val, y_err, z))
    return points, skipped


def jitter_x(
    points: list[ScatterPoint],
    spread_frac: float = 0.012,
) -> list[float]:
    """Per-point X offsets that separate runs sharing an X value.

    Returns one jittered X per point, in the same order.  The offset is
    deterministic (hash of the run id), so the cloud is stable across
    repaints, and scaled to ``spread_frac`` of the data's X span so it
    reads as a small graphical nudge regardless of axis units.  Points
    whose X value is unique get no offset.
    """
    if not points:
        return []
    xs = [p.x for p in points]
    span = (max(xs) - min(xs)) or 1.0
    amp = spread_frac * span

    #  Count how many points share each X so singletons aren't nudged.
    from collections import Counter
    shared = Counter(xs)

    out: list[float] = []
    for p in points:
        if shared[p.x] <= 1:
            out.append(p.x)
            continue
        #  Deterministic signed offset in [-amp, +amp] from the run id.
        h = 0
        for ch in p.run_id:
            h = (h * 131 + ord(ch)) & 0xFFFFFFFF
        frac = (h % 2001) / 1000.0 - 1.0   # [-1, 1]
        out.append(p.x + amp * frac)
    return out


__all__ = [
    "BEAM_AXIS_FIELDS",
    "ScatterPoint",
    "build_scatter",
    "jitter_x",
]
