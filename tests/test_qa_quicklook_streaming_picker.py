"""Unit tests for ``qa_quicklook.streaming_picker``.

Covers the pure helpers behind the §1.5.2 Option C dialog: integral-
above-cut bin-interpolation semantics, the four-quantity bundle, the
right-most noise/data crossover heuristic, and the seed-priority
chain (rundb > crossover-when-conf-unusable > conf > crossover > 5).

Run::

    .venv/bin/python -m unittest tests.test_qa_quicklook_streaming_picker
"""

from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook import streaming_picker as sp  # noqa: E402


def _edges(lo: float, hi: float, n: int) -> list[float]:
    """Uniform edges; mirrors ROOT TH1F uniform-bin booking."""
    step = (hi - lo) / n
    return [lo + i * step for i in range(n + 1)]


# ---------------------------------------------------------------------------
# integral_above
# ---------------------------------------------------------------------------

class IntegralAboveTests(unittest.TestCase):
    """The integral is what the side panel sums on every drag — has to
    behave right at edges, at bin boundaries, and inside straddled bins."""

    def setUp(self) -> None:
        # Six uniform bins on [0, 60); ramp counts 1..6 so the bin
        # contributing to a given cut is easy to reason about.
        self.counts = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
        self.edges = _edges(0.0, 60.0, 6)

    def test_cut_at_or_below_left_edge_returns_total(self) -> None:
        total = sum(self.counts)
        self.assertEqual(sp.integral_above(self.counts, self.edges, -5.0), total)
        self.assertEqual(sp.integral_above(self.counts, self.edges, 0.0), total)

    def test_cut_at_or_above_right_edge_returns_zero(self) -> None:
        self.assertEqual(sp.integral_above(self.counts, self.edges, 60.0), 0.0)
        self.assertEqual(sp.integral_above(self.counts, self.edges, 999.0), 0.0)

    def test_cut_on_bin_boundary_yields_bins_above(self) -> None:
        # cut = 30 → bin 3 (edges 30..40) fully included = 4+5+6 = 15.
        # Implementation routes 30 into bin index 3 via "lo <= cut < hi";
        # partial = counts[3] * (40-30)/10 = 4; above = 5+6 = 11. Sum = 15.
        self.assertEqual(
            sp.integral_above(self.counts, self.edges, 30.0), 15.0,
        )

    def test_cut_mid_bin_linearly_interpolates(self) -> None:
        # cut = 35 → straddles bin 3 (30..40). Tail = 4 * (40-35)/10 = 2.
        # Plus bins 4 and 5 → 5 + 6 = 11. Total = 13.
        self.assertAlmostEqual(
            sp.integral_above(self.counts, self.edges, 35.0), 13.0,
        )

    def test_empty_counts_or_mismatched_edges_returns_zero(self) -> None:
        self.assertEqual(sp.integral_above([], [], 0.0), 0.0)
        self.assertEqual(sp.integral_above([1.0, 2.0], [0.0, 1.0], 0.5), 0.0)


# ---------------------------------------------------------------------------
# cut_stats
# ---------------------------------------------------------------------------

class CutStatsTests(unittest.TestCase):
    """Smoke-tests the four-quantity bundle and the two undefined-ratio
    edge cases (no noise above cut → sn_ratio is None; empty sample →
    p_misfire / acceptance fall back to 0)."""

    def setUp(self) -> None:
        self.edges = _edges(0.0, 10.0, 5)  # 2-wide bins
        # Noise concentrated at low n_σ, data concentrated at high.
        self.noise = [80.0, 15.0, 4.0, 1.0, 0.0]
        self.data  = [5.0, 10.0, 20.0, 30.0, 35.0]

    def test_basic_quantities_at_mid_cut(self) -> None:
        s = sp.cut_stats(self.noise, self.data, self.edges, 4.0)
        # noise above cut: bin 2 fully (4) + bin 3 (1) + bin 4 (0) = 5
        # → p_misfire = 5 / 100 = 0.05
        self.assertAlmostEqual(s.p_misfire, 0.05)
        # data above cut: 20 + 30 + 35 = 85; total data = 100 → 0.85.
        self.assertAlmostEqual(s.acceptance, 0.85)
        # SN above cut = 85 / 5 = 17.
        self.assertIsNotNone(s.sn_ratio)
        assert s.sn_ratio is not None  # for type narrowing
        self.assertAlmostEqual(s.sn_ratio, 17.0)
        self.assertAlmostEqual(s.n_above_data, 85.0)

    def test_no_noise_above_cut_gives_none_sn(self) -> None:
        s = sp.cut_stats(self.noise, self.data, self.edges, 8.5)
        # Bin 4 contributes 0 noise, bins above don't exist → above_noise = 0
        # The straddled bin (index 4, 8..10) carries 0 noise so above_noise = 0.
        self.assertEqual(sp.integral_above(self.noise, self.edges, 8.5), 0.0)
        self.assertIsNone(s.sn_ratio)
        # data still contributes from the straddled bin.
        self.assertGreater(s.n_above_data, 0.0)

    def test_empty_noise_sample_collapses_p_misfire_to_zero(self) -> None:
        s = sp.cut_stats([0.0] * 5, self.data, self.edges, 5.0)
        self.assertEqual(s.p_misfire, 0.0)
        # acceptance computed on data only, unaffected.
        self.assertGreater(s.acceptance, 0.0)
        self.assertIsNone(s.sn_ratio)


