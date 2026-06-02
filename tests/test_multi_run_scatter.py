"""Unit tests for ``qa_quicklook.multi_run_scatter``."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook import multi_run_scatter as mrs  # noqa: E402
from qa_quicklook.cross_run_trends import MetricSpec  # noqa: E402
from qa_quicklook.rundb import ResultEntry, RunRecord  # noqa: E402


def _results(**runs) -> dict:
    """Build a results_load-shaped dict: run -> {sensor: {quantity: entry}}.
    Each kwarg is run_id=value (sensor 'all', quantity 'q')."""
    out = {}
    for run_id, val in runs.items():
        out[run_id.replace("_", "-")] = {
            "all": {"q": ResultEntry(value=val, error=0.0)}
        }
    return out


def _record(run_id: str, **fields) -> RunRecord:
    return RunRecord(run_id=run_id, own_fields=dict(fields), inherited_fields={})


_METRIC = MetricSpec(key="q", label="Q", sensor="all", quantity="q")


class BuildScatterTests(unittest.TestCase):
    def test_joins_y_with_x_beam_info(self) -> None:
        results = _results(**{"20251101-100000": 5.0, "20251101-110000": 7.0})
        records = [
            _record("20251101-100000", v_bias=45.0),
            _record("20251101-110000", v_bias=48.0),
        ]
        run_ids = ["20251101-100000", "20251101-110000"]
        pts, skipped = mrs.build_scatter(results, records, run_ids, _METRIC, "v_bias")
        self.assertEqual(skipped, [])
        self.assertEqual({(p.x, p.y) for p in pts}, {(45.0, 5.0), (48.0, 7.0)})

    def test_optional_z_colour_axis(self) -> None:
        results = _results(**{"20251101-100000": 5.0})
        records = [_record("20251101-100000", v_bias=45.0, mirror_mm=12.0)]
        pts, _ = mrs.build_scatter(
            results, records, ["20251101-100000"], _METRIC, "v_bias", "mirror_mm")
        self.assertEqual(pts[0].z, 12.0)

    def test_skips_run_missing_y(self) -> None:
        results = _results(**{"20251101-100000": 5.0})  # only one has Y
        records = [
            _record("20251101-100000", v_bias=45.0),
            _record("20251101-110000", v_bias=48.0),
        ]
        pts, skipped = mrs.build_scatter(
            results, records, ["20251101-100000", "20251101-110000"], _METRIC, "v_bias")
        self.assertEqual(len(pts), 1)
        self.assertIn("20251101-110000", skipped)

    def test_skips_run_missing_or_nonnumeric_x(self) -> None:
        results = _results(**{"20251101-100000": 5.0, "20251101-110000": 7.0})
        records = [
            _record("20251101-100000", v_bias=45.0),
            _record("20251101-110000", v_bias="bad"),   # non-numeric
        ]
        pts, skipped = mrs.build_scatter(
            results, records, ["20251101-100000", "20251101-110000"], _METRIC, "v_bias")
        self.assertEqual([p.run_id for p in pts], ["20251101-100000"])
        self.assertIn("20251101-110000", skipped)

    def test_bool_x_is_not_coerced(self) -> None:
        #  A bool field must not sneak in as 0/1 on a numeric axis.
        results = _results(**{"20251101-100000": 5.0})
        records = [_record("20251101-100000", beam_status=True)]
        pts, skipped = mrs.build_scatter(
            results, records, ["20251101-100000"], _METRIC, "beam_status")
        self.assertEqual(pts, [])
        self.assertIn("20251101-100000", skipped)


class JitterTests(unittest.TestCase):
    def test_singletons_unchanged(self) -> None:
        pts = [mrs.ScatterPoint("r1", 1.0, 10.0), mrs.ScatterPoint("r2", 2.0, 20.0)]
        self.assertEqual(mrs.jitter_x(pts), [1.0, 2.0])

    def test_shared_x_is_spread_deterministically(self) -> None:
        pts = [mrs.ScatterPoint("rA", 5.0, 1.0), mrs.ScatterPoint("rB", 5.0, 2.0),
               mrs.ScatterPoint("rC", 9.0, 3.0)]
        a = mrs.jitter_x(pts)
        b = mrs.jitter_x(pts)
        self.assertEqual(a, b)                      # deterministic
        self.assertNotEqual(a[0], a[1])             # the two at x=5 separated
        self.assertEqual(a[2], 9.0)                 # singleton untouched
        #  Offsets bounded by spread_frac * span (span = 9-5 = 4).
        for off, p in zip(a, pts):
            self.assertLessEqual(abs(off - p.x), 0.012 * 4 + 1e-9)

    def test_empty(self) -> None:
        self.assertEqual(mrs.jitter_x([]), [])


class OverlayTests(unittest.TestCase):
    def test_one_series_per_runlist_in_order(self) -> None:
        results = _results(**{
            "20251101-100000": 5.0,
            "20251101-110000": 7.0,
            "20251102-100000": 9.0,
        })
        records = [
            _record("20251101-100000", v_bias=45.0),
            _record("20251101-110000", v_bias=48.0),
            _record("20251102-100000", v_bias=50.0),
        ]
        runlists = [
            ("A", ["20251101-100000", "20251101-110000"]),
            ("B", ["20251102-100000"]),
        ]
        out = mrs.build_overlay(results, records, runlists, _METRIC, x_field="v_bias")
        #  Series order preserved (drives stable colour assignment).
        self.assertEqual([name for name, _, _ in out], ["A", "B"])
        self.assertEqual(len(out[0][1]), 2)        # A → two points
        self.assertEqual(len(out[1][1]), 1)        # B → one point
        self.assertEqual(out[1][1][0].x, 50.0)

    def test_missing_data_surfaces_per_series_skipped(self) -> None:
        results = _results(**{"20251101-100000": 5.0})  # only one run has Y
        records = [
            _record("20251101-100000", v_bias=45.0),
            _record("20251101-110000", v_bias=48.0),
        ]
        out = mrs.build_overlay(
            results, records,
            [("A", ["20251101-100000", "20251101-110000"])],
            _METRIC, x_field="v_bias")
        _name, pts, skipped = out[0]
        self.assertEqual(len(pts), 1)
        self.assertIn("20251101-110000", skipped)


if __name__ == "__main__":
    unittest.main()
