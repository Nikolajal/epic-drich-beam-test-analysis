"""Tests for ``qa_quicklook.qa_pipeline``.

Skip-stage + manifest + exit-code coverage.  We don't actually run the
C++ writers — they're stubbed via ``subprocess.run`` monkeypatch.
The pipeline itself is what we're testing: stage iteration, error
propagation, manifest content, joblock acquire/release.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Iterator
from unittest.mock import MagicMock

import pytest

from qa_quicklook import qa_pipeline, joblock


# ---------------------------------------------------------------------------
# Fixtures — minimal run directory + stubbed subprocess.
# ---------------------------------------------------------------------------

@pytest.fixture
def run_dir(tmp_path: Path) -> Path:
    """A tmp Data/<run_id>/ skeleton with the qa/ subtree pre-created."""
    run_id = "20260101-100000"
    rd = tmp_path / run_id
    rd.mkdir()
    for stage in ("lightdata", "recodata", "recotrack"):
        (rd / "qa" / stage).mkdir(parents=True)
    return rd


@pytest.fixture
def opts(tmp_path: Path) -> qa_pipeline.PipelineOptions:
    return qa_pipeline.PipelineOptions(
        run_id="20260101-100000",
        data_repo=tmp_path,
        stages=["lightdata", "recodata", "recotrack"],
        max_spill=15,
        threads=4,
        force_rebuild=False,
        force_upstream=False,
        notify=set(),
        emit_json=False,
        dry_run=False,
    )


@pytest.fixture
def fake_subprocess_ok(monkeypatch: pytest.MonkeyPatch):
    """Stub subprocess.run to always return exit 0 + create the output file."""
    def _fake(argv, **kwargs):
        #  Detect which writer is being invoked by argv[0].
        binary = Path(argv[0]).name
        run_dir = Path(argv[1]) / argv[2]
        run_dir.mkdir(parents=True, exist_ok=True)
        output_map = {
            "lightdata_writer":      run_dir / "lightdata.root",
            "recodata_writer":       run_dir / "recodata.root",
            "recotrackdata_writer":  run_dir / "recotrackdata.root",
        }
        if binary in output_map:
            output_map[binary].write_bytes(b"fake")
        rv = MagicMock()
        rv.returncode = 0
        rv.stdout = ""
        rv.stderr = ""
        return rv
    monkeypatch.setattr(qa_pipeline.subprocess, "run", _fake)


@pytest.fixture
def fake_subprocess_fail_lightdata(monkeypatch: pytest.MonkeyPatch):
    """Stub: lightdata fails (exit 3), other writers never run."""
    def _fake(argv, **kwargs):
        binary = Path(argv[0]).name
        if binary == "lightdata_writer":
            rv = MagicMock()
            rv.returncode = 3
            rv.stdout = ""
            rv.stderr = "lightdata exploded\n"
            return rv
        raise AssertionError(f"unexpected stage invoked: {binary}")
    monkeypatch.setattr(qa_pipeline.subprocess, "run", _fake)


@pytest.fixture(autouse=True)
def isolated_joblock(monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    """Redirect the joblock cache to a tmp dir so tests don't litter ~/.cache."""
    cache = tmp_path / "joblock_cache"
    cache.mkdir()
    monkeypatch.setattr(joblock, "CACHE_DIR", cache)


# ---------------------------------------------------------------------------
# Happy-path tests.
# ---------------------------------------------------------------------------

class TestHappyPath:
    def test_full_run_writes_manifest(
        self, run_dir, opts, fake_subprocess_ok, tmp_path,
    ) -> None:
        result = qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        assert result.exit_code == qa_pipeline.EXIT_OK
        assert all(s.state == "ok" for s in result.stages)
        manifest = result.manifest_path
        assert manifest is not None and manifest.is_file()
        payload = json.loads(manifest.read_text())
        assert payload["schema"] == "qa_pipeline.v1"
        assert payload["run_id"] == "20260101-100000"
        assert payload["exit_code"] == 0
        assert len(payload["stages"]) == 3

    def test_dry_run_no_subprocess(
        self, run_dir, opts, tmp_path, monkeypatch,
    ) -> None:
        opts.dry_run = True
        # subprocess.run MUST NOT be called — if it is, we want a loud
        # AssertionError.
        def _boom(*args, **kwargs):
            raise AssertionError("dry-run invoked subprocess")
        monkeypatch.setattr(qa_pipeline.subprocess, "run", _boom)
        result = qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        assert result.exit_code == qa_pipeline.EXIT_OK
        assert all(s.state == "ok" for s in result.stages)


