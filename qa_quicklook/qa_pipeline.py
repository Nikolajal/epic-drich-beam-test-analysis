"""qa_quicklook/qa_pipeline.py — shifter-facing QA pipeline wrapper.

Runs the lightdata → recodata → recotrack writer chain in ``--QA`` mode
with stop-on-first-failure semantics, joblock guards against double-
launch races, and a distinct notification on completion so the shifter
knows the QA cascade finished.

Per-run writer config is sourced automatically: the lightdata stage is
passed ``--run-database run-lists/<YYYY>.database.toml`` (the run id's
campaign year), so the writer reconstructs each run in the ALCOR op_mode
(LET / ToT) and at the streaming n_sigma threshold recorded for it —
no per-run CLI.  Absent database → writer defaults (LET, no override).

Design ground rules (locked 2026-05-29 from the workflow audit):

    --max-spill default     4             (caps shifter wall at ~3 min)
    notify backend           macos / linux libnotify / windows silent
    manifest location        Data/<RUN>/qa/qa_pipeline_manifest.json
    reuse-existing-output    skip stage with "nothing to do"
    error handling           stop on first failure, exit codes 10/20/30
                             ranges per stage so callers can route

CLI
---

::

    qa_pipeline RUN_ID
      [--data-repo PATH]      # defaults to <repo>/Data
      [--stages STAGES]       # comma-separated subset; default = all 3
      [--max-spill N]         # default 4; 0 → no cap (every spill)
      [--force-rebuild]       # forwarded to each stage
      [--force-upstream]      # cascades rebuild into earlier stages
      [--notify CSV]          # default 'macos,stdout,file' (auto-falls
                              # back per platform)
      [--json]                # emit progress events as JSON lines
      [--dry-run]             # print argv per stage and exit

Module entry-point (`run_pipeline`) is also callable in-process from
the dashboard so the Qt button can drive the same logic with progress
callbacks instead of subprocess parsing.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Callable, Optional

from . import joblock

#  Stage exit-code ranges — surface in CLI return code so callers
#  (cron, dashboard) can route by failing stage.  0 = success.
EXIT_OK = 0
EXIT_LOCK_STOLEN = 2
EXIT_RUN_MISSING = 3
EXIT_LIGHTDATA_BASE = 10
EXIT_RECODATA_BASE = 20
EXIT_RECOTRACK_BASE = 30

#  Names + binaries for each writer.  Order is the pipeline order:
#  recodata reads lightdata.root; recotrack reads recodata.root.
_STAGE_SPECS = [
    {
        "name": "lightdata",
        "binary": "build/bin/lightdata_writer",
        "output": "lightdata.root",
        "exit_base": EXIT_LIGHTDATA_BASE,
        "supports_max_spill": True,
        #  Only lightdata reads the per-run database (--run-database): the
        #  writer resolves the ALCOR op_mode (LET / ToT) AND the streaming
        #  n_sigma threshold per run from it.  recodata / recotrack don't.
        "supports_run_database": True,
    },
    {
        "name": "recodata",
        "binary": "build/bin/recodata_writer",
        "output": "recodata.root",
        "exit_base": EXIT_RECODATA_BASE,
        "supports_max_spill": True,
    },
    {
        "name": "recotrack",
        "binary": "build/bin/recotrackdata_writer",
        "output": "recotrackdata.root",
        "exit_base": EXIT_RECOTRACK_BASE,
        "supports_max_spill": False,
    },
]

_RUN_ID_RE = re.compile(r"^\d{8}-\d{6}$")

_WRITER_LABEL = "qa_pipeline"

#  Public tuple of stage names, in execution order — exported so
#  dashboards / dashboards-style consumers can lay out per-stage
#  widgets without duplicating the spec table.
STAGE_NAMES: tuple[str, ...] = tuple(s["name"] for s in _STAGE_SPECS)


# ---------------------------------------------------------------------------
# Progress-line parser — the symmetric inverse of the --json emitter
# in main().  Lives here so the wire format has exactly one owner.
# ---------------------------------------------------------------------------

def parse_progress_line(line: str) -> Optional[dict]:
    """Parse one line of qa_pipeline ``--json`` stdout.

    Returns the inner event dict (``{stage, state, exit_code, wall_s,
    new_pdfs}``) if the line is a well-formed qa_pipeline progress
    event, else None.  Lines that aren't JSON, are JSON but lack the
    ``qa_pipeline`` envelope, or are missing the required ``stage`` /
    ``state`` fields are silently ignored — a chained writer may
    interleave stray informational lines onto stdout under --json and
    the reader must not choke on them.
    """
    line = line.strip()
    if not line or line[0] != "{":
        return None
    try:
        doc = json.loads(line)
    except ValueError:
        return None
    if not isinstance(doc, dict):
        return None
    ev = doc.get("qa_pipeline")
    if not isinstance(ev, dict):
        return None
    if not isinstance(ev.get("stage"), str):
        return None
    if not isinstance(ev.get("state"), str):
        return None
    return ev


def consume_progress_buffer(buffer: str) -> tuple[list[dict], str]:
    """Drain complete lines from ``buffer``, returning ``(events, rest)``.

    Callers stream stdout chunks from QProcess (which delivers
    arbitrary-sized byte slices, not line-aligned) and must buffer
    across reads.  This helper does the line-splitting + parsing in one
    pass: events from all complete lines are returned, and the
    trailing incomplete fragment is handed back to be prepended to the
    next chunk.
    """
    events: list[dict] = []
    while "\n" in buffer:
        line, buffer = buffer.split("\n", 1)
        ev = parse_progress_line(line)
        if ev is not None:
            events.append(ev)
    return events, buffer


# ---------------------------------------------------------------------------
# Data structures.
# ---------------------------------------------------------------------------

@dataclass
class StageResult:
    """One stage's outcome, surfaced into the manifest + status return."""
    name: str
    state: str  #  "ok" | "skipped" | "failed" | "not_run"
    exit_code: Optional[int] = None
    wall_s: Optional[float] = None
    new_pdfs: list[str] = field(default_factory=list)
    reason: str = ""


