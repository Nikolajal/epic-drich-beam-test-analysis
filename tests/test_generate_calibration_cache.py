"""PyROOT tests for the AlcorFinedata low-stats channel cache.

Exercises the cache added in BACKLOG row P 1.20 — channels whose
Y-projection is below the >=250-entries gate are remembered so
subsequent `generate_calibration` calls skip the per-channel
ProjectionY allocation.  Without the cache, the per-spill `--QA`
cascade pays ~10–15 s/spill of redundant projection work.

The tests drive the cache through synthetic ``TH2F`` inputs and
inspect the static instrumentation getters
(``get_low_stats_cache_size``, ``get_low_stats_cached_skips``) — no
real run data is required.

Skipped automatically when the built C++ library is missing (CI fast-
lint job, fresh clone before `cmake --build`) so the suite still
passes on environments without ROOT or the project's build artefacts.
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LIB_PATH = REPO_ROOT / "build" / "libbeam_test_analysis.dylib"
if not LIB_PATH.exists():
    # CI/lint environments without a build aren't the target of this
    # test — skip the whole module rather than fail import.
    raise unittest.SkipTest(
        f"built library not found at {LIB_PATH}; "
        "run `cmake --build build` to populate it before this test runs"
    )

try:
    import ROOT  # noqa: E402
except ImportError as exc:  # pragma: no cover — exercised only without ROOT
    raise unittest.SkipTest(f"PyROOT not importable: {exc}")

# Quiet down ROOT — the per-channel sigmoid fit prints Minuit chatter
# the user can't act on at unittest verbosity.
ROOT.gROOT.SetBatch(True)
ROOT.gErrorIgnoreLevel = ROOT.kWarning

# Load the project library so cppyy sees AlcorFinedata.  Idempotent —
# `gSystem.Load` returns 1 on repeat without re-linking.
if ROOT.gSystem.Load(str(LIB_PATH)) < 0:  # pragma: no cover — env issue
    raise unittest.SkipTest(f"gSystem.Load returned an error for {LIB_PATH}")


# ---------------------------------------------------------------------------
# Synthetic histogram helpers.
#
# X axis is the dense `GlobalIndex::tdc_ordinal()` — ordinal 0 is the
# very first valid channel (device 192, fifo 0, chip 0, channel 0,
# tdc 0).  Y axis is the 256-bin fine value.  Every test builds a
# histogram with N x-bins; the first ``n_high`` channels get
# >=250 entries (high stats), the rest get a configurable
# below-threshold count (low stats).
# ---------------------------------------------------------------------------

LOW_STATS_THRESHOLD = 250  # mirrors the gate inside generate_calibration


def _make_synthetic_th2f(
    *,
    n_channels: int,
    n_high_stats: int,
    low_stats_entries: int = 50,
    high_stats_entries: int = 600,
    name: str = "h_synth",
) -> "ROOT.TH2F":
    """Build a (channel, fine) histogram with mixed-stats channels.

    The Y profile we put down for high-stats channels is a clean
    square pulse between fine bins 20 and 90 — that's where the
    sigmoid fit places its edge seeds.  We don't *require* the fit
    to converge (the cache logic runs independently of the
    convergence verdict), so the precise shape doesn't matter; we
    just want enough entries to clear the >=250 gate.
    """
    h = ROOT.TH2F(name, name, n_channels, 0, n_channels, 256, 0, 256)
    h.SetDirectory(0)  # don't leak into gDirectory between tests
    for xbin in range(1, n_channels + 1):
        ordinal = xbin - 1  # tdc_ordinal()
        if ordinal < n_high_stats:
            # Plateau between fine bins 20 and 90 — gives the edge
            # finder something to seed on.
            per_bin = max(1, high_stats_entries // 70)
            for fbin in range(20, 90):
                # 1-indexed: bin centres at 20.5..89.5
                h.SetBinContent(xbin, fbin + 1, float(per_bin))
        else:
            # Distribute `low_stats_entries` thinly across the
            # spectrum.  Total bin-content sum is what TH1::GetEntries
            # returns after SetBinContent (no Fill), which is what the
            # >=250 gate inspects.
            assert low_stats_entries < LOW_STATS_THRESHOLD, (
                "test misuse: low_stats_entries must be below the gate"
            )
            for fbin in range(low_stats_entries):
                h.SetBinContent(xbin, fbin + 10, 1.0)
        # GetEntries() after SetBinContent reports 0 by default —
        # ProjectionY computes entries from the bin sum, but the
        # 250-entry gate inside generate_calibration calls
        # `projection->GetEntries()`.  Force-set so the test
        # mirrors how a real fill would behave.
    # ``GetEntries`` after SetBinContent only updates if we tell it.
    # The production code never relies on this for the parent TH2F
    # (it projects per-channel), but ProjectionY copies the sum.  To
    # make GetEntries on the projection match the bin sum we use
    # ResetStats so ROOT recomputes stats from the bin contents the
    # next time they're queried.
    h.ResetStats()
    return h


def _reset_calibration_state() -> None:
    """Drop every static cache so tests don't bleed into each other."""
    ROOT.AlcorFinedata.clear_low_stats_cache()
    ROOT.AlcorFinedata.set_low_stats_retry_period(0)
    # `generate_calibration(..., overwrite=true)` clears the
    # parameter maps too — call it with an empty histogram to wipe
    # everything left over from a prior test.  Empty TH2F is fine:
    # the loop skips xbin range = 0.
    empty = ROOT.TH2F("h_reset", "h_reset", 1, 0, 1, 256, 0, 256)
    empty.SetDirectory(0)
    ROOT.AlcorFinedata.generate_calibration(empty, True)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class LowStatsCachePopulatesTests(unittest.TestCase):
    """First call (overwrite=true) populates the cache from low-stats skips."""

    def setUp(self) -> None:
        _reset_calibration_state()

    def test_cache_populates_on_first_call(self) -> None:
        # 20 channels: first 5 high-stats, remaining 15 low-stats.
        n_total, n_high = 20, 5
        h = _make_synthetic_th2f(n_channels=n_total, n_high_stats=n_high)
        ROOT.AlcorFinedata.generate_calibration(h, True)
        # All 15 low-stats channels should be cached; high-stats
        # channels either calibrate or fail edge-span, but never
        # enter the low-stats cache.
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cache_size(),
            n_total - n_high,
            "every low-stats channel should be cached after the first pass",
        )
        # Counter resets on overwrite=true and no cache hits happen
        # on the first call (cache was empty), so the cumulative
        # cached-skip counter must be 0.
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(), 0,
            "no cache hits expected on the first (cleared-cache) call",
        )