# ---------------------------------------------------------------------------
# Skip-stage behaviour.
# ---------------------------------------------------------------------------

class TestSkipStage:
    def test_existing_lightdata_root_skips_lightdata(
        self, run_dir, opts, fake_subprocess_ok, tmp_path,
    ) -> None:
        (run_dir / "lightdata.root").write_bytes(b"already there")
        result = qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        lightdata = next(s for s in result.stages if s.name == "lightdata")
        assert lightdata.state == "skipped"
        assert "existing output preserved" in lightdata.reason
        assert result.exit_code == qa_pipeline.EXIT_OK

    def test_force_rebuild_overrides_skip(
        self, run_dir, opts, fake_subprocess_ok, tmp_path,
    ) -> None:
        (run_dir / "lightdata.root").write_bytes(b"already there")
        opts.force_rebuild = True
        result = qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        lightdata = next(s for s in result.stages if s.name == "lightdata")
        assert lightdata.state == "ok"  # ran despite existing output

    def test_stages_subset_runs_only_selected(
        self, run_dir, opts, fake_subprocess_ok, tmp_path,
    ) -> None:
        opts.stages = ["recodata"]
        result = qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        states = {s.name: s.state for s in result.stages}
        assert states["lightdata"] == "not_run"
        assert states["recodata"] == "ok"
        assert states["recotrack"] == "not_run"


# ---------------------------------------------------------------------------
# Failure path.
# ---------------------------------------------------------------------------

class TestFailure:
    def test_lightdata_failure_stops_pipeline(
        self, run_dir, opts, fake_subprocess_fail_lightdata, tmp_path,
    ) -> None:
        result = qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        # Exit code in the lightdata range (10-19).
        assert qa_pipeline.EXIT_LIGHTDATA_BASE <= result.exit_code < qa_pipeline.EXIT_RECODATA_BASE
        # The lightdata stage failed; recodata + recotrack never ran.
        states = {s.name: s.state for s in result.stages}
        assert states["lightdata"] == "failed"
        assert states.get("recodata") is None or states["recodata"] == "not_run"

    def test_missing_run_dir_exits_3(
        self, opts, tmp_path,
    ) -> None:
        opts.run_id = "20260102-100000"  # never created
        result = qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        assert result.exit_code == qa_pipeline.EXIT_RUN_MISSING


# ---------------------------------------------------------------------------
# Joblock guard.
# ---------------------------------------------------------------------------

class TestJoblockGuard:
    def test_live_writer_lock_refuses_start(
        self, run_dir, opts, tmp_path, monkeypatch,
    ) -> None:
        """A live lightdata_writer lock on the same run blocks qa_pipeline."""
        #  Write a "running" lock for lightdata_writer with our own PID
        #  so the liveness check passes.
        import os
        live_lock = joblock.JobLock(
            writer="lightdata_writer", run=opts.run_id,
            pid=os.getpid(), argv=["lightdata_writer"],
            started_at=joblock.now_iso(),
        )
        joblock.write_lock(live_lock)
        result = qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        assert result.exit_code == qa_pipeline.EXIT_LOCK_STOLEN
        assert result.stages == []

    def test_releases_lock_on_success(
        self, run_dir, opts, fake_subprocess_ok, tmp_path,
    ) -> None:
        qa_pipeline.run_pipeline(opts, repo_root=tmp_path)
        my_lock = joblock.read_lock("qa_pipeline", opts.run_id)
        assert my_lock is not None
        assert my_lock.state == joblock.EFFECTIVE_SUCCESS
        assert my_lock.exit_code == 0


# ---------------------------------------------------------------------------
# Progress-event parser — wire format the dashboard reads back from
# ``qa_pipeline --json`` stdout to drive the live status-bar strip.
# ---------------------------------------------------------------------------