@dataclass
class PipelineOptions:
    run_id: str
    data_repo: Path
    stages: list[str]
    max_spill: int  #  0 → no cap
    threads: int    #  0 → writer's auto-detect (typically over-subscribes)
    force_rebuild: bool
    force_upstream: bool
    notify: set[str]
    emit_json: bool
    dry_run: bool
    clean: bool = False  #  purge regenerable QA artifacts before running


@dataclass
class PipelineResult:
    run_id: str
    stages: list[StageResult]
    manifest_path: Optional[Path]
    wall_s: float
    exit_code: int


# ---------------------------------------------------------------------------
# Manifest + PDF discovery.
# ---------------------------------------------------------------------------

def _qa_dir_for(stage_name: str) -> str:
    """Map stage name to ``qa/<dir>`` subpath."""
    return {
        "lightdata": "qa/lightdata",
        "recodata":  "qa/recodata",
        "recotrack": "qa/recotrack",
    }[stage_name]


#  Run-directory entries that ``--clean`` is allowed to remove.  Kept
#  deliberately narrow: only artifacts the pipeline itself regenerates
#  on the next run.  Raw DAQ device directories (``rdo-*`` / ``kc705-*``),
#  calibration files (``fine_calib*`` / ``fine_calibration.root`` /
#  ``gold_*`` / ``timing_fine_calib.*``), and anything not matched here
#  are NEVER touched — a clean-slate test must not nuke its own inputs.
#
#  This is the SAFE, reusable clean: it does not know about historical
#  refactor-baseline snapshots (``*_baseline/`` etc.); those are a
#  one-off purge done outside the pipeline.
_CLEAN_REMOVE_FILES = (
    "lightdata.root",
    "recodata.root",
    "recotrackdata.root",
    ".DS_Store",
)
#  Stray per-histogram PDFs the recodata helpers dump into the run root
#  (radial_fit / sigma_vs_n_fit emit ``<run>/h_*.pdf`` directly — the
#  curated qa/<stage>/*.pdf set is separate).  Glob-matched.
_CLEAN_REMOVE_GLOBS = (
    "h_*.pdf",
)