class LowStatsCacheEarlyExitTests(unittest.TestCase):
    """Second call with overwrite=false short-circuits via the cache."""

    def setUp(self) -> None:
        _reset_calibration_state()

    def test_second_call_uses_cache(self) -> None:
        n_total, n_high = 30, 4
        n_low = n_total - n_high
        h = _make_synthetic_th2f(n_channels=n_total, n_high_stats=n_high)

        # Populate.
        ROOT.AlcorFinedata.generate_calibration(h, True)
        cache_after_seed = ROOT.AlcorFinedata.get_low_stats_cache_size()
        self.assertEqual(cache_after_seed, n_low)

        # Re-run with overwrite=false: every low-stats channel should
        # be skipped via the cache (one increment per skip), so the
        # cumulative cached-skip counter equals the cache size.
        ROOT.AlcorFinedata.generate_calibration(h, False)
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(),
            n_low,
            "second call must short-circuit every cached channel",
        )
        # Cache size must not shrink — these channels stay cached.
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cache_size(),
            n_low,
            "cache size must persist across non-overwrite calls",
        )

    def test_overwrite_true_clears_cache(self) -> None:
        n_total, n_high = 12, 3
        h = _make_synthetic_th2f(n_channels=n_total, n_high_stats=n_high)
        ROOT.AlcorFinedata.generate_calibration(h, True)
        self.assertGreater(ROOT.AlcorFinedata.get_low_stats_cache_size(), 0)
        # An overwrite=true call wipes the cache AND the counters,
        # then re-populates from the same histogram in the same call.
        ROOT.AlcorFinedata.generate_calibration(h, True)
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(), 0,
            "overwrite=true must reset the cumulative cached-skip counter",
        )
        # Same low-stats channels re-discovered → cache is the same size.
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cache_size(),
            n_total - n_high,
        )