class TestParseProgressLine:
    def test_started_event(self) -> None:
        ev = qa_pipeline.parse_progress_line(
            '{"qa_pipeline": {"stage": "lightdata", "state": "started"}}'
        )
        assert ev == {"stage": "lightdata", "state": "started"}

    def test_finished_event_carries_wall_and_pdfs(self) -> None:
        line = (
            '{"qa_pipeline": {"stage": "recodata", "state": "ok", '
            '"exit_code": 0, "wall_s": 12.4, '
            '"new_pdfs": ["a.pdf", "b.pdf"]}}'
        )
        ev = qa_pipeline.parse_progress_line(line)
        assert ev is not None
        assert ev["stage"] == "recodata"
        assert ev["state"] == "ok"
        assert ev["exit_code"] == 0
        assert ev["wall_s"] == pytest.approx(12.4)
        assert ev["new_pdfs"] == ["a.pdf", "b.pdf"]

    def test_blank_line_returns_none(self) -> None:
        assert qa_pipeline.parse_progress_line("") is None
        assert qa_pipeline.parse_progress_line("   \n") is None

    def test_non_json_returns_none(self) -> None:
        #  Some other writer accidentally printed to stdout under --json.
        assert qa_pipeline.parse_progress_line(
            "[lightdata] done in 12.4s"
        ) is None

    def test_malformed_json_returns_none(self) -> None:
        assert qa_pipeline.parse_progress_line('{"qa_pipeline": ') is None

    def test_other_envelope_returns_none(self) -> None:
        #  Different writer using the same JSON-lines convention.
        assert qa_pipeline.parse_progress_line(
            '{"livemon": {"stage": "lightdata", "state": "started"}}'
        ) is None

    def test_envelope_value_not_dict_returns_none(self) -> None:
        assert qa_pipeline.parse_progress_line(
            '{"qa_pipeline": "started"}'
        ) is None

    def test_missing_state_returns_none(self) -> None:
        assert qa_pipeline.parse_progress_line(
            '{"qa_pipeline": {"stage": "lightdata"}}'
        ) is None

    def test_missing_stage_returns_none(self) -> None:
        assert qa_pipeline.parse_progress_line(
            '{"qa_pipeline": {"state": "started"}}'
        ) is None


class TestConsumeProgressBuffer:
    def test_three_complete_lines_yield_three_events(self) -> None:
        buf = (
            '{"qa_pipeline": {"stage": "lightdata", "state": "started"}}\n'
            '{"qa_pipeline": {"stage": "lightdata", "state": "ok", '
            '"exit_code": 0, "wall_s": 1.0, "new_pdfs": []}}\n'
            '{"qa_pipeline": {"stage": "recodata", "state": "started"}}\n'
        )
        events, rest = qa_pipeline.consume_progress_buffer(buf)
        assert rest == ""
        assert [e["stage"] for e in events] == [
            "lightdata", "lightdata", "recodata",
        ]
        assert [e["state"] for e in events] == ["started", "ok", "started"]

    def test_partial_line_retained_in_buffer(self) -> None:
        #  Simulates QProcess delivering a chunk that splits a JSON
        #  line halfway — the parser MUST hold the fragment back, not
        #  emit a half-event or crash.
        buf = (
            '{"qa_pipeline": {"stage": "lightdata", "state": "started"}}\n'
            '{"qa_pipeline": {"stage": "lightd'
        )
        events, rest = qa_pipeline.consume_progress_buffer(buf)
        assert len(events) == 1
        assert events[0]["stage"] == "lightdata"
        assert rest == '{"qa_pipeline": {"stage": "lightd'

    def test_resume_with_remainder_emits_pending_event(self) -> None:
        #  The classic stream pattern: feed chunk1, get residual,
        #  prepend residual to chunk2, drain again.  Both events fire.
        chunk1 = '{"qa_pipeline": {"stage": "lightd'
        chunk2 = (
            'ata", "state": "ok", "exit_code": 0, '
            '"wall_s": 2.5, "new_pdfs": []}}\n'
        )
        events1, residual = qa_pipeline.consume_progress_buffer(chunk1)
        assert events1 == []
        events2, rest = qa_pipeline.consume_progress_buffer(residual + chunk2)
        assert rest == ""
        assert len(events2) == 1
        assert events2[0]["stage"] == "lightdata"
        assert events2[0]["state"] == "ok"
        assert events2[0]["wall_s"] == pytest.approx(2.5)

    def test_noise_lines_dropped(self) -> None:
        #  A wrapper script that tees a "starting…" banner ahead of
        #  the pipeline's own stdout must not desync the strip.
        buf = (
            "starting qa_pipeline…\n"
            '{"qa_pipeline": {"stage": "lightdata", "state": "started"}}\n'
            "warning: temp file left behind\n"
            '{"qa_pipeline": {"stage": "lightdata", "state": "ok", '
            '"exit_code": 0, "wall_s": 1.0, "new_pdfs": ["x.pdf"]}}\n'
        )
        events, rest = qa_pipeline.consume_progress_buffer(buf)
        assert rest == ""
        assert [e["state"] for e in events] == ["started", "ok"]
        assert events[1]["new_pdfs"] == ["x.pdf"]

    def test_empty_buffer_returns_nothing(self) -> None:
        events, rest = qa_pipeline.consume_progress_buffer("")
        assert events == []
        assert rest == ""