def clean_run_dir(
    run_dir: Path,
    log: Callable[[str], None],
    *,
    dry_run: bool = False,
) -> int:
    """Remove regenerable QA artifacts so the next run starts clean.

    Returns the number of entries removed.  Allowlist-protected: only
    the writer output roots, the ``qa/`` tree, and stray run-root
    ``h_*.pdf`` are touched.  Raw device dirs + calibration files are
    never matched, so a mis-fire can't destroy a run's inputs.
    """
    removed = 0

    def _rm(path: Path) -> None:
        nonlocal removed
        if dry_run:
            log(f"qa_pipeline[clean]: would remove {path.name}")
            removed += 1
            return
        try:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
            log(f"qa_pipeline[clean]: removed {path.name}")
            removed += 1
        except OSError as e:
            log(f"qa_pipeline[clean]: WARNING could not remove {path.name}: {e}")

    #  qa/ output tree (every stage's curated PDFs + manifest).
    qa_dir = run_dir / "qa"
    if qa_dir.is_dir():
        _rm(qa_dir)
    #  Writer output roots + misc.
    for name in _CLEAN_REMOVE_FILES:
        p = run_dir / name
        if p.exists():
            _rm(p)
    #  Stray run-root PDFs.
    for pattern in _CLEAN_REMOVE_GLOBS:
        for p in sorted(run_dir.glob(pattern)):
            _rm(p)

    log(f"qa_pipeline[clean]: {removed} entry(ies) "
        f"{'would be ' if dry_run else ''}removed from {run_dir.name}")
    return removed


def _snapshot_pdfs(run_dir: Path) -> dict[str, set[str]]:
    """Snapshot the current PDF set per stage so we can compute the delta."""
    snap: dict[str, set[str]] = {}
    for stage in _STAGE_SPECS:
        qa_dir = run_dir / _qa_dir_for(stage["name"])
        snap[stage["name"]] = {
            p.name for p in qa_dir.glob("*.pdf")
        } if qa_dir.is_dir() else set()
    return snap


def _new_pdfs(
    run_dir: Path, stage_name: str, before: dict[str, set[str]],
) -> list[str]:
    """List PDF basenames that landed in this stage's qa-dir since ``before``."""
    qa_dir = run_dir / _qa_dir_for(stage_name)
    if not qa_dir.is_dir():
        return []
    after = {p.name for p in qa_dir.glob("*.pdf")}
    return sorted(after - before.get(stage_name, set()))


# ---------------------------------------------------------------------------
# Notification — distinct from the manual flow.
# ---------------------------------------------------------------------------

def _fmt_duration(seconds: float) -> str:
    """Human-readable duration: 90 → '1m 30s', 45 → '45s', 3661 → '1h 1m 1s'."""
    s = int(round(seconds))
    if s < 60:
        return f"{s}s"
    m, s = divmod(s, 60)
    if m < 60:
        return f"{m}m {s}s" if s else f"{m}m"
    h, m = divmod(m, 60)
    out = f"{h}h"
    if m:
        out += f" {m}m"
    if s:
        out += f" {s}s"
    return out


def _notify_macos(title: str, body: str) -> None:
    try:
        #  Plain FYI banner.  osascript's `display notification` is
        #  fire-and-forget — its "Show" button is inert by design — and
        #  that's fine: the operator opens the run in the dashboard.
        subprocess.Popen([
            "osascript", "-e",
            f'display notification "{body}" with title "{title}"',
        ])
        #  Glass.aiff for manual, Hero.aiff for auto-monitor.  This is
        #  the manual qa_pipeline path; the live-monitor wrapper uses
        #  its own Hero distinct from this.
        for snd in ("/System/Library/Sounds/Glass.aiff",
                    "/System/Library/Sounds/Ping.aiff"):
            if Path(snd).is_file():
                subprocess.Popen(["afplay", snd])
                break
    except OSError:
        pass