# ---------------------------------------------------------------------------
# noise_data_crossover
# ---------------------------------------------------------------------------

class CrossoverTests(unittest.TestCase):
    """Right-most crossover heuristic — picks the n_σ where the data
    step plot emerges from the noise floor going leftward (i.e. the
    last bin where data ≥ noise before noise re-dominates to the
    right)."""

    def setUp(self) -> None:
        self.edges = _edges(0.0, 5.0, 5)  # bin width 1

    def test_canonical_crossover_picked_right(self) -> None:
        # data ≥ noise at bin 2, noise > data at bin 3 → crossover at
        # bin-2 centre = 2.5.
        noise = [10.0, 6.0, 2.0, 4.0, 1.0]
        data =  [1.0, 2.0, 3.0, 1.0, 0.5]
        self.assertEqual(
            sp.noise_data_crossover(noise, data, self.edges), 2.5,
        )

    def test_no_crossover_returns_none(self) -> None:
        # noise dominates everywhere.
        noise = [10.0, 8.0, 7.0, 6.0, 5.0]
        data  = [1.0, 1.0, 1.0, 1.0, 1.0]
        self.assertIsNone(sp.noise_data_crossover(noise, data, self.edges))

    def test_rightmost_picked_when_multiple_crossovers(self) -> None:
        # Two crossovers — at bin 1 (1.5) and bin 3 (3.5).  Walk from
        # the right should pick 3.5.
        noise = [10.0, 1.0, 5.0, 1.0, 5.0]
        data  = [1.0, 5.0, 1.0, 5.0, 1.0]
        self.assertEqual(
            sp.noise_data_crossover(noise, data, self.edges), 3.5,
        )

    def test_degenerate_inputs(self) -> None:
        self.assertIsNone(sp.noise_data_crossover([], [], []))
        self.assertIsNone(sp.noise_data_crossover([1.0], [1.0], [0.0, 1.0]))


# ---------------------------------------------------------------------------
# seed_threshold
# ---------------------------------------------------------------------------

class SeedThresholdTests(unittest.TestCase):
    """The priority chain is the contract operators see the first time
    they open the dialog — pin every branch."""

    def setUp(self) -> None:
        self.edges = _edges(0.0, 5.0, 5)
        # Canonical: crossover lives at 2.5 (see CrossoverTests).
        self.noise = [10.0, 6.0, 2.0, 4.0, 1.0]
        self.data  = [1.0, 2.0, 3.0, 1.0, 0.5]

    def test_rundb_wins_when_positive(self) -> None:
        v = sp.seed_threshold(
            rundb_value=12.5, conf_value=3.0,
            noise_counts=self.noise, data_counts=self.data, edges=self.edges,
        )
        self.assertEqual(v, 12.5)

    def test_crossover_outranks_disable_sentinel_conf(self) -> None:
        v = sp.seed_threshold(
            rundb_value=0.0, conf_value=sp.QA_DISABLE_SENTINEL,
            noise_counts=self.noise, data_counts=self.data, edges=self.edges,
        )
        self.assertEqual(v, 2.5)

    def test_conf_wins_when_usable_and_rundb_zero(self) -> None:
        v = sp.seed_threshold(
            rundb_value=0.0, conf_value=4.2,
            noise_counts=self.noise, data_counts=self.data, edges=self.edges,
        )
        self.assertEqual(v, 4.2)

    def test_crossover_fallback_when_conf_also_unusable(self) -> None:
        # No rundb, conf is 0 (unusable per the > 0 check), crossover
        # available → take it.
        v = sp.seed_threshold(
            rundb_value=0.0, conf_value=0.0,
            noise_counts=self.noise, data_counts=self.data, edges=self.edges,
        )
        self.assertEqual(v, 2.5)

    def test_ultimate_fallback_when_nothing_available(self) -> None:
        # No rundb, no conf, no crossover (noise dominates everywhere).
        flat_noise = [10.0] * 5
        flat_data  = [1.0] * 5
        v = sp.seed_threshold(
            rundb_value=0.0, conf_value=0.0,
            noise_counts=flat_noise, data_counts=flat_data, edges=self.edges,
        )
        self.assertEqual(v, 5.0)

    def test_negative_conf_treated_as_unusable(self) -> None:
        # Defensive: conf reads back as something nonsensical (< 0).
        # Should fall through to the crossover.
        v = sp.seed_threshold(
            rundb_value=0.0, conf_value=-1.0,
            noise_counts=self.noise, data_counts=self.data, edges=self.edges,
        )
        self.assertEqual(v, 2.5)


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