class TestStageNamesExport:
    def test_stage_names_in_execution_order(self) -> None:
        #  Dashboards lay out per-stage widgets keyed on this tuple;
        #  reordering it is a wire-format break for the strip.
        assert qa_pipeline.STAGE_NAMES == (
            "lightdata", "recodata", "recotrack",
        )


class TestCleanRunDir:
    """``clean_run_dir`` is the allowlist-protected purge behind both
    ``qa_pipeline --clean`` and the dashboard's Clear QA button.  It must
    drop every regenerable artefact while leaving raw DAQ device dirs and
    calibration files — a run's irreplaceable inputs — untouched."""

    @staticmethod
    def _populate(rd: Path) -> None:
        """Lay down a realistic mix of regenerable + protected entries."""
        #  Regenerable: qa/ render tree, writer output roots, stray PDFs.
        for stage in ("lightdata", "recodata", "recotrack"):
            (rd / "qa" / stage).mkdir(parents=True)
            (rd / "qa" / stage / "plot.pdf").write_bytes(b"pdf")
        (rd / "lightdata.root").write_bytes(b"root")
        (rd / "recodata.root").write_bytes(b"root")
        (rd / "recotrackdata.root").write_bytes(b"root")
        (rd / "h_radial_fit.pdf").write_bytes(b"pdf")
        (rd / "h_sigma_vs_n.pdf").write_bytes(b"pdf")
        #  Protected: raw device dir + calibration files + a kept PDF
        #  that isn't an h_-prefixed stray.
        (rd / "rdo-0").mkdir()
        (rd / "rdo-0" / "spill_000.dat").write_bytes(b"raw")
        (rd / "fine_calibration.root").write_bytes(b"calib")
        (rd / "timing_fine_calib.txt").write_bytes(b"calib")
        (rd / "summary.pdf").write_bytes(b"pdf")

    def test_removes_regenerable_keeps_inputs(self, tmp_path: Path) -> None:
        rd = tmp_path / "20260101-100000"
        rd.mkdir()
        self._populate(rd)

        removed = qa_pipeline.clean_run_dir(rd, lambda _m: None)

        #  qa/ tree + 3 writer roots + 2 stray h_*.pdf = 6 entries.
        assert removed == 6
        assert not (rd / "qa").exists()
        assert not (rd / "lightdata.root").exists()
        assert not (rd / "recodata.root").exists()
        assert not (rd / "recotrackdata.root").exists()
        assert not (rd / "h_radial_fit.pdf").exists()
        assert not (rd / "h_sigma_vs_n.pdf").exists()
        #  Raw data + calibration + non-stray PDF survive untouched.
        assert (rd / "rdo-0" / "spill_000.dat").read_bytes() == b"raw"
        assert (rd / "fine_calibration.root").exists()
        assert (rd / "timing_fine_calib.txt").exists()
        assert (rd / "summary.pdf").exists()

    def test_pinned_run_is_not_exempt(self, tmp_path: Path) -> None:
        #  ``.qa_persistent`` guards RAW data against retention pruning;
        #  it must NOT shield regenerable QA.  The purge ignores it (and
        #  leaves the marker itself in place — it lives at the run root,
        #  outside the allowlist).
        rd = tmp_path / "20260101-110000"
        rd.mkdir()
        self._populate(rd)
        (rd / ".qa_persistent").touch()

        removed = qa_pipeline.clean_run_dir(rd, lambda _m: None)

        assert removed == 6
        assert not (rd / "qa").exists()
        assert not (rd / "lightdata.root").exists()
        assert (rd / ".qa_persistent").exists()

    def test_dry_run_deletes_nothing(self, tmp_path: Path) -> None:
        rd = tmp_path / "20260101-120000"
        rd.mkdir()
        self._populate(rd)

        removed = qa_pipeline.clean_run_dir(rd, lambda _m: None, dry_run=True)

        assert removed == 6
        assert (rd / "qa").exists()
        assert (rd / "lightdata.root").exists()