def _notify_linux(title: str, body: str) -> None:
    try:
        subprocess.Popen(["notify-send", "-u", "normal", title, body])
    except OSError:
        pass


def _notify_file(run_dir: Path, body: str) -> None:
    """Touch a ``.qa_pipeline_done`` marker the dashboard's fs-watcher can pick up."""
    marker = run_dir / "qa" / ".qa_pipeline_done"
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(body + "\n", encoding="utf-8")


def _notify_stdout(line: str) -> None:
    print(line, flush=True)


def _fire_notifications(
    *,
    backends: set[str],
    title: str,
    body: str,
    summary_line: str,
    run_dir: Path,
    banner: bool = True,
) -> None:
    """Multiplex the notification across the operator's chosen backends.

    ``banner=False`` suppresses only the system banner (macOS/Linux) while
    still emitting the stdout line and the ``.qa_pipeline_done`` file marker.
    Used for the whole-pipeline wrap-up when a single stage ran — its own
    per-stage banner already announced completion, so a second banner would
    just be noise.
    """
    sysname = platform.system()
    if "stdout" in backends or "all" in backends:
        _notify_stdout(summary_line)
    if "file" in backends or "all" in backends:
        _notify_file(run_dir, summary_line)
    if banner and ("macos" in backends or "all" in backends):
        if sysname == "Darwin":
            _notify_macos(title, body)
        elif sysname == "Linux":
            #  "macos" maps to libnotify when we're on Linux — same
            #  shifter intent (system banner), different backend.
            _notify_linux(title, body)
        #  Windows / headless: silently fall through; stdout + file
        #  still carry the news.


def _fire_stage_notification(
    *,
    backends: set[str],
    stage_name: str,
    result: "StageResult",
    run_id: str,
) -> None:
    """Per-writer system banner — ping as each individual stage finishes.

    Covers single-writer runs (``--stages lightdata``) and gives per-stage
    progress on a full cascade.  Fires ONLY the system banner: stdout already
    carries the ``[stage] done in …`` line from :func:`_run_stage`, and the
    ``.qa_pipeline_done`` file marker is reserved for whole-pipeline
    completion (the dashboard's fs-watcher keys off it), so neither is
    duplicated here.
    """
    #  Only announce stages that actually ran.  Dry-run / excluded stages
    #  have no wall time, so there's nothing to report (and _fmt_duration
    #  would choke on a None).
    if result.wall_s is None:
        return
    if result.state == "ok":
        n = len(result.new_pdfs)
        title = f"✓ {stage_name} done · {run_id}"
        body = (f"{stage_name} finished in {_fmt_duration(result.wall_s)}"
                f" · {n} new PDF{'s' if n != 1 else ''}")
    elif result.state == "failed":
        title = f"✗ {stage_name} failed · {run_id}"
        body = f"{stage_name} failed (exit {result.exit_code})"
    else:
        return  # not_run / skipped — nothing to announce
    if not ("macos" in backends or "all" in backends):
        return
    sysname = platform.system()
    if sysname == "Darwin":
        _notify_macos(title, body)
    elif sysname == "Linux":
        _notify_linux(title, body)


# ---------------------------------------------------------------------------
# Stage execution.
# ---------------------------------------------------------------------------

