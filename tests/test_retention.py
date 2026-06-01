"""Tests for ``qa_quicklook.retention``.

Pure-function coverage — every fixture builds a minimal Data/<run>/ tree
on tmp_path that mirrors the real layout:

    Data/
      20260101-100000/
        rdo-196/decoded/alcdaq.fifo_24.root
        rdo-197/decoded/alcdaq.fifo_05.root
        lightdata.root
        recodata.root
        recotrackdata.root
        qa/lightdata/01_trigger_matrix.pdf
        qa/recodata/01_radial_fit.pdf

The lex sort on YYYYMMDD-HHMMSS doubles as a chronological sort, so
the tests can pick "newest" / "oldest" by name.

Run with ``.venv/bin/python -m pytest tests/test_retention.py``.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from qa_quicklook import retention


# ---------------------------------------------------------------------------
# Helpers — build the on-disk layout for a single fake run.
# ---------------------------------------------------------------------------

def _make_run(
    data_dir: Path,
    run_id: str,
    *,
    with_raw_decoded: bool = True,
    with_lightdata: bool = True,
    with_recodata: bool = True,
    with_recotrack: bool = True,
    with_pdfs: bool = True,
    qa_managed: bool = True,
) -> Path:
    """Create a single fake run directory under ``data_dir``.

    Each file is non-empty so the bytes-freed report is exercised.

    ``qa_managed`` defaults True so the tier-logic tests exercise a
    run the sweep is actually allowed to touch — the sweep NEVER
    prunes a run without the ``.qa_managed`` marker (see the
    exemption test).  Pass ``qa_managed=False`` to build a
    hand-placed / externally-acquired run.
    """
    run_dir = data_dir / run_id
    run_dir.mkdir(parents=True)
    if qa_managed:
        (run_dir / retention.QA_MANAGED_MARKER).touch()
    if with_raw_decoded:
        for rdo in ("rdo-196", "rdo-197"):
            decoded = run_dir / rdo / "decoded"
            decoded.mkdir(parents=True)
            (decoded / "alcdaq.fifo_24.root").write_bytes(b"x" * 1024)
    if with_lightdata:
        (run_dir / "lightdata.root").write_bytes(b"x" * 512)
    if with_recodata:
        (run_dir / "recodata.root").write_bytes(b"x" * 256)
    if with_recotrack:
        (run_dir / "recotrackdata.root").write_bytes(b"x" * 128)
    if with_pdfs:
        qa_light = run_dir / "qa" / "lightdata"
        qa_light.mkdir(parents=True)
        (qa_light / "01_trigger_matrix.pdf").write_bytes(b"x" * 64)
        qa_reco = run_dir / "qa" / "recodata"
        qa_reco.mkdir(parents=True)
        (qa_reco / "01_radial_fit.pdf").write_bytes(b"x" * 64)
    return run_dir


def _build_campaign(data_dir: Path, n_runs: int) -> list[Path]:
    """Build ``n_runs`` runs with increasing timestamps; return newest-first."""
    runs = [
        _make_run(data_dir, f"2026010{i // 10}-{i % 10:02d}0000")
        for i in range(1, n_runs + 1)
    ]
    return list(reversed(runs))  # newest first


# ---------------------------------------------------------------------------
# list_run_dirs — the lex sort + run-id filter.
# ---------------------------------------------------------------------------

class TestListRunDirs:
    def test_empty_dir(self, tmp_path: Path) -> None:
        assert retention.list_run_dirs(tmp_path) == []

    def test_missing_dir(self, tmp_path: Path) -> None:
        assert retention.list_run_dirs(tmp_path / "nonexistent") == []

    def test_newest_first(self, tmp_path: Path) -> None:
        _make_run(tmp_path, "20260101-100000")
        _make_run(tmp_path, "20260103-100000")
        _make_run(tmp_path, "20260102-100000")
        names = [p.name for p in retention.list_run_dirs(tmp_path)]
        assert names == ["20260103-100000", "20260102-100000", "20260101-100000"]

    def test_ignores_non_run_dirs(self, tmp_path: Path) -> None:
        _make_run(tmp_path, "20260101-100000")
        (tmp_path / "lost+found").mkdir()
        (tmp_path / "cache").mkdir()
        (tmp_path / "stale.txt").write_text("ignored")
        names = [p.name for p in retention.list_run_dirs(tmp_path)]
        assert names == ["20260101-100000"]


# ---------------------------------------------------------------------------
# sweep — the policy logic.  Pure: no I/O beyond iterdir.
# ---------------------------------------------------------------------------

class TestSweep:
    def test_empty_campaign(self, tmp_path: Path) -> None:
        plan = retention.sweep(tmp_path, full_keep_n=5, qa_keep_n=50)
        assert plan["full_keep"] == []
        assert plan["qa_only"] == []
        assert plan["fully_pruned"] == []

    def test_non_qa_managed_runs_never_touched(self, tmp_path: Path) -> None:
        """THE core invariant: a run without ``.qa_managed`` is NEVER
        pruned/demoted, regardless of age, no matter how far past the
        keep window it sits.  This is the whole reason the old
        ``enforce_qa_managed`` escape hatch was removed — hand-placed
        runs are the downloader's responsibility, full stop."""
        #  10 hand-placed runs (no .qa_managed), full_keep_n=1, qa_keep_n=2
        #  — under the old behaviour 8 of these would be fully pruned and
        #  1 demoted.  Now: ALL land in user_managed, none touched.
        for i in range(1, 11):
            _make_run(tmp_path, f"2026010{i // 10}-{i % 10:02d}0000",
                      qa_managed=False)
        plan = retention.sweep(tmp_path, full_keep_n=1, qa_keep_n=2)
        assert plan["full_keep"] == []
        assert plan["qa_only"] == []
        assert plan["fully_pruned"] == []
        assert len(plan["user_managed"]) == 10

    def test_qa_managed_marker_survives_qa_only_demotion(
        self, tmp_path: Path,
    ) -> None:
        """A QA-only demotion must NOT delete the ``.qa_managed`` /
        ``.qa_persistent`` markers — stripping them would un-manage /
        unpin the run on the next sweep."""
        _make_run(tmp_path, "20260101-100000")  # newest, full-keep
        older = _make_run(tmp_path, "20259901-100000")  # demoted to qa-only
        retention.pin_persistent(older)  # also drop .qa_persistent
        # pin moves it to the persistent bucket, so build a separate
        # non-pinned older run to exercise the demotion delete-list.
        older2 = _make_run(tmp_path, "20259801-100000")
        plan = retention.sweep(tmp_path, full_keep_n=1, qa_keep_n=50)
        demote = next(
            (e for e in plan["qa_only"]
             if e["run"] == str(older2)), None)
        assert demote is not None
        names = {Path(p).name for p in demote["delete"]}
        assert retention.QA_MANAGED_MARKER not in names
        assert retention.QA_PERSISTENT_MARKER not in names
        # and the raw decoded + lightdata.root ARE in the delete list.
        assert "lightdata.root" in names

    def test_all_within_full_keep(self, tmp_path: Path) -> None:
        runs = _build_campaign(tmp_path, n_runs=3)
        plan = retention.sweep(tmp_path, full_keep_n=5, qa_keep_n=50)
        # All 3 runs in full-keep, nothing demoted or pruned.
        assert len(plan["full_keep"]) == 3
        assert plan["qa_only"] == []
        assert plan["fully_pruned"] == []

    def test_split_full_keep_qa_only(self, tmp_path: Path) -> None:
        runs = _build_campaign(tmp_path, n_runs=8)
        plan = retention.sweep(tmp_path, full_keep_n=3, qa_keep_n=50)
        # 3 newest stay full; remaining 5 demoted to QA-only.
        assert len(plan["full_keep"]) == 3
        assert len(plan["qa_only"]) == 5
        assert plan["fully_pruned"] == []

    def test_beyond_qa_keep_pruned_entirely(self, tmp_path: Path) -> None:
        runs = _build_campaign(tmp_path, n_runs=10)
        plan = retention.sweep(tmp_path, full_keep_n=2, qa_keep_n=5)
        # 2 full + (5-2)=3 QA-only + (10-5)=5 fully pruned.
        assert len(plan["full_keep"]) == 2
        assert len(plan["qa_only"]) == 3
        assert len(plan["fully_pruned"]) == 5

    def test_qa_only_prunes_raw_decoded_and_lightdata(
        self, tmp_path: Path,
    ) -> None:
        runs = _build_campaign(tmp_path, n_runs=2)
        plan = retention.sweep(tmp_path, full_keep_n=1, qa_keep_n=50)
        assert len(plan["qa_only"]) == 1
        delete_names = {Path(p).name for p in plan["qa_only"][0]["delete"]}
        # Raw decoded device dirs + lightdata.root → in the delete list.
        assert "rdo-196" in delete_names
        assert "rdo-197" in delete_names
        assert "lightdata.root" in delete_names
        # qa/, recodata.root, recotrackdata.root → kept by default.
        assert "qa" not in delete_names
        assert "recodata.root" not in delete_names
        assert "recotrackdata.root" not in delete_names

    def test_qa_only_drops_recotrackdata_when_disabled(
        self, tmp_path: Path,
    ) -> None:
        runs = _build_campaign(tmp_path, n_runs=2)
        plan = retention.sweep(
            tmp_path, full_keep_n=1, qa_keep_n=50, keep_recotrackdata=False,
        )
        delete_names = {Path(p).name for p in plan["qa_only"][0]["delete"]}
        assert "recotrackdata.root" in delete_names
        assert "recodata.root" not in delete_names  # still kept

    def test_full_keep_n_clamped_to_zero(self, tmp_path: Path) -> None:
        _build_campaign(tmp_path, n_runs=5)
        plan = retention.sweep(tmp_path, full_keep_n=-3, qa_keep_n=10)
        assert plan["full_keep_n"] == 0
        assert plan["full_keep"] == []

    def test_qa_keep_n_clamped_to_full_keep(self, tmp_path: Path) -> None:
        _build_campaign(tmp_path, n_runs=5)
        # qa_keep_n < full_keep_n → clamped equal → no QA-only tier.
        plan = retention.sweep(tmp_path, full_keep_n=10, qa_keep_n=3)
        assert plan["qa_keep_n"] == 10
        assert len(plan["full_keep"]) == 5  # all of them
        assert plan["qa_only"] == []
        assert plan["fully_pruned"] == []

    def test_already_qa_only_run_skipped_from_demote_list(
        self, tmp_path: Path,
    ) -> None:
        """Run with no raw decoded files (already swept) → no qa_only entry."""
        _make_run(tmp_path, "20260101-100000")  # newest, full-keep
        _make_run(
            tmp_path, "20259901-100000",  # older
            with_raw_decoded=False,
            with_lightdata=False,
        )
        plan = retention.sweep(tmp_path, full_keep_n=1, qa_keep_n=50)
        # Older run has nothing to delete → no qa_only entry.
        assert plan["qa_only"] == []


