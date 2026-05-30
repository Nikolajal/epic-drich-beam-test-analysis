"""Unit tests for ``qa_quicklook.cross_run_trends``.

Covers the four behaviours the General-tab tiles actually depend on:

  - ``select_recent_runs`` is lex-sort-descending → head → reverse, so
    a mixed batch yields the newest ``N`` in chronological order.
  - ``extract_series`` records missing runs (no sensor row, no
    quantity row) separately from present-but-zero values — the tile
    footer relies on this to distinguish "no data" from "data is 0".
  - The ``DCR rate per event`` derive falls back to "missing" when
    either input scalar is absent or ``n_events <= 0``, and never
    raises ZeroDivisionError on a fresh run.
  - ``read_trend_runs_n`` returns the operator's value on a valid
    config, the default on every failure mode (missing file, missing
    section, non-int value, parse error), and clamps non-positive
    overrides to 1 so the page never tries to render a 0-wide tile.

Run::

    .venv/bin/python -m unittest tests.test_cross_run_trends
"""

from __future__ import annotations

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook import cross_run_trends  # noqa: E402
from qa_quicklook import rundb  # noqa: E402
from qa_quicklook.cross_run_trends import (  # noqa: E402
    DEFAULT_METRICS,
    DEFAULT_TREND_RUNS_N,
    MetricSpec,
    TrendPoint,
    extract_series,
    load_trends,
    read_trend_runs_n,
    select_recent_runs,
)


def _write_synthetic_results(path: Path) -> None:
    """Drop a small ``standard_results.toml`` that covers the cases.

    Runs (chronological):

      - 20251111-100000 — earliest; only ``full.n_gamma``, no DCR pair.
      - 20260527-073111 — DCR pair present, ``full`` quantities absent.
      - 20260528-191848 — full record (n_gamma + sigma + DCR pair +
        streaming.n_fires).
      - 20260530-120000 — n_events == 0  (rate must NOT divide-by-zero;
        missing).

    Plus one "noise" run that lives under sensor ``"1350"`` only — used
    to confirm a sensor mismatch is recorded as missing, not silently
    skipped.
    """
    path.write_text(textwrap.dedent(
        """
        [results."20251111-100000".all."full.n_gamma"]
        value = 18.5
        error = 0.4

        [results."20260527-073111".all."lightdata.n_dcr_hits"]
        value = 159363.0

        [results."20260527-073111".all."lightdata.n_events"]
        value = 1521873.0

        [results."20260528-191848".all."full.n_gamma"]
        value = 24.1
        error = 0.6

        [results."20260528-191848".all."full.sigma"]
        value = 2.07

        [results."20260528-191848".all."lightdata.n_dcr_hits"]
        value = 50000.0

        [results."20260528-191848".all."lightdata.n_events"]
        value = 100000.0

        [results."20260528-191848".all."streaming.n_fires"]
        value = 482.0

        [results."20260530-120000".all."lightdata.n_dcr_hits"]
        value = 10.0

        [results."20260530-120000".all."lightdata.n_events"]
        value = 0.0

        [results."20260101-010101"."1350"."dcr.mean"]
        value = 2.7
        """
    ).strip())


class SelectRecentRunsTests(unittest.TestCase):
    def test_returns_newest_n_in_chronological_order(self) -> None:
        ids = [
            "20251111-100000",
            "20260530-120000",
            "20260101-010101",
            "20260527-073111",
            "20260528-191848",
        ]
        out = select_recent_runs(ids, n=3)
        # Newest three, oldest-to-newest.
        self.assertEqual(
            out,
            ["20260527-073111", "20260528-191848", "20260530-120000"],
        )

    def test_dedupes_ids(self) -> None:
        out = select_recent_runs(["a", "b", "a", "c"], n=10)
        # Sort puts c > b > a; window returns all three.
        self.assertEqual(out, ["a", "b", "c"])

    def test_non_positive_n_yields_empty(self) -> None:
        self.assertEqual(select_recent_runs(["a", "b"], n=0), [])
        self.assertEqual(select_recent_runs(["a", "b"], n=-5), [])


class ExtractSeriesTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self._path = Path(self._tmp.name) / "standard_results.toml"
        _write_synthetic_results(self._path)
        self._results = rundb.results_load(self._path)
        # Sanity — the fixture loaded.
        self.assertIn("20260528-191848", self._results)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_quantity_metric_pulls_value_and_error(self) -> None:
        metric = next(m for m in DEFAULT_METRICS if m.key == "n_photons")
        ids = ["20251111-100000", "20260528-191848"]
        series = extract_series(self._results, metric, ids)
        self.assertEqual(len(series.points), 2)
        self.assertEqual(series.points[0],
                         TrendPoint("20251111-100000", 18.5, 0.4))
        self.assertEqual(series.points[1],
                         TrendPoint("20260528-191848", 24.1, 0.6))
        self.assertEqual(series.missing, [])

    def test_missing_runs_recorded(self) -> None:
        metric = next(m for m in DEFAULT_METRICS if m.key == "n_photons")
        # 20260527-073111 has no ``full.n_gamma`` — must show up in missing.
        ids = ["20260527-073111", "20260528-191848"]
        series = extract_series(self._results, metric, ids)
        self.assertEqual([p.run_id for p in series.points],
                         ["20260528-191848"])
        self.assertEqual(series.missing, ["20260527-073111"])

    def test_sensor_mismatch_is_missing(self) -> None:
        # The fixture's "20260101-010101" run only has sensor "1350"
        # populated, so a sensor="all" lookup must record it as missing.
        metric = next(m for m in DEFAULT_METRICS if m.key == "n_photons")
        ids = ["20260101-010101"]
        series = extract_series(self._results, metric, ids)
        self.assertEqual(series.points, [])
        self.assertEqual(series.missing, ["20260101-010101"])

    def test_dcr_rate_derive_computes_ratio(self) -> None:
        metric = next(m for m in DEFAULT_METRICS if m.key == "dcr_rate")
        ids = ["20260528-191848"]
        series = extract_series(self._results, metric, ids)
        self.assertEqual(len(series.points), 1)
        self.assertAlmostEqual(series.points[0].value, 50000.0 / 100000.0)

    def test_dcr_rate_skips_zero_event_runs(self) -> None:
        metric = next(m for m in DEFAULT_METRICS if m.key == "dcr_rate")
        # 20260530-120000 has n_events == 0 — must be missing, not
        # a ZeroDivisionError.
        series = extract_series(
            self._results, metric, ["20260530-120000"],
        )
        self.assertEqual(series.points, [])
        self.assertEqual(series.missing, ["20260530-120000"])

    def test_streaming_fires_absent_on_legacy_runs(self) -> None:
        # The aspirational metric — writer doesn't publish it for the
        # legacy fixture runs, so every point should be missing.
        metric = next(
            m for m in DEFAULT_METRICS if m.key == "n_streaming_fires")
        ids = ["20251111-100000", "20260527-073111", "20260528-191848"]
        series = extract_series(self._results, metric, ids)
        # Only the run that explicitly published streaming.n_fires is
        # present.
        self.assertEqual([p.run_id for p in series.points],
                         ["20260528-191848"])
        self.assertEqual(
            series.missing,
            ["20251111-100000", "20260527-073111"],
        )


class LoadTrendsTests(unittest.TestCase):
    def test_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "standard_results.toml"
            _write_synthetic_results(path)
            out = load_trends(path, n_runs=5)
        # One TrendSeries per default metric, in order.
        self.assertEqual(
            [s.metric.key for s in out],
            [m.key for m in DEFAULT_METRICS],
        )
        # photon yield: two runs publish full.n_gamma (the earliest
        # 20251111-100000 + the full record 20260528-191848).
        n_photons = next(s for s in out if s.metric.key == "n_photons")
        self.assertEqual(
            [p.run_id for p in n_photons.points],
            ["20251111-100000", "20260528-191848"],
        )

    def test_missing_file_returns_empty_list(self) -> None:
        out = load_trends(Path("/this/path/does/not/exist.toml"))
        self.assertEqual(out, [])


class ReadTrendRunsNTests(unittest.TestCase):
    def _write(self, body: str) -> Path:
        d = Path(self._tmp.name)
        p = d / "qa_quicklook.toml"
        p.write_text(body)
        return p

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_reads_valid_override(self) -> None:
        p = self._write("[qa_general]\ntrend_runs_n = 30\n")
        self.assertEqual(read_trend_runs_n(p), 30)

    def test_missing_file_returns_default(self) -> None:
        self.assertEqual(
            read_trend_runs_n(Path(self._tmp.name) / "absent.toml"),
            DEFAULT_TREND_RUNS_N,
        )

    def test_missing_section_returns_default(self) -> None:
        p = self._write("[ui]\ntheme = 'dark'\n")
        self.assertEqual(read_trend_runs_n(p), DEFAULT_TREND_RUNS_N)

    def test_non_integer_value_returns_default(self) -> None:
        p = self._write("[qa_general]\ntrend_runs_n = 'lots'\n")
        self.assertEqual(read_trend_runs_n(p), DEFAULT_TREND_RUNS_N)

    def test_parse_error_returns_default(self) -> None:
        p = self._write("not [ valid toml ===")
        self.assertEqual(read_trend_runs_n(p), DEFAULT_TREND_RUNS_N)

    def test_non_positive_clamps_to_one(self) -> None:
        p = self._write("[qa_general]\ntrend_runs_n = -7\n")
        self.assertEqual(read_trend_runs_n(p), 1)
        p2 = self._write("[qa_general]\ntrend_runs_n = 0\n")
        self.assertEqual(read_trend_runs_n(p2), 1)


class MetricSpecValidationTests(unittest.TestCase):
    def test_must_supply_quantity_xor_derive(self) -> None:
        with self.assertRaises(ValueError):
            MetricSpec(key="oops", label="oops")
        with self.assertRaises(ValueError):
            MetricSpec(
                key="oops", label="oops",
                quantity="a.b", derive=lambda _q: (0.0, 0.0),
            )


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