def _build_stage_argv(
    stage: dict, opts: PipelineOptions, repo_root: Path,
) -> list[str]:
    """Construct the writer's argv from stage spec + pipeline options."""
    argv = [
        str(repo_root / stage["binary"]),
        str(opts.data_repo),
        opts.run_id,
        "--QA",
    ]
    if opts.max_spill > 0 and stage["supports_max_spill"]:
        argv.extend(["--max-spill", str(opts.max_spill)])
    if opts.threads > 0:
        #  Default --threads 4 measured 2026-05-29 to beat both writer
        #  auto-detect (which picks ~16 on a 10-core box → over-subscribed
        #  and slower) AND explicit --threads 10.  The framer is
        #  bottlenecked on ROOT I/O / gROOT-mutex contention, not on
        #  CPU, so few threads doing productive reads beats many
        #  threads contending.  Operators can pass --threads 0 for the
        #  writer.s auto-detect on machines with very different
        #  topology.
        argv.extend(["--threads", str(opts.threads)])
    #  Per-run writer config from the campaign-year database (default on).
    #  lightdata_writer resolves the ALCOR op_mode (LET=1 / ToT=4 / …) and the
    #  streaming n_sigma threshold for this run from `--run-database`, so a run
    #  tagged `op_mode = 4` reconstructs in ToT automatically — no per-run CLI.
    #  Convention: run id `YYYYMMDD-HHMMSS` → `run-lists/<YYYY>.database.toml`.
    #  Skipped silently if that file is absent (→ writer defaults: LET, no
    #  threshold override) and for stages that don't read it.
    if stage.get("supports_run_database") and len(opts.run_id) >= 4:
        run_db = repo_root / "run-lists" / f"{opts.run_id[:4]}.database.toml"
        if run_db.is_file():
            argv.extend(["--run-database", str(run_db)])
    if opts.force_rebuild:
        argv.append("--force-rebuild")
    if opts.force_upstream:
        argv.append("--force-upstream")
    return argv


def _stage_output_exists(stage: dict, opts: PipelineOptions) -> bool:
    """Skip-existing-output predicate: does the stage's output already exist?"""
    return (opts.data_repo / opts.run_id / stage["output"]).is_file()