class LowStatsRetryPeriodTests(unittest.TestCase):
    """`set_low_stats_retry_period(N)` drops the cache every N-th call."""

    def setUp(self) -> None:
        _reset_calibration_state()

    def test_period_zero_never_invalidates(self) -> None:
        n_total, n_high = 8, 2
        h = _make_synthetic_th2f(n_channels=n_total, n_high_stats=n_high)
        ROOT.AlcorFinedata.set_low_stats_retry_period(0)  # default: infinity
        ROOT.AlcorFinedata.generate_calibration(h, True)
        for _ in range(5):
            ROOT.AlcorFinedata.generate_calibration(h, False)
        # Cache must still be populated — period=0 means never retry.
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cache_size(),
            n_total - n_high,
        )
        # Every non-overwrite call short-circuited every cached
        # channel: 5 calls × (n_total - n_high) skips.
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(),
            5 * (n_total - n_high),
        )

    def test_period_two_invalidates_every_other_call(self) -> None:
        # With period=2, clearing happens at the START of the 2nd,
        # 4th, ... non-overwrite call.  We start the policy AFTER an
        # overwrite=true seed call, which doesn't touch the
        # non-overwrite call counter.
        n_total, n_high = 8, 2
        n_low = n_total - n_high
        h = _make_synthetic_th2f(n_channels=n_total, n_high_stats=n_high)
        ROOT.AlcorFinedata.generate_calibration(h, True)  # seed
        ROOT.AlcorFinedata.set_low_stats_retry_period(2)

        # Call 1 (count → 1, 1%2 != 0): no clear, all cached → +n_low skips.
        ROOT.AlcorFinedata.generate_calibration(h, False)
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(), n_low
        )

        # Call 2 (count → 2, 2%2 == 0): clear the cache BEFORE the
        # channel loop, then re-populate by re-discovering every
        # low-stats channel.  No cache hits this call → counter
        # unchanged.
        ROOT.AlcorFinedata.generate_calibration(h, False)
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(), n_low,
            "retry call must not add cached-skip counts",
        )
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cache_size(), n_low,
            "retry call must re-populate from the same histogram",
        )

        # Call 3: no clear (3%2 != 0), all cached → +n_low.
        ROOT.AlcorFinedata.generate_calibration(h, False)
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(), 2 * n_low
        )

        # Call 4: clear again (4%2 == 0), no cache hits added.
        ROOT.AlcorFinedata.generate_calibration(h, False)
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(), 2 * n_low
        )

    def test_period_one_clears_every_call(self) -> None:
        # period=1 = "always retry" — every non-overwrite call drops
        # the cache, so no call should ever register a cached skip.
        n_total, n_high = 6, 1
        h = _make_synthetic_th2f(n_channels=n_total, n_high_stats=n_high)
        ROOT.AlcorFinedata.generate_calibration(h, True)
        ROOT.AlcorFinedata.set_low_stats_retry_period(1)
        for _ in range(4):
            ROOT.AlcorFinedata.generate_calibration(h, False)
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cached_skips(), 0,
            "period=1 must clear the cache on every call",
        )
        # Cache is still re-populated each call — final state is full.
        self.assertEqual(
            ROOT.AlcorFinedata.get_low_stats_cache_size(),
            n_total - n_high,
        )


if __name__ == "__main__":  # pragma: no cover — manual driver
    unittest.main()