# ---------------------------------------------------------------------------
# apply — the destructive action.
# ---------------------------------------------------------------------------

class TestApply:
    def test_empty_plan_no_op(self, tmp_path: Path) -> None:
        plan = retention.sweep(tmp_path, full_keep_n=5, qa_keep_n=50)
        report = retention.apply(plan, mtime_grace_s=0.0)
        assert report["n_deleted"] == 0
        assert report["bytes_freed"] == 0
        assert report["errors"] == []

    def test_qa_only_actually_deletes_raw_decoded(self, tmp_path: Path) -> None:
        _build_campaign(tmp_path, n_runs=2)
        plan = retention.sweep(tmp_path, full_keep_n=1, qa_keep_n=50)
        report = retention.apply(plan, mtime_grace_s=0.0)

        # _build_campaign produces "20260100-0i0000" via integer
        # arithmetic, so the QA-only entry (older = i=1) is named
        # "20260100-010000".  Schema-wise valid (regex matches the
        # 8-digit + dash + 6-digit shape); semantic month/day is
        # irrelevant for the sweep policy.
        older_run = tmp_path / "20260100-010000"
        assert not (older_run / "rdo-196").exists()
        assert not (older_run / "rdo-197").exists()
        assert not (older_run / "lightdata.root").exists()
        # QA-keep set survives.
        assert (older_run / "recodata.root").exists()
        assert (older_run / "recotrackdata.root").exists()
        assert (older_run / "qa" / "lightdata" / "01_trigger_matrix.pdf").exists()
        assert report["n_deleted"] > 0
        assert report["bytes_freed"] > 0

    def test_fully_pruned_removes_whole_run_dir(self, tmp_path: Path) -> None:
        _build_campaign(tmp_path, n_runs=4)
        plan = retention.sweep(tmp_path, full_keep_n=1, qa_keep_n=2)
        report = retention.apply(plan, mtime_grace_s=0.0)
        # 1 full + 1 QA-only + 2 fully-pruned (last two by age).
        assert len(list(tmp_path.iterdir())) == 2
        # Errors should be empty for a clean tmp_path.
        assert report["errors"] == []

    def test_full_keep_runs_untouched(self, tmp_path: Path) -> None:
        runs = _build_campaign(tmp_path, n_runs=5)
        # Snapshot the newest two runs' file lists before the sweep.
        newest = runs[0]
        before = sorted(p.name for p in newest.iterdir())
        plan = retention.sweep(tmp_path, full_keep_n=2, qa_keep_n=10)
        retention.apply(plan, mtime_grace_s=0.0)
        after = sorted(p.name for p in newest.iterdir())
        assert before == after  # nothing changed in the full-keep tier

    def test_errors_captured_not_raised(self, tmp_path: Path) -> None:
        """A nonexistent path in the plan should record an error, not raise."""
        plan = {
            "data_dir": str(tmp_path),
            "full_keep_n": 0,
            "qa_keep_n": 0,
            "keep_recotrackdata": True,
            "full_keep": [],
            "qa_only": [],
            "fully_pruned": [str(tmp_path / "does-not-exist")],
        }
        report = retention.apply(plan, mtime_grace_s=0.0)
        assert len(report["errors"]) == 1
        assert report["errors"][0]["path"].endswith("does-not-exist")


# ---------------------------------------------------------------------------
# format_plan_summary — log-friendly one-liner.
# ---------------------------------------------------------------------------

class TestFormatSummary:
    def test_summary_counts_match(self, tmp_path: Path) -> None:
        _build_campaign(tmp_path, n_runs=10)
        plan = retention.sweep(tmp_path, full_keep_n=2, qa_keep_n=5)
        summary = retention.format_plan_summary(plan)
        assert "2 full" in summary
        assert "3" in summary  # 3 QA-only (qa_keep − full_keep)
        assert "5" in summary  # 5 fully pruned (10 − qa_keep)