def _run_stage(
    stage: dict,
    opts: PipelineOptions,
    repo_root: Path,
    pdf_snapshot_before: dict[str, set[str]],
    log_callback: Optional[Callable[[str], None]] = None,
) -> StageResult:
    """Execute one stage of the pipeline.  Returns ``StageResult``."""
    name = stage["name"]
    log = log_callback or (lambda _msg: None)

    #  Skip-stage logic per design.
    if not opts.force_rebuild and _stage_output_exists(stage, opts):
        log(f"[{name}] skipped — existing output preserved")
        return StageResult(
            name=name, state="skipped", reason="existing output preserved",
        )

    argv = _build_stage_argv(stage, opts, repo_root)
    log(f"[{time.strftime('%H:%M:%S')}] [{name}] running: {' '.join(argv)}")

    if opts.dry_run:
        return StageResult(name=name, state="ok", reason="dry-run")

    start = time.time()
    try:
        proc = subprocess.run(
            argv,
            cwd=str(repo_root),
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError as exc:
        return StageResult(
            name=name, state="failed",
            exit_code=stage["exit_base"],
            reason=f"binary not found: {exc}",
        )
    wall_s = time.time() - start

    if proc.returncode != 0:
        #  Surface the last 200 lines of stderr (or stdout) to help the
        #  shifter understand what blew up without digging through logs.
        tail = proc.stderr or proc.stdout or ""
        tail_lines = tail.splitlines()[-200:]
        log("\n".join(tail_lines))
        return StageResult(
            name=name, state="failed",
            exit_code=stage["exit_base"] + min(proc.returncode, 9),
            wall_s=wall_s,
            reason=f"exit {proc.returncode}",
        )

    run_dir = opts.data_repo / opts.run_id
    new_pdfs = _new_pdfs(run_dir, name, pdf_snapshot_before)
    log(f"[{name}] done in {_fmt_duration(wall_s)} — {len(new_pdfs)} new PDF(s)")
    return StageResult(
        name=name, state="ok", exit_code=0, wall_s=wall_s, new_pdfs=new_pdfs,
    )


# ---------------------------------------------------------------------------
# Joblock guard.
# ---------------------------------------------------------------------------

def _acquire_lock(run_id: str) -> Optional[joblock.JobLock]:
    """Acquire the qa_pipeline lock for this run.

    Refuses to start if any of {lightdata, recodata, recotrack,
    qa_pipeline} has a live lock for this run.  Returns the lock on
    success, None on failure (caller should bail with EXIT_LOCK_STOLEN).
    """
    live = []
    for writer in ("lightdata_writer", "recodata_writer",
                   "recotrackdata_writer", _WRITER_LABEL):
        lock = joblock.read_lock(writer, run_id)
        if lock is not None:
            state = joblock.effective_state(lock)
            if state == joblock.EFFECTIVE_RUNNING:
                live.append((writer, lock.pid))
    if live:
        for writer, pid in live:
            sys.stderr.write(
                f"qa_pipeline: refusing to start — {writer} alive on "
                f"{run_id} (PID {pid})\n"
            )
        return None
    lock = joblock.JobLock(
        writer=_WRITER_LABEL,
        run=run_id,
        pid=os.getpid(),
        argv=list(sys.argv),
        started_at=joblock.now_iso(),
        state=joblock.EFFECTIVE_RUNNING,
    )
    joblock.write_lock(lock)
    return lock


def _release_lock(
    run_id: str, success: bool, exit_code: int,
) -> None:
    joblock.update_lock(
        _WRITER_LABEL, run_id,
        state=joblock.EFFECTIVE_SUCCESS if success else joblock.EFFECTIVE_ERROR,
        exit_code=exit_code,
        finished_at=joblock.now_iso(),
    )


# ---------------------------------------------------------------------------
# Public entry-point.
# ---------------------------------------------------------------------------

def run_pipeline(
    opts: PipelineOptions,
    repo_root: Path,
    progress_callback: Optional[Callable[[dict], None]] = None,
    log_callback: Optional[Callable[[str], None]] = None,
) -> PipelineResult:
    """Run the QA pipeline.  Returns a structured result.

    ``progress_callback`` fires per stage transition with a small
    event dict ``{stage, state, exit_code, wall_s, new_pdfs}``.  Used
    by the dashboard's button.

    ``log_callback`` is invoked with human-readable lines.  Default
    prints to stdout when running as CLI.
    """
    log = log_callback or (lambda msg: print(msg, flush=True))
    progress = progress_callback or (lambda _ev: None)

    run_dir = opts.data_repo / opts.run_id
    if not _RUN_ID_RE.match(opts.run_id):
        log(f"qa_pipeline: bad run id {opts.run_id!r} — expected YYYYMMDD-HHMMSS")
        return PipelineResult(opts.run_id, [], None, 0.0, EXIT_RUN_MISSING)
    if not run_dir.is_dir():
        log(f"qa_pipeline: run dir not found: {run_dir}")
        return PipelineResult(opts.run_id, [], None, 0.0, EXIT_RUN_MISSING)

    lock = _acquire_lock(opts.run_id)
    if lock is None:
        return PipelineResult(opts.run_id, [], None, 0.0, EXIT_LOCK_STOLEN)

    #  Clean slate: purge regenerable QA artifacts BEFORE the snapshot
    #  so the new-PDF delta is computed against an empty baseline (every
    #  emitted PDF reads as "new").  Allowlist-protected — never touches
    #  device dirs or calibration files.  Runs inside the lock so a
    #  concurrent cascade can't race the deletion.
    if opts.clean:
        clean_run_dir(run_dir, log, dry_run=opts.dry_run)

    snapshot = _snapshot_pdfs(run_dir)
    stage_results: list[StageResult] = []
    overall_start = time.time()
    overall_exit = EXIT_OK

    try:
        for stage in _STAGE_SPECS:
            if stage["name"] not in opts.stages:
                stage_results.append(
                    StageResult(name=stage["name"], state="not_run",
                                reason="excluded by --stages"))
                continue
            progress({"stage": stage["name"], "state": "started"})
            result = _run_stage(stage, opts, repo_root, snapshot, log)
            stage_results.append(result)
            progress({
                "stage": stage["name"],
                "state": result.state,
                "exit_code": result.exit_code,
                "wall_s": result.wall_s,
                "new_pdfs": result.new_pdfs,
            })
            #  Per-writer banner: only for an explicit single-writer run
            #  (`--stages lightdata`).  In a full cascade the individual
            #  stages are an implementation detail — the operator wants ONE
            #  "QA done" banner at the end (fired by the wrap-up below), not a
            #  separate ping as each writer finishes.  The wrap-up banner is
            #  reciprocally suppressed for single-stage runs (banner=n_ran>1),
            #  so exactly one banner fires either way.
            if len(opts.stages) == 1:
                _fire_stage_notification(
                    backends=opts.notify, stage_name=stage["name"],
                    result=result, run_id=opts.run_id,
                )
            if result.state == "failed":
                overall_exit = result.exit_code or stage["exit_base"]
                break  # stop-on-first-failure
    finally:
        wall_s = time.time() - overall_start
        manifest_path = _write_manifest(
            run_dir, opts, stage_results, wall_s, overall_exit,
        )
        success = overall_exit == EXIT_OK
        _release_lock(opts.run_id, success, overall_exit)

        #  Fire notifications regardless of success — distinct title
        #  per outcome so the shifter reads it at a glance.
        #
        #  Sweep audit (2026-05-30): the run id goes in the TITLE too,
        #  not just the body.  On macOS the banner header is the only
        #  glanceable field — a shifter watching multiple parallel
        #  cascades couldn't tell which run just finished without
        #  expanding the notification.
        if success:
            title = f"✓ QA done · {opts.run_id}"
            body = f"qa_pipeline finished in {_fmt_duration(wall_s)}"
        else:
            failed = next(
                (s for s in stage_results if s.state == "failed"), None,
            )
            who = failed.name if failed else "unknown"
            title = f"✗ QA failed · {opts.run_id}"
            body = f"{who} failed (exit {overall_exit})"
        summary = (
            f"[qa_pipeline] run={opts.run_id} "
            f"status={'ok' if success else 'failed'} "
            f"stages={sum(1 for s in stage_results if s.state == 'ok')}"
            f"/{len(stage_results)} "
            f"pdfs={sum(len(s.new_pdfs) for s in stage_results)} "
            f"wall={_fmt_duration(wall_s)}"
        )
        #  Whole-pipeline wrap-up.  The file marker (.qa_pipeline_done) and the
        #  stdout summary always fire; the system banner is suppressed when only
        #  a single stage ran, since its own per-stage banner already announced
        #  the result and a second "QA done" banner would just be noise.
        n_ran = sum(1 for s in stage_results if s.state in ("ok", "failed"))
        _fire_notifications(
            backends=opts.notify, title=title, body=body,
            summary_line=summary, run_dir=run_dir,
            banner=n_ran > 1,
        )

    return PipelineResult(
        run_id=opts.run_id,
        stages=stage_results,
        manifest_path=manifest_path,
        wall_s=wall_s,
        exit_code=overall_exit,
    )


# ---------------------------------------------------------------------------
# Manifest writer.
# ---------------------------------------------------------------------------

def _write_manifest(
    run_dir: Path,
    opts: PipelineOptions,
    stages: list[StageResult],
    wall_s: float,
    exit_code: int,
) -> Optional[Path]:
    """Write Data/<RUN>/qa/qa_pipeline_manifest.json."""
    qa_dir = run_dir / "qa"
    try:
        qa_dir.mkdir(parents=True, exist_ok=True)
    except OSError:
        return None
    path = qa_dir / "qa_pipeline_manifest.json"
    payload = {
        "schema": "qa_pipeline.v1",
        "run_id": opts.run_id,
        "wall_s": wall_s,
        "exit_code": exit_code,
        "options": {
            "stages": opts.stages,
            "max_spill": opts.max_spill,
            "force_rebuild": opts.force_rebuild,
            "force_upstream": opts.force_upstream,
        },
        "stages": [asdict(s) for s in stages],
    }
    try:
        path.write_text(json.dumps(payload, indent=2))
    except OSError:
        return None
    return path


# ---------------------------------------------------------------------------
# argparse CLI + main().
# ---------------------------------------------------------------------------

def _build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="qa_pipeline",
        description="Run the QA pipeline (lightdata → recodata → recotrack) "
                    "with --QA on a single run id.",
    )
    p.add_argument("run_id", help="YYYYMMDD-HHMMSS run id under Data/")
    p.add_argument("--data-repo", default=None,
                   help="Data repo root (defaults to <project>/Data)")
    p.add_argument("--stages", default="lightdata,recodata,recotrack",
                   help="Comma-separated subset of stages to run")
    p.add_argument("--max-spill", type=int, default=4,
                   help="--max-spill forwarded to each stage; 0 → no cap")
    p.add_argument("--threads", type=int, default=4,
                   help="--threads forwarded to each stage; default 4 "
                        "beats writer auto-detect on the typical 10-core "
                        "macOS box per 2026-05-29 measurement (16 → 4: -13%% "
                        "wall, -5GB peak memory).  Pass 0 to use the "
                        "writer's auto-detect instead.")
    p.add_argument("--force-rebuild", action="store_true",
                   help="Forward --force-rebuild to every stage")
    p.add_argument("--force-upstream", action="store_true",
                   help="Forward --force-upstream — cascades into earlier stages")
    p.add_argument("--notify", default="macos,stdout,file",
                   help="Comma-separated notify backends (macos|stdout|file|all)")
    p.add_argument("--json", action="store_true",
                   help="Emit one JSON event per stage transition on stdout")
    p.add_argument("--dry-run", action="store_true",
                   help="Print the argv per stage and exit without running")
    p.add_argument("--clean", action="store_true",
                   help="Purge regenerable QA artifacts (qa/ tree, writer "
                        "output roots, stray run-root h_*.pdf) before "
                        "running, so the test starts from a clean slate.  "
                        "Allowlist-protected: never touches raw device dirs "
                        "or calibration files.  Honours --dry-run.")
    return p


