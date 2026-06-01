"""qa_quicklook/streaming_picker.py — pure helpers behind Option C.

Backs the interactive n_σ picker (see
``include/triggers/streaming/DISCUSSION.md`` §1.5.2 Option C and the
``shifter-nsigma-to-rundb`` memory).  Kept deliberately pure: no Qt,
no matplotlib, no uproot imports at module load — the dialog imports
both numpy and uproot lazily so that ``unittest`` can cover this file
on a stripped-down environment.

Contract
--------

The dialog reads ``h_streaming_score_noise`` + ``h_streaming_score_data``
from ``<run>/lightdata.root`` (both booked at ``125`` bins on ``0..50``,
see ``src/lightdata_writer.cxx`` near the ``RootHist<TH1F>`` calls
that name the two hists) and renders them on a shared log-Y canvas.
The operator drags a vertical line; the side panel shows four live
quantities and a "Save to rundb" button commits via
:func:`qa_quicklook.rundb.update_run_field`.

The four quantities

  ``p_misfire = Σ(noise ≥ cut) / Σ noise``
      First-frames noise distribution above the cut — fraction of
      pure-noise frames that would fire the trigger.

  ``acceptance = Σ(data ≥ cut) / Σ data``
      Data-taking distribution above the cut — fraction of data
      frames the trigger would keep.

  ``sn_ratio  = Σ(data ≥ cut) / Σ(noise ≥ cut)``
      Naïve signal-to-noise above the cut.  Drops to ``nan`` when the
      noise integral is zero (sn is undefined, not infinite, in that
      regime — the dialog renders ``"—"``).

  ``n_above_data``
      Raw count above the cut on the data sample.  The "is there
      anything left to trigger on" sanity check.

Crossover seed
--------------

When the rundb has no value and the streaming-conf default is the
``1e9`` QA-disable sentinel (the realistic case the first time an
operator opens the picker), :func:`noise_data_crossover` picks the
right-most n_σ where the noise step plot crosses the data step plot
from above.  That's where the data tail emerges from the noise floor
on the log-Y canvas — the canonical "natural threshold" the
discussion-doc QA recipe points at.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Sequence

#  Match the C++ writer's sentinel for QA-mode "streaming disabled".
#  Kept in sync with ``conf/QA/streaming.toml`` (``n_sigma_threshold =
#  1.0e9``).  When the conf value is at this level we treat it as
#  "no usable default" and fall through to the crossover heuristic.
QA_DISABLE_SENTINEL = 1.0e9


@dataclass(frozen=True)
class CutStats:
    """One-shot panel-ready summary of the cut."""

    p_misfire: float        # ∫noise ≥ cut / Σnoise  (0 when Σnoise = 0)
    acceptance: float       # ∫data  ≥ cut / Σdata   (0 when Σdata  = 0)
    sn_ratio: Optional[float]  # ∫data ≥ cut / ∫noise ≥ cut  (None when 0/0)
    n_above_data: float     # raw data counts above the cut


def integral_above(
    counts: Sequence[float],
    edges: Sequence[float],
    cut: float,
) -> float:
    """Integral of ``counts`` over bins whose centre satisfies ``x ≥ cut``.

    Linearly interpolates inside the bin that straddles ``cut`` so a
    slow drag of the picker line produces smoothly-varying stats
    rather than step jumps every bin boundary.  ``counts`` is the
    bare bin-content array (length ``N``); ``edges`` is the bin-edge
    array (length ``N+1``).  Matches ``hist.to_numpy()`` from uproot.

    Edge semantics:
      - ``cut <= edges[0]``  → full integral
      - ``cut >= edges[-1]`` → 0
      - otherwise → tail of the straddled bin (linear in cut) plus
        the sum of the strictly-above bins.
    """
    n = len(counts)
    if n == 0 or len(edges) != n + 1:
        return 0.0
    if cut <= edges[0]:
        return float(sum(counts))
    if cut >= edges[-1]:
        return 0.0

    #  Find the bin whose range contains the cut.  edges is strictly
    #  monotonic by ROOT TH1 convention so a linear scan is fine for
    #  the 125-bin hist; np.searchsorted would be faster on a giant
    #  hist but isn't worth the import here.
    for i in range(n):
        lo, hi = edges[i], edges[i + 1]
        if lo <= cut < hi:
            #  Fraction of THIS bin that sits above the cut, assuming
            #  bin contents are uniformly distributed inside the bin.
            #  Plus all strictly-above bins.
            partial = counts[i] * (hi - cut) / (hi - lo)
            above = sum(counts[i + 1:])
            return float(partial + above)
    #  Fall-through (shouldn't happen given the edge checks above).
    return 0.0


def cut_stats(
    noise_counts: Sequence[float],
    data_counts: Sequence[float],
    edges: Sequence[float],
    cut: float,
) -> CutStats:
    """Bundle the four panel quantities at a single cut."""
    sum_noise = float(sum(noise_counts))
    sum_data = float(sum(data_counts))
    above_noise = integral_above(noise_counts, edges, cut)
    above_data = integral_above(data_counts, edges, cut)

    p_misfire = (above_noise / sum_noise) if sum_noise > 0 else 0.0
    acceptance = (above_data / sum_data) if sum_data > 0 else 0.0
    sn_ratio: Optional[float] = (
        (above_data / above_noise) if above_noise > 0 else None
    )
    return CutStats(
        p_misfire=p_misfire,
        acceptance=acceptance,
        sn_ratio=sn_ratio,
        n_above_data=above_data,
    )


def noise_data_crossover(
    noise_counts: Sequence[float],
    data_counts: Sequence[float],
    edges: Sequence[float],
) -> Optional[float]:
    """Find the right-most n_σ where ``data`` emerges above ``noise``.

    Walks the bins right-to-left.  Reports the centre of the
    right-most bin where data ≥ noise *and* the next bin to the right
    has noise > data — i.e. the canvas crossover the QA recipe points
    operators at.  Returns ``None`` when no crossover exists (e.g.
    data dominates everywhere, or noise dominates everywhere).
    """
    n = len(noise_counts)
    if n < 2 or len(data_counts) != n or len(edges) != n + 1:
        return None

    #  Walk i = n-2 down to 0; we need i+1 to have noise > data for
    #  a "data crosses out of the noise floor going right" event.
    for i in range(n - 2, -1, -1):
        if data_counts[i] >= noise_counts[i] and \
           noise_counts[i + 1] > data_counts[i + 1]:
            return float(0.5 * (edges[i] + edges[i + 1]))
    return None


def seed_threshold(
    *,
    rundb_value: float,
    conf_value: float,
    noise_counts: Sequence[float],
    data_counts: Sequence[float],
    edges: Sequence[float],
    disable_sentinel: float = QA_DISABLE_SENTINEL,
) -> float:
    """Pick the initial n_σ to seed the picker line at.

    Priority chain:
      1. ``rundb_value`` when ``> 0``  (the operator's last decision).
      2. :func:`noise_data_crossover` when computable AND the conf
         default looks unusable (≥ ``disable_sentinel``).
      3. ``conf_value``                (the streaming-conf default).
      4. Crossover as a last-ditch fallback when conf is also unusable.
      5. ``5.0``                       (a deliberately low-ish sentinel
         that keeps the line inside the booked 0..50 range).

    Why step 2 outranks step 3: the only reason the conf default
    reads as the disable sentinel is QA mode, and the operator
    opening the picker in QA mode wants the canvas's natural cut, not
    a 1e9 off-canvas line.
    """
    if rundb_value > 0:
        return float(rundb_value)

    conf_usable = 0 < conf_value < disable_sentinel
    crossover = noise_data_crossover(noise_counts, data_counts, edges)

    if not conf_usable and crossover is not None:
        return crossover
    if conf_usable:
        return float(conf_value)
    if crossover is not None:
        return crossover
    return 5.0


__all__ = [
    "QA_DISABLE_SENTINEL",
    "CutStats",
    "integral_above",
    "cut_stats",
    "noise_data_crossover",
    "seed_threshold",
]