def main(argv: Optional[list[str]] = None) -> int:
    """CLI entry-point.  Returns the exit code; main() also returns it."""
    args = _build_argparser().parse_args(argv)
    #  Resolve the repo root via the module location — qa_quicklook/
    #  always sits one level below it.
    repo_root = Path(__file__).resolve().parent.parent
    data_repo = (
        Path(args.data_repo).resolve() if args.data_repo
        else repo_root / "Data"
    )
    opts = PipelineOptions(
        run_id=args.run_id,
        data_repo=data_repo,
        stages=[s.strip() for s in args.stages.split(",") if s.strip()],
        max_spill=args.max_spill,
        threads=args.threads,
        force_rebuild=args.force_rebuild,
        force_upstream=args.force_upstream,
        notify={s.strip() for s in args.notify.split(",") if s.strip()},
        emit_json=args.json,
        dry_run=args.dry_run,
        clean=args.clean,
    )

    def _json_progress(ev: dict) -> None:
        if opts.emit_json:
            print(json.dumps({"qa_pipeline": ev}), flush=True)

    result = run_pipeline(
        opts, repo_root,
        progress_callback=_json_progress,
    )
    return result.exit_code


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())


__all__ = [
    "EXIT_LIGHTDATA_BASE",
    "EXIT_LOCK_STOLEN",
    "EXIT_OK",
    "EXIT_RECODATA_BASE",
    "EXIT_RECOTRACK_BASE",
    "EXIT_RUN_MISSING",
    "PipelineOptions",
    "PipelineResult",
    "STAGE_NAMES",
    "StageResult",
    "consume_progress_buffer",
    "main",
    "parse_progress_line",
    "run_pipeline",
]
