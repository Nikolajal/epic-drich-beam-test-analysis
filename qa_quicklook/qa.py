"""QA tab — three sub-tabs: General (chaptered overview) / Full plots / Macros.

Layout
------
Top: shared run picker (one source of truth across sub-tabs).
Body: one ``_StepQaPage`` per pipeline step, plus a Macros placeholder.

Each step page renders two stacked sections:

  1. **PDFs** — any ``<run>/qa/<step>/*.pdf`` the writer dropped on
     disk are loaded via ``PySide6.QtPdf`` and shown inline.  Empty
     when the writer hasn't been taught to emit PDFs yet.  This is
     the "writers print pdfs that we pick up" path — keeps the
     dashboard out of plot-rendering business when the writer can
     own the look-and-feel directly.

  2. **Histograms** — fallback grid: every TH1/TH2 in the step's
     canonical ``.root`` file rendered via uproot + matplotlib.
     Click → modal with the full-size figure.  Useful while writers
     don't yet emit PDFs; can stay as the "browse everything" path
     even after PDFs land.

Macros sub-tab is a placeholder until we wire bespoke ROOT macros in.
"""

from __future__ import annotations

import os
import weakref
from pathlib import Path
from typing import Optional

from PySide6 import QtCore, QtGui, QtWidgets

from . import cross_run_trends
from . import qa_pipeline
from . import rundb
from . import thumbs


def _spawn_root_canvas(root_path: Path, hist_path: str) -> bool:
    """Open one histogram in a real ROOT TCanvas (no Terminal in the loop).

    Spawns ``build/bin/qa_tcanvas`` — a standalone binary that owns
    its ``TApplication`` + ``TCanvas`` + GUI event loop (see
    ``macros/utilities/qa_tcanvas.cpp``).  Same architecture as
    ``qa_tbrowser``: ROOT gets a real interactive session without
    the dashboard having to inflict a Terminal window on the
    operator (the prior osascript path popped Terminal.app every
    click, which the user explicitly asked to avoid).

    Returns True iff the spawn was accepted by Qt (false → binary
    missing; the modal's matplotlib path is still fully functional).
    """
    #  Walk up from ``Data/<run>/foo.root`` → ``<repo>`` so the
    #  binary path is reproducible regardless of the operator's
    #  cwd at launch time.
    repo_root = root_path.parent.parent.parent
    binary = repo_root / "build" / "bin" / "qa_tcanvas"
    if not binary.is_file():
        return False
    argv = [binary.as_posix(), root_path.as_posix(), hist_path]
    return QtCore.QProcess.startDetached(argv[0], argv[1:])


_PLOTS_THEME_OVERRIDE: Optional[str] = None  # set by set_plots_theme_override()


def set_plots_theme_override(value: Optional[str]) -> None:
    """Pin the plot theme independently of the Qt UI palette.

    ``value`` is one of ``"light"``, ``"dark"``, or ``"follow"`` /
    ``None`` (which reverts to the auto-follow heuristic).  Driven by
    ``[ui].plots_theme`` in ``qa_quicklook.toml`` so an operator can
    keep a dark UI but force light QA plots (or vice versa) when
    projector / printer / contrast preferences need it.  Lives at
    module scope because ``_dark_theme_active`` is consulted from
    many widget constructors deep in the QA tree — passing the
    setting down every level would be noise.
    """
    global _PLOTS_THEME_OVERRIDE
    if value is None:
        _PLOTS_THEME_OVERRIDE = None
        return
    v = str(value).strip().lower()
    _PLOTS_THEME_OVERRIDE = v if v in ("light", "dark") else None


def _dark_theme_active() -> bool:
    """True iff QA plots should render in dark mode.

    Resolution order:

      1. Explicit override from ``[ui].plots_theme`` via
         ``set_plots_theme_override`` — wins for any value other
         than ``"follow"``.
      2. Heuristic on the Qt window palette — covers the
         ``"follow"`` case + the no-app-yet case (returns False).

    Used to flip matplotlib's axis/label colours so the figures stay
    readable inside the active card background.
    """
    if _PLOTS_THEME_OVERRIDE == "dark":
        return True
    if _PLOTS_THEME_OVERRIDE == "light":
        return False
    app = QtWidgets.QApplication.instance()
    if app is None:
        return False
    bg = app.palette().color(QtGui.QPalette.Window)
    return bg.lightness() < 128


# One ``STEPS`` entry per pipeline step.  Each entry is:
#   (key, label, canonical_root, qa_subdir).
# ``canonical_root`` is the file we walk for TH1/TH2 thumbnails;
# ``qa_subdir`` is where we look for writer-emitted PDFs.  Order
# matches the pipeline (raw → reco → reco-track → standalone calib).
#
# Used by the "Full plots" deep-dive sub-tab.  The topic-first surface
# (Triggers / Pixel noise / Calibration / etc.) re-aggregates these
# PDFs through ``TOPICS`` + ``topic_of`` below.
STEPS: tuple[tuple[str, str, str, str], ...] = (
    ("lightdata",   "Lightdata",     "lightdata.root",       "qa/lightdata"),
    ("recodata",    "Recodata",      "recodata.root",        "qa/recodata"),
    ("recotrack",   "Recotrack",     "recotrackdata.root",   "qa/recotrack"),
    ("calibration", "Pulser calib",  "pulser_calib_qa.root", "qa/calibration"),
)

# ---------------------------------------------------------------------------
# Topic-first taxonomy (operator-facing, 2026-05-29 reorg).
#
# A "topic" is a physics-level concern that lives across pipeline stages:
# "trigger health" pulls from lightdata + recodata + recotrack, "pixel
# noise" pulls from lightdata's DCR + afterpulse + crosstalk emissions,
# and so on.  The shifter asks "is the trigger matrix healthy?", not
# "is the lightdata sub-tab healthy?" — this taxonomy serves that.
#
# Each entry: (topic_key, label, description).  Order is the sub-tab
# display order.  ``general`` and ``full_plots`` are special: General
# is a curated overview (one representative plot per topic), Full plots
# is the legacy pipeline-stage drill-down (Lightdata / Recodata / …).
# Only three sub-tabs: General (the curated, chaptered overview), Full plots
# (per-writer drill-down), and Macros.  The intermediate topic tabs (Triggers /
# Pixel noise / Calibration) were retired — the General chapters cover that
# grouping now, and Full plots remains the exhaustive per-writer view.
TOPICS: tuple[tuple[str, str, str], ...] = (
    ("general",      "General",       "Curated overview — all QA chapters for the run"),
    ("full_plots",   "Full plots",    "Pipeline-stage drill-down: every PDF the writers emitted, grouped by writer"),
)

# ---------------------------------------------------------------------------
# PDF → topic routing.
#
# Each rule is (compiled-regex on basename-without-prefix, topic_key).
# Basename matching strips the leading ``NN_`` writer-order prefix so
# the rule doesn't have to chase emission order changes.  First match
# wins — order rules from specific → generic.  Unmatched PDFs fall
# through to "full_plots only" (visible in the deep-dive but not in
# any topic tab).
import re as _re_topics
_PDF_TOPIC_RULES: tuple[tuple["_re_topics.Pattern[str]", str], ...] = (
    # Streaming-trigger threshold setter — goes on Triggers.  Match
    # BEFORE the generic "score" rule so it doesn't get reclassified.
    (_re_topics.compile(r"^streaming_score(\..*)?$"),            "triggers"),
    # Pulser-calib-side anchor-Δt vs spill (single canvas per run) →
    # Calibration.  Match BEFORE the lightdata-side anchor_dt rule.
    (_re_topics.compile(r"^anchor_dt_vs_spill(\..*)?$"),         "calibration"),
    # Pulser-calib anchor cadence (consecutive-Δt Gaussian) → Calibration.
    (_re_topics.compile(r"^anchor_consecutive_dt(\..*)?$"),      "calibration"),
    # Pulser-calib 1D channel−anchor Δt (coincidence distribution) → Calibration.
    (_re_topics.compile(r"^anchor_dt_1d(\..*)?$"),               "calibration"),
    # Pulser-calib coincidence hitmap (laser spot) → Calibration.
    (_re_topics.compile(r"^coincidence_map(\..*)?$"),            "calibration"),
    # Lightdata per-trigger anchor-Δt PDFs → Triggers.
    (_re_topics.compile(r"^anchor_dt_.+(\..*)?$"),               "triggers"),
    # Per-trigger time-difference w/ Cherenkov → Triggers.
    (_re_topics.compile(r"^.*time_diff.*"),                      "triggers"),
    # Trigger coincidence matrix → Triggers.
    (_re_topics.compile(r"^trigger_matrix(\..*)?$"),             "triggers"),
    # Ring finder QA from the Hough stage → Triggers.
    (_re_topics.compile(r"^.*ring_finder.*"),                    "triggers"),
    (_re_topics.compile(r"^.*hough.*"),                          "triggers"),
    # Timing-sensor DCR — a per-channel dark-count measurement, so it
    # belongs with the other DCR plots on Noise.  Match BEFORE the generic
    # ``^timing.*`` alignment rule (which would otherwise steal it to
    # Calibration purely because the name starts with "timing").
    (_re_topics.compile(r"^timing_dcr.*"),                       "noise"),
    # Timing — chip-0/chip-1 alignment, ref-Δ.
    (_re_topics.compile(r"^timing.*"),                           "calibration"),
    (_re_topics.compile(r"^.*fine_calib.*"),                     "calibration"),
    # Pixel noise — DCR, afterpulse, cross-talk.
    (_re_topics.compile(r"^dcr.*"),                              "noise"),
    (_re_topics.compile(r"^afterpulse.*"),                       "noise"),
    (_re_topics.compile(r"^ct_.*"),                              "noise"),
    (_re_topics.compile(r"^.*crosstalk.*"),                      "noise"),
)


def _strip_pdf_prefix(basename: str) -> str:
    """Strip ``NN_`` writer-order prefix from a PDF basename.

    ``"05_streaming_score.pdf"`` → ``"streaming_score.pdf"``.
    Bare names (no prefix) pass through unchanged.  Used by
    ``topic_of`` so the routing rules don't have to chase emission
    order changes across writer revisions.
    """
    m = _re_topics.match(r"^\d+_(.+)$", basename)
    return m.group(1) if m else basename


def topic_of(pdf_basename: str) -> Optional[str]:
    """Map a PDF basename to its topic key, or None if unmatched.

    PDFs that don't match any topic rule are excluded from topic tabs
    but remain accessible via the "Full plots" deep-dive sub-tab —
    nothing is lost, just unsurfaced on the topic-first surface.

    The function is pure: no I/O, just regex matches.  Reused both by
    the dashboard (tab routing) and by tests (snapshot the topic map
    for a fixed PDF set).
    """
    stripped = _strip_pdf_prefix(pdf_basename)
    for pattern, topic in _PDF_TOPIC_RULES:
        if pattern.match(stripped):
            return topic
    return None

# Per-step cap on matplotlib thumbnails — too many at once chokes the
# layout system.  PDFs are not capped (they're already curated by the
# writer that emitted them).
_MAX_THUMBS_PER_STEP = 12

#  Phase: responsive tile grid — minimum per-tile widths in px.  The
#  ``_ResponsiveTileGrid`` rebalances columns on resize so a wider
#  window shows more tiles per row (capped at 4) and a narrower one
#  collapses to a single column.  Two values because PDF tiles (with
#  filename header + 240 px thumb) are physically wider than the
#  matplotlib histogram thumbs.
_PDF_TILE_MIN_PX = 280
_THUMB_TILE_MIN_PX = 220
#  Cross-run trend tiles on the General tab — matplotlib line plots
#  across the most-recent N runs.  Slightly wider than the histogram
#  thumbs so 20+ run-id ticks on the x-axis stay readable.
_TREND_TILE_MIN_PX = 320
_TILE_GRID_MAX_COLS = 4


class QaView(QtWidgets.QWidget):
    """QA tab — see module docstring."""

    #  Emitted when the operator picks a different run here, so the app
    #  keeps the Run Manager's picker in sync.  Suppressed during a
    #  programmatic ``set_selected_run`` (guarded by ``_syncing_run``).
    run_selected = QtCore.Signal(str)

    def __init__(
        self,
        database_path: Path,
        data_dir: Path,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._db_path = database_path
        self._data_dir = data_dir
        self._records: list[rundb.RunRecord] = []
        self._current_run_id: Optional[str] = None
        self._syncing_run = False

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(12, 12, 12, 12)
        outer.setSpacing(8)

        # ── Top bar: run picker + step tabs + actions ───────────────
        # Layout mirrors the Run Manager pattern: identity (the run
        # picker) on the left, navigation (step tabs) in the middle,
        # actions (refresh / monitor / clear) on the right.  Old layout
        # had the run picker stretching across the full width and the
        # step tabs in a second row below — wasted vertical space and
        # made the "pick a different step" gesture twice as far from
        # the "pick a different run" gesture.
        #
        # The run picker scans ``data_dir`` (NOT the database) so QA
        # offers the same set of runs the Run Manager does — only
        # runs whose files are actually on disk are listed.
        top_row = QtWidgets.QHBoxLayout()
        top_row.setSpacing(8)
        top_row.addWidget(QtWidgets.QLabel("Run:"))
        self._run_combo = QtWidgets.QComboBox()
        self._run_combo.setMinimumWidth(220)
        # Fixed-ish width — give the step tab bar room to breathe in the
        # middle.  The Run Manager uses the same proportion (picker fits
        # one run-id width, actions on the right, expand-space between).
        self._run_combo.setSizePolicy(
            QtWidgets.QSizePolicy.Preferred,
            QtWidgets.QSizePolicy.Fixed,
        )
        self._run_combo.currentTextChanged.connect(self._on_run_changed)
        top_row.addWidget(self._run_combo)

        # Step tab bar — drives ``self._sub_stack`` below.  Decoupling
        # the bar from QTabWidget lets us put it on the same row as the
        # run picker (a QTabWidget would force its bar into its own
        # frame).  Document-mode styling matches the rest of the
        # dashboard's tab look.
        self._sub_tab_bar = QtWidgets.QTabBar()
        self._sub_tab_bar.setDocumentMode(True)
        self._sub_tab_bar.setExpanding(False)
        self._sub_tab_bar.setDrawBase(False)
        self._sub_tab_bar.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding,
            QtWidgets.QSizePolicy.Fixed,
        )
        top_row.addWidget(self._sub_tab_bar, 1)
        #  Bigger refresh button (see runmanager.py for the rationale).
        self._refresh_btn = QtWidgets.QPushButton(" ⟳  Refresh ")
        f = self._refresh_btn.font(); f.setPointSize(f.pointSize() + 2)
        self._refresh_btn.setFont(f)
        self._refresh_btn.setToolTip(
            "Re-scan Data/ for new runs and force the current QA "
            "sub-tab to rebuild from disk.  Use this after a writer "
            "finishes so its fresh PDFs / histograms appear without "
            "re-selecting the run."
        )
        self._refresh_btn.clicked.connect(self._on_refresh)
        top_row.addWidget(self._refresh_btn)
        # ── Monitor toggle: poll the current run dir on a timer so a
        # writer producing files live (datataking) gets picked up
        # without manual refresh.  mtime-aware so quiet runs cost
        # one stat() per file every 5 s, no rebuild work.
        #
        # Visual state machine (operator complaint: tiny tool-button
        # was invisible at-a-glance):
        #   - off    → grey
        #   - on     → slow green pulse (≈2.2 s half-cycle — noticeable
        #              without inducing seizures)
        #   - error  → faster red pulse (≈0.7 s half-cycle)
        # Label stays " ●  Monitor " in every state — colour does the
        # talking; a suffix would just make the button change width as
        # state shifts.
        self._monitor_btn = QtWidgets.QPushButton(" ●  Monitor ")
        f = self._monitor_btn.font(); f.setPointSize(f.pointSize() + 2)
        self._monitor_btn.setFont(f)
        self._monitor_btn.setCheckable(True)
        self._monitor_btn.setChecked(False)
        self._monitor_btn.setToolTip(
            "Auto-refresh the active sub-tab every 5 s when the "
            "current run dir's file mtimes change — for monitoring "
            "writers as they produce output during datataking.\n"
            "Off by default to keep idle dashboards quiet."
        )
        self._monitor_btn.toggled.connect(self._on_monitor_toggled)
        top_row.addWidget(self._monitor_btn)

        # ── Clear QA button — purge the selected run's QA artefacts ──
        # Deletes the regenerable QA outputs ON DISK for the current
        # run (delegating to ``qa_pipeline.clean_run_dir`` — the same
        # allowlist-protected purge as ``qa_pipeline --clean``: the
        # ``qa/`` render tree, the writer output roots, and stray
        # run-root ``h_*.pdf``).  Raw DAQ device dirs and calibration
        # files are never matched; the ``.qa_persistent`` pin guards raw
        # data only, so pinned runs clear like any other.  After the
        # purge it drops the in-process render caches and rebuilds, so
        # the view reflects the now-empty QA.
        self._clear_btn = QtWidgets.QPushButton(" 🗑  Clear QA ")
        f = self._clear_btn.font(); f.setPointSize(f.pointSize() + 2)
        self._clear_btn.setFont(f)
        self._clear_btn.setToolTip(
            "Delete the selected run's regenerable QA artefacts on disk "
            "(the qa/ render tree + lightdata/recodata/recotrack output "
            "roots + stray h_*.pdf — same set as `qa_pipeline --clean`), "
            "then rebuild the view.  Raw device dirs and calibration "
            "files are left untouched.  Works on pinned runs too — the "
            "pin protects raw data, not regenerable QA."
        )
        self._clear_btn.clicked.connect(self._on_clear_qa)
        top_row.addWidget(self._clear_btn)
        outer.addLayout(top_row)

        # Polling state.  Last seen max-mtime of the current run dir;
        # the timer compares this against the live value and only
        # invalidates+rebuilds on change.
        self._monitor_last_mtime: float = 0.0
        self._monitor_timer = QtCore.QTimer(self)
        self._monitor_timer.setInterval(5000)
        self._monitor_timer.timeout.connect(self._on_monitor_tick)

        # Pulse animator.  QVariantAnimation interpolates a QColor
        # continuously between two anchors so the transition is
        # smooth, not strobed.  Ping-pong: when one half-cycle ends
        # we swap start/end and restart.  Two duration profiles:
        #
        #   - "on"    → 2200 ms per half-cycle, ease in/out.  Calm,
        #               slow breathing — reads as healthy.
        #   - "error" → 700 ms per half-cycle, linear.  Quicker
        #               attention-grabber without strobing.
        self._monitor_anim = QtCore.QVariantAnimation(self)
        self._monitor_anim.valueChanged.connect(self._on_monitor_anim_value)
        self._monitor_anim.finished.connect(self._on_monitor_anim_finished)
        self._monitor_visual_state: str = "off"
        self._set_monitor_visual_state("off")

        # ── Body: stacked per-step pages, driven by the top-row tab bar ──
        # The bar lives up in ``top_row`` (built above) — it just feeds
        # ``currentChanged`` into this stack so picking a step switches
        # the visible page.  Same UX as the old QTabWidget; the only
        # difference is the bar location.
        self._sub_stack = QtWidgets.QStackedWidget()
        self._sub_tab_bar.currentChanged.connect(self._sub_stack.setCurrentIndex)
        self._sub_tab_bar.currentChanged.connect(self._on_sub_tab_changed)
        outer.addWidget(self._sub_stack, 1)

        # Topic-first sub-tabs (2026-05-29 reorg).  Order matches the
        # TOPICS tuple.  ``general`` and ``full_plots`` map to bespoke
        # pages; the rest get a generic ``_TopicQaPage`` that pulls
        # PDFs from every pipeline stage matching its topic key.
        #
        # _step_pages remains the canonical "iterate every stage's
        # page" handle — re-used for cache-invalidate sweeps from
        # ``_on_clear_qa``.  Stage pages now live INSIDE the
        # ``_FullPlotsQaPage`` so we proxy through to its internal
        # ``_stage_pages`` dict.
        self._topic_pages: dict[str, QtWidgets.QWidget] = {}
        for topic_key, label, description in TOPICS:
            if topic_key == "general":
                page: QtWidgets.QWidget = _GeneralQaPage()
            elif topic_key == "full_plots":
                page = _FullPlotsQaPage()
                # Inject the active rundb path so the streaming-score
                # n_σ picker (§1.5.2 Option C) inside the thumbnail
                # tree knows where to commit on Save.  Threaded once
                # here at construction; rebuilds on later run-changes
                # pick it up via ``_StepQaPage._database_path``.
                page.set_database_path(self._db_path)
                # _step_pages is the legacy "all pipeline stages" handle;
                # proxy through the FullPlots page so the rest of the
                # QaView class doesn't have to know about the nesting.
                self._step_pages: dict[str, _StepQaPage] = (
                    page._stage_pages  # noqa: SLF001 — intentional access
                )
            else:
                page = _TopicQaPage(topic_key, label, description)
            self._sub_tab_bar.addTab(label)
            self._sub_stack.addWidget(page)
            self._topic_pages[topic_key] = page

        # Macros sub-tab — placeholder, fills in when bespoke macros
        # get a curated launcher.
        self._macros_page = _MacrosPlaceholder()
        self._sub_tab_bar.addTab("Macros")
        self._sub_stack.addWidget(self._macros_page)

        # Populate the run picker from the database.
        self.reload()

    # ----- data + selection ---------------------------------------------

    def reload(self) -> None:
        """Re-scan ``data_dir`` for runs and repopulate the picker.

        The QA tab's source of runs is the on-disk ``Data/`` tree —
        same as the Run Manager — not ``rundb.load_database``.
        A run that's in the database but has no data dir on disk
        wouldn't have anything to show in QA anyway; conversely a
        freshly-downloaded run dir that hasn't been added to the
        database yet still shows up here.
        """
        #  Most-recent-first.  Run ids are ``YYYYMMDD-HHMMSS`` so a
        #  reverse lexical sort is reverse-chronological for free.
        #  The operator almost always wants to inspect the run they
        #  just acquired, which lives at the top now.
        runs: list[str] = []
        if self._data_dir.is_dir():
            runs = sorted(
                (p.name for p in self._data_dir.iterdir() if p.is_dir()),
                reverse=True,
            )
        current = self._run_combo.currentText()
        self._run_combo.blockSignals(True)
        self._run_combo.clear()
        for r in runs:
            self._run_combo.addItem(r)
        if current:
            idx = self._run_combo.findText(current)
            if idx >= 0:
                self._run_combo.setCurrentIndex(idx)
        self._run_combo.blockSignals(False)
        if self._run_combo.count() and not self._current_run_id:
            self._current_run_id = self._run_combo.currentText()
        self._refresh_current_page()

    def _on_run_changed(self, run_id: str) -> None:
        self._current_run_id = run_id or None
        self._refresh_current_page()
        #  Broadcast for cross-tab sync unless this change came from the
        #  sync itself (avoids a ping-pong between the two pickers).
        if not self._syncing_run and run_id:
            self.run_selected.emit(run_id)

    def set_selected_run(self, run_id: str) -> None:
        """Programmatically select ``run_id`` here without re-broadcasting.

        No-op when the run isn't in the QA picker or is already current.
        """
        if not run_id or run_id == self._run_combo.currentText():
            return
        idx = self._run_combo.findText(run_id)
        if idx < 0:
            return
        self._syncing_run = True
        try:
            self._run_combo.setCurrentIndex(idx)
        finally:
            self._syncing_run = False

    def _on_sub_tab_changed(self, _idx: int) -> None:
        self._refresh_current_page()

    def _on_clear_qa(self) -> None:
        """Delete the selected run's regenerable QA artefacts, then rebuild.

        Two layers:

          - ON DISK — delegate to ``qa_pipeline.clean_run_dir``, the
            same allowlist-protected purge as ``qa_pipeline --clean``:
            the ``qa/`` render tree, the writer output roots
            (``lightdata``/``recodata``/``recotrackdata.root``), and
            stray run-root ``h_*.pdf``.  Raw DAQ device dirs and
            calibration files are NEVER matched, so a mis-fire can't
            destroy a run's inputs.  The ``.qa_persistent`` pin guards
            raw data only and does NOT exempt a run — its QA is still
            regenerable, so pinned and unpinned runs clear alike.
          - IN PROCESS — the on-disk artefacts just vanished, so drop
            the process-wide PDF-thumbnail cache (keyed on path+mtime,
            else stale renders are served from RAM forever) and
            invalidate *every* step page before ``_on_refresh`` walks
            the now-empty tree.

        Guarded by a Yes/No confirm (the project's destructive-action
        convention); no-ops with an info dialog if no run is selected.
        """
        if not self._current_run_id or self._data_dir is None:
            QtWidgets.QMessageBox.information(
                self, "Clear QA", "Select a run first.",
            )
            return
        run_dir = self._data_dir / self._current_run_id
        if not run_dir.is_dir():
            QtWidgets.QMessageBox.information(
                self, "Clear QA",
                f"Run directory not found:\n{run_dir}",
            )
            return
        # No pin exemption: ``.qa_persistent`` guards a run's RAW DAQ data
        # against retention pruning — it says nothing about the QA
        # artefacts, which are regenerable by re-running the pipeline.
        # Clearing QA on a pinned baseline is therefore safe (the raw
        # inputs the clean would need to rebuild are exactly what the pin
        # protects), so Clear QA treats pinned and unpinned runs alike.
        confirm = QtWidgets.QMessageBox.question(
            self, "Clear QA",
            f"Delete the regenerable QA artefacts for "
            f"{self._current_run_id}?\n\n"
            "Removes the qa/ render tree, lightdata.root, recodata.root, "
            "recotrackdata.root and stray h_*.pdf.\n\n"
            "Raw DAQ device dirs and calibration files are left "
            "untouched — everything removed is regenerable by re-running "
            "the QA pipeline.",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
        )
        if confirm != QtWidgets.QMessageBox.Yes:
            return
        # On-disk purge — mirror ``qa_pipeline --clean`` exactly.  The
        # log callback collects per-entry messages; only the count is
        # surfaced (failures are folded into the log lines by the helper
        # and would leave the entry on disk, reflected in the rebuild).
        msgs: list[str] = []
        try:
            removed = qa_pipeline.clean_run_dir(run_dir, msgs.append)
        except Exception as exc:  # noqa: BLE001
            QtWidgets.QMessageBox.warning(
                self, "Clear QA",
                f"Could not clear QA artefacts:\n{exc}",
            )
            return
        # In-process caches — drop the shared thumbnail cache and every
        # topic page's dedupe state (the FullPlots page proxies the
        # invalidation through to each nested stage page) so the rebuild
        # walks the now-empty tree instead of serving stale renders.
        try:
            _PdfTile._THUMB_CACHE.clear()
        except (NameError, AttributeError):  # pragma: no cover
            pass
        for page in self._topic_pages.values():
            if hasattr(page, "invalidate_cache"):
                page.invalidate_cache()
        self._on_refresh()
        QtWidgets.QMessageBox.information(
            self, "Clear QA",
            f"Cleared {removed} QA entry(ies) for {self._current_run_id}.",
        )

    def _on_refresh(self) -> None:
        """Re-scan Data/ + force the active page to rebuild from disk."""
        self.reload()
        # ``reload`` already calls ``_refresh_current_page``; force the
        # active sub-page to forget its cache so the rebuild actually
        # walks the disk (set_run dedupes when (run, dir) is unchanged).
        # Topic pages, general, and full-plots all implement the
        # invalidate_cache + set_run protocol.
        page = self._sub_stack.currentWidget()
        if hasattr(page, "invalidate_cache") and hasattr(page, "set_run"):
            page.invalidate_cache()
            page.set_run(self._current_run_id, self._data_dir)
        # Reset the mtime baseline so the next monitor tick sees
        # "no change" against the fresh state we just rebuilt against.
        self._monitor_last_mtime = self._current_run_max_mtime()

    # ----- monitor mode -------------------------------------------------

    def _on_monitor_toggled(self, on: bool) -> None:
        """Start / stop the 5-second monitor poll + pulse animation."""
        if on:
            # Anchor the baseline at the current state so the FIRST
            # tick only fires if something genuinely changed since
            # the toggle.
            self._monitor_last_mtime = self._current_run_max_mtime()
            self._monitor_timer.start()
            self._set_monitor_visual_state("on")
        else:
            self._monitor_timer.stop()
            self._set_monitor_visual_state("off")

    def _on_monitor_tick(self) -> None:
        """Poll: if any file in the current run dir is newer, rebuild.

        Errors (stat failures, vanished run dir) flip the visual
        state to red so the operator knows the monitor stopped
        producing useful results — without silently freezing.
        """
        try:
            cur = self._current_run_max_mtime()
        except Exception:  # noqa: BLE001
            self._set_monitor_visual_state("error")
            return
        # mtime walk succeeded → ensure we're back to the healthy
        # pulse state even if a previous tick had flagged error.
        if self._monitor_btn.isChecked():
            self._set_monitor_visual_state("on")
        if cur > self._monitor_last_mtime:
            self._monitor_last_mtime = cur
            # Same machinery as the manual refresh button but kept
            # cheap — no run-picker rescan unless really needed
            # (use the explicit refresh button for that).
            page = self._sub_stack.currentWidget()
            if hasattr(page, "invalidate_cache") and hasattr(page, "set_run"):
                page.invalidate_cache()
                page.set_run(self._current_run_id, self._data_dir)

    def _on_monitor_anim_value(self, color) -> None:
        """Apply the interpolated colour as the button background.

        Defensive: QVariantAnimation interpolates QColor on most Qt
        versions, but some older builds (or unusual easing curves)
        can emit a raw tuple or partial value.  Pass through anything
        QColor-shaped, ignore everything else — silently dropping a
        frame is better than crashing the dashboard mid-pulse.
        """
        try:
            css_name = color.name()  # QColor.name() → "#rrggbb"
        except AttributeError:
            return
        self._monitor_btn.setStyleSheet(
            f"QPushButton {{ background-color: {css_name};"
            "  color: white; font-weight: 600;"
            "  border-radius: 4px; padding: 2px 8px; }"
        )

    def _on_monitor_anim_finished(self) -> None:
        """Half-cycle done: swap start/end and restart for ping-pong."""
        if self._monitor_visual_state not in ("on", "error"):
            return
        start = self._monitor_anim.startValue()
        end = self._monitor_anim.endValue()
        self._monitor_anim.setStartValue(end)
        self._monitor_anim.setEndValue(start)
        self._monitor_anim.start()

    def _set_monitor_visual_state(self, state: str) -> None:
        """Drive the Monitor button's colour from the state machine.

        ``state`` ∈ {``"off"``, ``"on"``, ``"error"``}.

          - off    → no animation, neutral theme colours.
          - on     → smooth ping-pong between two greens, 2200 ms
                     per half-cycle, ease in/out.  Calm "alive"
                     breathing.
          - error  → smooth ping-pong between two reds, 700 ms per
                     half-cycle, linear.  Faster to flag attention
                     without strobing.

        The label stays ``" ●  Monitor "`` in every state; the dot's
        colour does the talking.  An "(on)" / "(err)" suffix would be
        redundant with the pulse animation and just makes the button
        change width as state shifts.

        The animation runs in the Qt event loop so the polling
        timer and rendering both stay responsive.
        """
        self._monitor_visual_state = state
        self._monitor_anim.stop()
        if state == "on":
            #  Soft green pulse: bright Apple-green (#2DBE60) ↔
            #  darker forest (#1E8A45).  Both keep white text legible.
            self._monitor_anim.setDuration(2200)
            self._monitor_anim.setEasingCurve(QtCore.QEasingCurve.InOutSine)
            self._monitor_anim.setStartValue(QtGui.QColor("#1E8A45"))
            self._monitor_anim.setEndValue(QtGui.QColor("#2DBE60"))
            self._monitor_anim.start()
        elif state == "error":
            #  Faster red pulse: deep crimson ↔ brighter red.  Linear
            #  easing reads as more urgent than the green's ease-in-out
            #  without being a full strobe.
            self._monitor_anim.setDuration(700)
            self._monitor_anim.setEasingCurve(QtCore.QEasingCurve.Linear)
            self._monitor_anim.setStartValue(QtGui.QColor("#8E2820"))
            self._monitor_anim.setEndValue(QtGui.QColor("#E74C3C"))
            self._monitor_anim.start()
        else:
            # Idle / off — drop the stylesheet so the theme reclaims
            # control of the button look.
            self._monitor_btn.setStyleSheet("")

    def _current_run_max_mtime(self) -> float:
        """Largest mtime across all files in the current run dir.

        Returns 0.0 when no run is selected or the dir doesn't exist —
        which means the monitor tick will fire as soon as it does
        (any real file beats 0.0).
        """
        if not self._current_run_id or self._data_dir is None:
            return 0.0
        run_dir = self._data_dir / self._current_run_id
        if not run_dir.is_dir():
            return 0.0
        # Walk one level deep + the qa/<step>/ subfolders we render
        # PDFs from.  Skipping the rdo-* subdirs (raw FIFO data —
        # those don't drive the QA view and would dominate stat() cost).
        best = 0.0
        try:
            for entry in run_dir.iterdir():
                name = entry.name
                if entry.is_file():
                    try:
                        best = max(best, entry.stat().st_mtime)
                    except OSError:
                        pass
                elif entry.is_dir() and name == "qa":
                    for sub in entry.rglob("*"):
                        if sub.is_file():
                            try:
                                best = max(best, sub.stat().st_mtime)
                            except OSError:
                                pass
        except OSError:
            return best
        return best

    def _refresh_current_page(self) -> None:
        page = self._sub_stack.currentWidget()
        # Duck-typed: topic pages, general, and full-plots all expose
        # set_run(run_id, data_dir).  Macros placeholder doesn't and
        # is correctly skipped.
        if hasattr(page, "set_run"):
            page.set_run(self._current_run_id, self._data_dir)


# ---------------------------------------------------------------------------
# Per-step QA page: PDFs + matplotlib thumbnails for one pipeline step.
# ---------------------------------------------------------------------------


class _StepQaPage(QtWidgets.QWidget):
    """Two-section page: writer-emitted PDFs on top, TH* thumbnails below.

    Lazy: only repaints when ``set_run`` is called with a different
    (run, data_dir) pair (the QA tab fires it on every sub-tab change
    too, so we de-dupe to avoid re-renders).
    """

    def __init__(
        self,
        label: str,
        root_name: str,
        qa_subdir: str,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._label = label
        self._root_name = root_name
        self._qa_subdir = qa_subdir
        self._current_run_id: Optional[str] = None
        self._current_data_dir: Optional[Path] = None
        #  Injected post-construction by QaView (via
        #  ``_FullPlotsQaPage.set_database_path``) so the streaming-
        #  score n_σ picker that lives inside ``_ThumbnailTile`` can
        #  reach the active rundb file.  None disables Save.
        self._database_path: Optional[Path] = None

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(0, 8, 0, 0)
        outer.setSpacing(8)

        # Compact status header — what's loaded right now.
        self._status = QtWidgets.QLabel("(no run selected)")
        self._status.setObjectName("muted")
        self._status.setStyleSheet("font-style: italic; font-size: 11px;")
        outer.addWidget(self._status)

        # Scrollable body — PDFs section then histograms section.
        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        self._body = QtWidgets.QWidget()
        self._body_layout = QtWidgets.QVBoxLayout(self._body)
        self._body_layout.setContentsMargins(0, 0, 0, 0)
        self._body_layout.setSpacing(12)
        self._body_layout.setAlignment(QtCore.Qt.AlignTop)
        self._scroll.setWidget(self._body)
        outer.addWidget(self._scroll, 1)

    def set_run(self, run_id: Optional[str], data_dir: Path) -> None:
        # De-dupe: avoid rebuilding when re-selected.
        if run_id == self._current_run_id and data_dir == self._current_data_dir:
            return
        self._current_run_id = run_id
        self._current_data_dir = data_dir
        self._rebuild()

    def set_database_path(self, path: Optional[Path]) -> None:
        """Set the rundb file the streaming-score n_σ picker writes to.

        Forwarded down through ``_build_histograms_section`` →
        ``_CollapsibleDirCard`` → ``_ThumbnailTile`` so the picker
        dialog has it ready to commit.  Forces a rebuild if the path
        changes — otherwise tiles built before the path arrived would
        keep their stale ``None``.  No-op when the value matches.
        """
        if path == self._database_path:
            return
        self._database_path = path
        if self._current_run_id and self._current_data_dir is not None:
            self.invalidate_cache()
            self._rebuild_with_current()

    def _rebuild_with_current(self) -> None:
        """Re-fire ``set_run`` with the cached selection after the
        dedupe was invalidated."""
        run_id, data_dir = self._current_run_id, self._current_data_dir
        if run_id and data_dir is not None:
            self._current_run_id = None
            self._current_data_dir = None
            self.set_run(run_id, data_dir)

    def invalidate_cache(self) -> None:
        """Drop the dedupe state so the next ``set_run`` forces rebuild.

        Used by the QA tab's ⟳ refresh button — operator clicked it
        because a writer finished and new files landed; the dedupe
        would otherwise skip the rebuild.
        """
        self._current_run_id = None
        self._current_data_dir = None

    # ----- internals --------------------------------------------------

    def _rebuild(self) -> None:
        # Wipe.
        while self._body_layout.count():
            item = self._body_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()

        run_id = self._current_run_id
        data_dir = self._current_data_dir
        if not run_id or data_dir is None:
            self._status.setText("(no run selected)")
            return
        run_dir = data_dir / run_id
        if not run_dir.is_dir():
            self._status.setText(f"run directory not found: {run_dir}")
            return

        # PDFs first — writer-controlled rendering, prefered when present.
        pdf_section, n_pdfs = self._build_pdf_section(run_dir)
        if pdf_section is not None:
            self._body_layout.addWidget(pdf_section)

        # Histograms — fallback / browse-all path via uproot+matplotlib.
        hist_section, n_hists = self._build_histograms_section(run_dir)
        if hist_section is not None:
            self._body_layout.addWidget(hist_section)

        bits = []
        bits.append(f"run: {run_id}")
        if n_pdfs:
            bits.append(f"{n_pdfs} PDF(s)")
        if n_hists:
            bits.append(f"{n_hists} histogram(s)")
        if not n_pdfs and not n_hists:
            bits.append("nothing to show — writer hasn't produced output yet")
        self._status.setText("  ·  ".join(bits))

    def _build_pdf_section(
        self, run_dir: Path,
    ) -> tuple[Optional[QtWidgets.QWidget], int]:
        """Scan ``<run>/<qa_subdir>/*.pdf`` and render each inline."""
        qa_dir = run_dir / self._qa_subdir
        if not qa_dir.is_dir():
            return None, 0
        pdfs = sorted(qa_dir.glob("*.pdf"))
        if not pdfs:
            return None, 0

        # Try the Qt PDF module.  Ships with PySide6 6.5+ as a separate
        # import; if it isn't present we fall back to a "click to open
        # externally" tile list so the operator still sees the files.
        try:
            from PySide6 import QtPdf, QtPdfWidgets  # noqa: F401
            have_qt_pdf = True
        except ImportError:
            have_qt_pdf = False

        holder = QtWidgets.QFrame()
        holder.setObjectName("cardSurface")
        v = QtWidgets.QVBoxLayout(holder)
        v.setContentsMargins(14, 12, 14, 14)
        v.setSpacing(10)

        head = QtWidgets.QHBoxLayout()
        title = QtWidgets.QLabel(f"<b>{self._label}</b>  ·  PDFs from writer")
        title.setTextFormat(QtCore.Qt.RichText)
        head.addWidget(title)
        head.addStretch(1)
        cap = QtWidgets.QLabel(f"{len(pdfs)}")
        cap.setObjectName("muted")
        cap.setStyleSheet("font-size: 11px;")
        head.addWidget(cap)
        v.addLayout(head)

        tiles = [_PdfTile(pdf, have_qt_pdf=have_qt_pdf) for pdf in pdfs]
        v.addWidget(_ResponsiveTileGrid(
            tiles, tile_min_width=_PDF_TILE_MIN_PX,
        ))
        return holder, len(pdfs)

    def _build_histograms_section(
        self, run_dir: Path,
    ) -> tuple[Optional[QtWidgets.QWidget], int]:
        """Render the writer's TDirectory tree as collapsible cards.

        Recursive: every sub-folder becomes a collapsible nested
        card inside its parent.  Sub-sub-folders nest naturally
        (Triggers/TIMING/h_xxx → Triggers card, TIMING sub-card
        inside, h_xxx thumbnail inside that).  All collapsed by
        default so the page opens fast — operator drills into the
        groups they care about.
        """
        path = run_dir / self._root_name
        if not path.is_file():
            return None, 0
        tree = thumbs.enumerate_histograms_tree(path)
        total = _count_tree_hists(tree)
        if total == 0:
            return None, 0

        # Lazy matplotlib import.
        try:
            import matplotlib  # noqa: F401
            matplotlib.use("QtAgg", force=False)
            from matplotlib.backends.backend_qtagg import FigureCanvas
            from matplotlib.figure import Figure
        except ImportError:
            warn = QtWidgets.QFrame()
            warn.setObjectName("cardSurface")
            wv = QtWidgets.QVBoxLayout(warn)
            wv.setContentsMargins(14, 12, 14, 14)
            msg = QtWidgets.QLabel(
                "matplotlib not installed — `pip install matplotlib` or "
                "rerun ./scripts/qa_quicklook --reinstall."
            )
            msg.setObjectName("muted")
            msg.setWordWrap(True)
            wv.addWidget(msg)
            return warn, 0

        # Outer container: step header + one collapsible per top-level
        # entry (top-level histograms become a "(top level)" card).
        holder = QtWidgets.QWidget()
        v = QtWidgets.QVBoxLayout(holder)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(8)

        # Step header card (always visible — context for what we're
        # looking at).
        header = QtWidgets.QFrame()
        header.setObjectName("cardSurface")
        hv = QtWidgets.QVBoxLayout(header)
        hv.setContentsMargins(14, 8, 14, 8)
        hv.setSpacing(2)
        title = QtWidgets.QLabel(
            f"<b>{self._label}</b>  ·  histograms from "
            f"<code>{self._root_name}</code>"
        )
        title.setTextFormat(QtCore.Qt.RichText)
        hv.addWidget(title)
        sub = QtWidgets.QLabel(
            f"{total} histogram(s) — collapsible cards mirror the "
            "writer's TDirectory tree; click to expand."
        )
        sub.setObjectName("muted")
        sub.setStyleSheet("font-size: 10px;")
        hv.addWidget(sub)
        v.addWidget(header)

        dark = _dark_theme_active()
        # Top-level own histograms → "(top level)" card.
        if tree["hists"]:
            v.addWidget(_CollapsibleDirCard(
                path, "(top level)", tree["hists"], {},
                dark, FigureCanvas, Figure, depth=0,
                database_path=self._database_path,
            ))
        # Sub-directories at the top level, in order.
        for subname, subnode in tree["subdirs"].items():
            v.addWidget(_CollapsibleDirCard(
                path, subname, subnode["hists"], subnode["subdirs"],
                dark, FigureCanvas, Figure, depth=0,
                database_path=self._database_path,
            ))
        return holder, total


# ---------------------------------------------------------------------------
# PDF tile: inline QtPdf rendering of one .pdf file, or "open externally"
# fallback when PySide6.QtPdf isn't available.
# ---------------------------------------------------------------------------


class _ResponsiveTileGrid(QtWidgets.QWidget):
    """Container that re-flows its child tiles when its width changes.

    The QA tab used to hard-code grids at 3 columns (matplotlib thumbs)
    or 1 column (PDF tiles stacked vertically).  Either choice wastes
    horizontal space on wide screens and overflows the viewport on
    narrow ones.  This widget watches its own width, computes how many
    fixed-min-width tiles fit, and rebuilds the QGridLayout placement
    when the answer changes.

    Tile ordering is preserved on every reflow — the operator's
    reading order stays the writer's emission order regardless of
    column count.  Cap at ``_TILE_GRID_MAX_COLS`` (4) because beyond
    that even a wide tile becomes too small to read the thumbnail.
    """

    def __init__(
        self,
        tiles: list[QtWidgets.QWidget],
        *,
        tile_min_width: int = _PDF_TILE_MIN_PX,
        spacing: int = 8,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._tiles = list(tiles)
        self._tile_min_width = tile_min_width
        self._spacing = spacing
        self._cur_cols = 0

        self._grid = QtWidgets.QGridLayout(self)
        self._grid.setContentsMargins(0, 0, 0, 0)
        self._grid.setHorizontalSpacing(spacing)
        self._grid.setVerticalSpacing(spacing)
        #  Tiles take their natural width per column, no stretch.  The
        #  trailing column(s) of the last row stay empty rather than
        #  ballooning the right-most tile.
        for i, tile in enumerate(self._tiles):
            self._grid.addWidget(tile, 0, i)  # placeholder; real placement in _reflow
        #  Initial reflow uses any size hint we already have; the
        #  first resizeEvent triggered by the parent layout will hit
        #  the genuine width and re-place if different.
        self._reflow(max(self.width(), 1))

    def resizeEvent(self, event: QtGui.QResizeEvent) -> None:
        self._reflow(event.size().width())
        super().resizeEvent(event)

    def _cols_for_width(self, width: int) -> int:
        if width <= 0:
            return 1
        #  n cols fit when n * tile_min_width + (n - 1) * spacing ≤ width
        #  ⇒ n ≤ (width + spacing) / (tile_min_width + spacing)
        cols = (width + self._spacing) // (self._tile_min_width + self._spacing)
        return max(1, min(_TILE_GRID_MAX_COLS, int(cols)))

    def _reflow(self, width: int) -> None:
        cols = self._cols_for_width(width)
        if cols != self._cur_cols:
            self._cur_cols = cols
            #  Detach every tile first so re-adding lands at the new (r, c).
            for tile in self._tiles:
                self._grid.removeWidget(tile)
            for i, tile in enumerate(self._tiles):
                r, c = divmod(i, cols)
                self._grid.addWidget(tile, r, c)
            # Make every column share equally so tile_w is uniform.
            for c in range(cols):
                self._grid.setColumnStretch(c, 1)
        #  Even when ``cols`` didn't change, the available width per
        #  tile did (the user dragged the splitter / resized the
        #  window).  QGridLayout doesn't call ``heightForWidth`` on
        #  children — we do it manually so each tile's row gets the
        #  height its aspect-ratio thumbnail wants.  Without this the
        #  tile box stays at whatever natural sizeHint the header gave
        #  it and the preview shrinks / squishes inside.
        if cols > 0:
            tile_w = max(1, (width - (cols - 1) * self._spacing) // cols)
            for tile in self._tiles:
                has_hfw = (hasattr(tile, "hasHeightForWidth")
                           and tile.hasHeightForWidth())
                if not has_hfw:
                    continue
                h = tile.heightForWidth(tile_w)
                if h > 0:
                    #  setFixedHeight is atomic — separate
                    #  setMinimumHeight + setMaximumHeight calls have
                    #  an ordering bug when a resize moves h across
                    #  the existing min/max range (the second call
                    #  gets clamped against the first).
                    tile.setFixedHeight(h)


class _ThumbRenderTask(QtCore.QRunnable):
    """Worker job: rasterise page 1 of a PDF to a ``QImage`` off the GUI thread.

    Each task creates its OWN ``QPdfDocument`` (QtPdf is reentrant, so distinct
    instances render concurrently on different threads); the result QImage is
    handed back to the main thread via the pool's signal, where it's turned
    into a QPixmap (QPixmap is GUI-thread-only) and mounted on the tile.
    """

    def __init__(self, tile_ref, pdf_path: Path, height_px: int, signal) -> None:
        super().__init__()
        self._tile_ref = tile_ref
        self._pdf_path = pdf_path
        self._height_px = height_px
        self._signal = signal

    def run(self) -> None:  # worker thread
        image = None
        try:
            from PySide6 import QtPdf
            doc = QtPdf.QPdfDocument()
            doc.load(self._pdf_path.as_posix())
            if doc.pageCount() >= 1:
                ps = doc.pagePointSize(0)
                page_h = max(1.0, float(ps.height()))
                page_w = max(1.0, float(ps.width()))
                scale = self._height_px / page_h
                target_w = max(1, int(round(page_w * scale)))
                image = doc.render(
                    0, QtCore.QSize(target_w, self._height_px))
        except Exception:  # noqa: BLE001
            image = None
        self._signal.emit(self._tile_ref, image)


class _ThumbRenderPool(QtCore.QObject):
    """Parallel background renderer for :class:`_PdfTile` thumbnails.

    Each thumbnail is a *blocking* ``QPdfDocument`` render; building a page full
    of tiles on the GUI thread froze the UI.  Tiles instead show a placeholder
    and submit a job here, run across a ``QThreadPool`` (N = cores − 1).  The
    rendered QImage comes back to the GUI thread via a queued signal, becomes a
    QPixmap, and is mounted on the tile — so the window stays responsive and the
    plots fill in concurrently.  Submission order sets priority, so the first
    (top-of-page) tiles render first.
    """

    rendered = QtCore.Signal(object, object)  # (weakref tile, QImage|None)

    _instance: "Optional[_ThumbRenderPool]" = None

    @classmethod
    def instance(cls) -> "_ThumbRenderPool":
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def __init__(self) -> None:
        super().__init__()
        self._pool = QtCore.QThreadPool(self)
        self._pool.setMaxThreadCount(max(2, (os.cpu_count() or 4) - 1))
        self._seq = 0
        #  Queued (default cross-thread) delivery → runs on the GUI thread.
        self.rendered.connect(self._on_rendered)

    def submit(self, tile: "_PdfTile") -> None:
        self._seq += 1
        task = _ThumbRenderTask(
            weakref.ref(tile), tile._pdf_path,
            type(tile)._THUMB_HEIGHT_PX, self.rendered)
        #  Higher priority = sooner; earlier submissions (top tiles) win.
        self._pool.start(task, -self._seq)

    def _on_rendered(self, tile_ref, image) -> None:  # GUI thread
        tile = tile_ref()
        if tile is None:
            return
        if image is None or image.isNull():
            tile._on_async_pixmap(None)
            return
        pixmap = QtGui.QPixmap.fromImage(image)
        #  Populate the process-wide thumb cache so a re-render is instant.
        try:
            mtime_ns = tile._pdf_path.stat().st_mtime_ns
            cache = _PdfTile._THUMB_CACHE
            cache[(str(tile._pdf_path), mtime_ns)] = pixmap
            if len(cache) > _PdfTile._THUMB_CACHE_MAX:
                for k in list(cache.keys())[
                        :len(cache) - _PdfTile._THUMB_CACHE_MAX]:
                    cache.pop(k, None)
        except OSError:
            pass
        tile._on_async_pixmap(pixmap)


class _PdfTile(QtWidgets.QFrame):
    """Single PDF — fast thumbnail (page 1 raster, cached) + click-to-zoom.

    Why no inline ``QPdfView`` per tile any more
    --------------------------------------------
    Constructing a ``QPdfView`` per PDF and loading its document up-front
    is *slow* — every sub-tab change rebuilds the section, and each tile
    blocks on a synchronous ``QPdfDocument.load()``  + view construction.
    On a folder with half a dozen PDFs the wait was easily seconds.

    The thumbnail-only path renders page 1 once to a small ``QPixmap``,
    caches it (process-wide) by (path, mtime), and shows it in a
    clickable ``QLabel``.  Click → modal that constructs the heavy
    ``QPdfView`` on demand for the full multi-page navigation.  The
    cache invalidates on writer re-emit (mtime changes), so the
    operator never sees a stale preview.
    """

    # Process-wide thumbnail cache.  Keyed by (str(path), mtime_ns) so
    # a re-emitted PDF re-renders automatically.  Memory-bounded by
    # cache size policy — pruned to the most recent N entries when
    # the cache grows past the soft cap.
    _THUMB_CACHE: dict[tuple[str, int], QtGui.QPixmap] = {}
    _THUMB_CACHE_MAX = 64
    #  Cache the page at a HIGH resolution (800 px) and let the
    #  ``_ClickableThumb`` label scale on each resize — keeps the
    #  thumbnail crisp when the tile grid expands the column width
    #  on a wide window.  240 px was fine for the fixed-size
    #  previous behaviour but blurred as soon as the responsive
    #  layout started stretching tiles.
    _THUMB_HEIGHT_PX = 800

    def __init__(
        self,
        pdf_path: Path,
        *,
        have_qt_pdf: bool,
        aspect_override: Optional[float] = None,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._pdf_path = pdf_path
        self._have_qt_pdf = have_qt_pdf
        #  Optional H/W aspect forced on the embedded ``_ClickableThumb``.
        #  Used by the General overview to keep every headline tile at
        #  a single aspect (square = 1.0) regardless of the source PDF
        #  page geometry — different writers emit different page sizes
        #  and the natural per-tile aspect made the headline row look
        #  ragged.  ``None`` falls back to source aspect (full-plots
        #  view keeps that flexibility on purpose — heterogeneous PDFs
        #  there are deliberate browse-all behaviour).
        self._aspect_override = aspect_override
        self._thumb: _ClickableThumb | None = None
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        #  Expanding horizontally; height tracks width via heightForWidth.
        #  Without this the grid would give every tile the same equal
        #  column width but each tile would stay at its (small) natural
        #  sizeHint and the box wouldn't grow with the window.
        sp = QtWidgets.QSizePolicy(QtWidgets.QSizePolicy.Expanding,
                                   QtWidgets.QSizePolicy.Preferred)
        sp.setHeightForWidth(True)
        self.setSizePolicy(sp)

        v = QtWidgets.QVBoxLayout(self)
        v.setContentsMargins(8, 8, 8, 8)
        v.setSpacing(4)

        # Header row: filename + size + Open-externally button.
        head = QtWidgets.QHBoxLayout()
        name = QtWidgets.QLabel(f"<b>{pdf_path.name}</b>")
        name.setTextFormat(QtCore.Qt.RichText)
        head.addWidget(name)
        try:
            size_kb = pdf_path.stat().st_size / 1024.0
            size_lbl = QtWidgets.QLabel(f"{size_kb:.0f} kB")
            size_lbl.setObjectName("muted")
            size_lbl.setStyleSheet("font-size: 10px; padding-left: 6px;")
            head.addWidget(size_lbl)
        except OSError:
            pass
        head.addStretch(1)
        open_btn = QtWidgets.QPushButton("Open ↗")
        open_btn.setToolTip("Open in the system PDF viewer")
        open_btn.clicked.connect(self._open_external)
        head.addWidget(open_btn)
        v.addLayout(head)

        # Thumbnail.  Rendered in the BACKGROUND (via _ThumbRenderQueue) so a
        # page full of tiles never blocks: install a cached thumb immediately
        # if we have one, else show a placeholder and enqueue an async render
        # that swaps it in when ready.  Click (once rendered) → big modal.
        self._layout = v
        self._placeholder: QtWidgets.QWidget | None = None
        cached = self._cached_thumb()
        if cached is not None:
            self._install_thumb(cached)
        elif self._have_qt_pdf:
            ph = QtWidgets.QLabel("rendering…")
            ph.setObjectName("muted")
            ph.setAlignment(QtCore.Qt.AlignCenter)
            ph.setMinimumHeight(150)
            ph.setStyleSheet("font-style: italic; color: #888;")
            v.addWidget(ph, 1)
            self._placeholder = ph
            _ThumbRenderPool.instance().submit(self)
        else:
            msg = QtWidgets.QLabel(
                "Inline PDF preview needs PySide6's QtPdf module "
                "(install <code>PySide6-Addons</code>).  Click Open ↗ "
                "to view the file in the system viewer."
            )
            msg.setTextFormat(QtCore.Qt.RichText)
            msg.setObjectName("muted")
            msg.setWordWrap(True)
            v.addWidget(msg)

    # ----- responsive sizing --------------------------------------------

    def hasHeightForWidth(self) -> bool:  # noqa: D401
        return self._thumb is not None

    def heightForWidth(self, w: int) -> int:
        """Header + (square-ish) thumbnail height.

        Total tile height = top/bottom margins + header row height +
        layout spacing + heightForWidth(thumb_width) of the thumbnail.
        thumb_width = w − left/right margins.  The grid uses this to
        pick a row height that lets every tile in the row sit at the
        natural aspect of its preview without clipping or stretching.
        """
        if self._thumb is None:
            return w
        margins = self.layout().contentsMargins()
        inner_w = max(1, w - margins.left() - margins.right())
        header_h = 0
        item = self.layout().itemAt(0)
        if item is not None and item.widget() is not None:
            header_h = item.widget().sizeHint().height()
        elif item is not None and item.layout() is not None:
            header_h = item.layout().sizeHint().height()
        spacing = self.layout().spacing()
        return (margins.top() + margins.bottom() +
                header_h + spacing +
                self._thumb.heightForWidth(inner_w))

    # ----- thumbnail rendering + cache ----------------------------------

    def _get_or_render_thumb(self) -> Optional[QtGui.QPixmap]:
        """Return page-1 thumbnail, hitting the class-level cache."""
        if not self._have_qt_pdf:
            return None
        try:
            mtime_ns = self._pdf_path.stat().st_mtime_ns
        except OSError:
            return None
        key = (str(self._pdf_path), mtime_ns)
        cached = type(self)._THUMB_CACHE.get(key)
        if cached is not None:
            return cached
        pixmap = self._render_page_one()
        if pixmap is not None:
            type(self)._THUMB_CACHE[key] = pixmap
            # LRU-ish pruning: when over the cap, drop the oldest few
            # insertions.  dict preserves insertion order on 3.7+,
            # so popping the first N entries removes the stalest.
            if len(type(self)._THUMB_CACHE) > type(self)._THUMB_CACHE_MAX:
                excess = len(type(self)._THUMB_CACHE) - type(self)._THUMB_CACHE_MAX
                for k in list(type(self)._THUMB_CACHE.keys())[:excess]:
                    type(self)._THUMB_CACHE.pop(k, None)
        return pixmap

    def _render_page_one(self) -> Optional[QtGui.QPixmap]:
        """Rasterise page 1 of the PDF into a height-bounded ``QPixmap``."""
        try:
            from PySide6 import QtPdf
            doc = QtPdf.QPdfDocument()
            doc.load(self._pdf_path.as_posix())
            if doc.pageCount() < 1:
                return None
            # Preserve aspect ratio at the thumbnail height.
            page_size = doc.pagePointSize(0)
            page_h = max(1.0, float(page_size.height()))
            page_w = max(1.0, float(page_size.width()))
            scale = type(self)._THUMB_HEIGHT_PX / page_h
            target_w = max(1, int(round(page_w * scale)))
            target_h = type(self)._THUMB_HEIGHT_PX
            image = doc.render(0, QtCore.QSize(target_w, target_h))
            if image.isNull():
                return None
            return QtGui.QPixmap.fromImage(image)
        except Exception:  # noqa: BLE001
            return None

    def _cached_thumb(self) -> Optional[QtGui.QPixmap]:
        """Return the cached page-1 thumbnail WITHOUT rendering (None on miss)."""
        if not self._have_qt_pdf:
            return None
        try:
            mtime_ns = self._pdf_path.stat().st_mtime_ns
        except OSError:
            return None
        return type(self)._THUMB_CACHE.get((str(self._pdf_path), mtime_ns))

    def _install_thumb(self, pixmap: QtGui.QPixmap) -> None:
        """Mount the rendered thumbnail as the clickable preview."""
        tlabel = _ClickableThumb(pixmap, aspect_override=self._aspect_override)
        tlabel.clicked.connect(self._open_modal)
        tlabel.setToolTip("Click to open inline at full size")
        self._layout.addWidget(tlabel, 1)
        self._thumb = tlabel

    def _on_async_pixmap(self, pixmap: Optional[QtGui.QPixmap]) -> None:
        """GUI-thread callback from :class:`_ThumbRenderPool`: drop the
        placeholder and mount the rendered thumbnail (or an error note)."""
        if self._thumb is not None:
            return  # already installed (e.g. a re-entrant render)
        if self._placeholder is not None:
            self._placeholder.setParent(None)
            self._placeholder.deleteLater()
            self._placeholder = None
        if pixmap is not None:
            self._install_thumb(pixmap)
        elif self._have_qt_pdf:
            err = QtWidgets.QLabel("(could not render preview — use Open ↗)")
            err.setObjectName("muted")
            err.setStyleSheet("font-style: italic;")
            self._layout.addWidget(err)

    # ----- actions ------------------------------------------------------

    def _open_external(self) -> None:
        QtGui.QDesktopServices.openUrl(
            QtCore.QUrl.fromLocalFile(self._pdf_path.as_posix())
        )

    def _open_modal(self) -> None:
        """Pop a modal with the full multi-page inline ``QPdfView``.

        This is where the expensive widget construction now happens —
        only on demand, only for the one PDF the operator clicked.
        """
        if not self._have_qt_pdf:
            self._open_external()
            return
        dlg = QtWidgets.QDialog(self)
        dlg.setWindowTitle(self._pdf_path.name)
        #  Sized for comfortable reading on a typical laptop screen
        #  (was 900×700 — operator-reported as cramped, especially for
        #  trigger_matrix / hitmap plots that already filled the
        #  source canvas).  Stays well within a 13" 1440×900 panel.
        dlg.resize(1280, 880)
        layout = QtWidgets.QVBoxLayout(dlg)
        layout.setContentsMargins(8, 8, 8, 8)
        try:
            from PySide6 import QtPdf, QtPdfWidgets
            doc = QtPdf.QPdfDocument(dlg)
            doc.load(self._pdf_path.as_posix())
            viewer = QtPdfWidgets.QPdfView(dlg)
            viewer.setDocument(doc)
            viewer.setPageMode(QtPdfWidgets.QPdfView.PageMode.MultiPage)
            viewer.setZoomMode(QtPdfWidgets.QPdfView.ZoomMode.FitToWidth)
            layout.addWidget(viewer, 1)
        except Exception as exc:  # noqa: BLE001
            err = QtWidgets.QLabel(
                f"could not embed PDF: {exc}\nUse the Open ↗ button instead."
            )
            err.setObjectName("muted")
            err.setWordWrap(True)
            layout.addWidget(err)
        buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.Close
        )
        buttons.rejected.connect(dlg.reject)
        layout.addWidget(buttons)
        dlg.exec()


class _ClickableThumb(QtWidgets.QLabel):
    """``QLabel`` that emits ``clicked()`` on left-click AND scales its
    pixmap to fit its current size, preserving the source aspect ratio.

    Why the manual scaling?  The simple ``setPixmap`` + a fixed-size
    cached image meant the tile never grew or shrank with the parent
    grid's column width — once the responsive grid stretched the
    column past 240 px, the thumbnail stayed pinned at 240 px and
    the tile box collapsed around it.

    Implementation: cache the high-res source pixmap once, override
    ``resizeEvent`` to rescale into the label's current size, and
    report ``heightForWidth(w)`` from the source aspect ratio so the
    parent grid allocates a square (or whatever-aspect) row height
    for each tile.

    Cursor shown as a pointing hand so the affordance is obvious.
    """

    clicked = QtCore.Signal()

    def __init__(
        self,
        pixmap: QtGui.QPixmap,
        *,
        aspect_override: Optional[float] = None,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._src_pixmap = pixmap                # full-res source — never mutated
        #  When set, ``heightForWidth`` reports ``w * aspect_override``
        #  instead of using the source pixmap's natural H/W.  The
        #  pixmap still scales preserving its own aspect inside the
        #  fixed box, so a portrait PDF inside a square tile gets
        #  letterboxed; a landscape one gets pillarboxed.  Cleaner
        #  than cropping the thumbnail itself.
        self._aspect_override = aspect_override
        self.setAlignment(QtCore.Qt.AlignCenter)
        self.setCursor(QtCore.Qt.PointingHandCursor)
        self.setMinimumSize(80, 80)
        #  Expanding width + height follows width via heightForWidth.
        sp = QtWidgets.QSizePolicy(QtWidgets.QSizePolicy.Expanding,
                                   QtWidgets.QSizePolicy.Preferred)
        sp.setHeightForWidth(True)
        self.setSizePolicy(sp)
        # Border so the thumbnail reads as a button.
        self.setStyleSheet(
            "QLabel { border: 1px solid rgba(155,142,142,0.25);"
            "         border-radius: 3px; padding: 2px; }"
            "QLabel:hover { border-color: #FF6B6B; }"
        )
        self._apply_scaled()

    def hasHeightForWidth(self) -> bool:  # noqa: D401 — Qt convention
        return True

    def heightForWidth(self, w: int) -> int:
        if self._aspect_override is not None and self._aspect_override > 0:
            return int(round(w * self._aspect_override))
        pm = self._src_pixmap
        if pm is None or pm.isNull() or pm.width() <= 0:
            return w
        return int(round(w * pm.height() / pm.width()))

    def sizeHint(self) -> QtCore.QSize:
        pm = self._src_pixmap
        if pm is None or pm.isNull():
            return super().sizeHint()
        # Hint at the current label width — Qt asks for sizeHint on
        # first show before a heightForWidth round-trip happens.
        w = max(self.width(), 200)
        return QtCore.QSize(w, self.heightForWidth(w))

    def resizeEvent(self, event: QtGui.QResizeEvent) -> None:
        self._apply_scaled()
        super().resizeEvent(event)

    def _apply_scaled(self) -> None:
        if self._src_pixmap is None or self._src_pixmap.isNull():
            return
        target = self.size()
        if target.width() <= 0 or target.height() <= 0:
            return
        scaled = self._src_pixmap.scaled(
            target,
            QtCore.Qt.KeepAspectRatio,
            QtCore.Qt.SmoothTransformation,
        )
        super().setPixmap(scaled)

    def mousePressEvent(self, event: QtGui.QMouseEvent) -> None:
        if event.button() == QtCore.Qt.LeftButton:
            self.clicked.emit()
        super().mousePressEvent(event)


# ---------------------------------------------------------------------------
# Thumbnail tile — matplotlib FigureCanvas backed by an uproot histogram.
# ---------------------------------------------------------------------------


class _ThumbnailTile(QtWidgets.QFrame):
    """One histogram thumbnail.  Click → pop a full-size interactive dialog.

    Bigger fig + transparent patches + dark-theme-aware tick / label
    colours so the plot reads cleanly on either light or dark cards.
    The modal dialog now ships with matplotlib's standard
    ``NavigationToolbar2QT`` for pan / zoom / save / home — the
    interactivity that the static thumbnail can't offer.
    """

    # Geometry tuned for a 3-column QA grid on a typical laptop.
    # 220 px height + ~3:2 fig aspect = readable axes + room for tick labels.
    _THUMB_HEIGHT_PX = 220

    #  Histograms whose modal we hijack with the Streaming-score
    #  n_σ picker (§1.5.2 Option C).  Either hist clicked opens the
    #  picker — both samples render on the same canvas anyway, so the
    #  thumbnail the operator clicked is just the trigger.  Matched
    #  on the bare basename so a future TDirectory subdir prefix
    #  wouldn't silently break the dispatch.
    _PICKER_HISTS = frozenset({
        "h_streaming_score_noise",
        "h_streaming_score_data",
        "h_streaming_score_inbeam",
    })

    def __init__(
        self,
        root_path: Path,
        hist_path: str,
        classname: str,
        FigureCanvas,
        Figure,
        *,
        dark: bool = False,
        database_path: Optional[Path] = None,
    ) -> None:
        super().__init__()
        self._root_path = root_path
        self._hist_path = hist_path
        self._classname = classname
        self._Figure = Figure
        self._FigureCanvas = FigureCanvas
        self._dark = dark
        self._database_path = database_path
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        # Hover affordance so the operator knows the tile is clickable.
        self.setCursor(QtCore.Qt.PointingHandCursor)
        self.setToolTip(f"Click to open '{hist_path.split('/')[-1]}' "
                        "full-size with pan / zoom / save")

        v = QtWidgets.QVBoxLayout(self)
        v.setContentsMargins(6, 6, 6, 6)
        v.setSpacing(2)

        # Figure size in inches × dpi defines the rendered pixel size.
        # 3.6 × 2.4" @ 100 dpi = 360 × 240 px — comfortably bigger than
        # the cramped 2.6 × 1.8" we had before.  tight_layout keeps the
        # axes from overflowing the canvas.
        fig = Figure(figsize=(3.6, 2.4), dpi=100, tight_layout=True)
        fig.patch.set_alpha(0.0)
        ax = fig.add_subplot(111)
        ax.patch.set_alpha(0.0)
        hist = thumbs.load_histogram(root_path, hist_path)
        if hist is not None:
            short = hist_path.split("/")[-1]
            thumbs.render_histogram(ax, hist, title=short, dark=self._dark)
        else:
            ax.text(0.5, 0.5, "(not found)",
                    ha="center", va="center", transform=ax.transAxes,
                    fontsize=8,
                    color="#E8E8E8" if dark else "#9B8E8E")
            ax.set_xticks([]); ax.set_yticks([])
        canvas = FigureCanvas(fig)
        canvas.setFixedHeight(self._THUMB_HEIGHT_PX)
        #  FigureCanvas swallows mouse events for matplotlib's own
        #  pick / hover handlers — that meant clicks landing on the
        #  histogram pixels (i.e. the entire visual centre of the
        #  tile) never reached the parent's ``mousePressEvent``, so
        #  the modal never opened.  Operator's main click target
        #  IS the canvas; mark it transparent for mouse events so
        #  clicks bubble up to the tile.  We don't use mpl's
        #  built-in picking on the thumbnails anyway — interactive
        #  exploration lives in the modal.
        canvas.setAttribute(QtCore.Qt.WA_TransparentForMouseEvents, True)
        v.addWidget(canvas)

        # Caption only — the "Open in ROOT" escape hatch lives inside
        # the modal, not on the tile (operator-confirmed flow:
        # static tile → click → mpl modal → button → TCanvas).  One
        # affordance per tile keeps the grid scannable.
        cap = QtWidgets.QLabel(hist_path)
        cap.setObjectName("muted")
        cap.setStyleSheet("font-size: 9px;")
        cap.setWordWrap(True)
        cap.setToolTip(f"{hist_path}  ·  {classname}")
        v.addWidget(cap)

    def mousePressEvent(self, event: QtGui.QMouseEvent) -> None:
        if event.button() == QtCore.Qt.LeftButton:
            self._show_big()
        super().mousePressEvent(event)

    def _show_big(self) -> None:
        """Pop the interactive plot dialog for this histogram.

        Streaming-score hists route to the n_σ picker (§1.5.2 Option
        C) instead of the generic modal — same click target, different
        dialog.  The picker still renders both noise + data even when
        only one of them was the tile clicked.
        """
        basename = self._hist_path.split("/")[-1]
        if basename in self._PICKER_HISTS:
            #  Run id derives from the standard ``Data/<run>/foo.root``
            #  layout; the rundb path is whatever the QaView injected
            #  down the tree.  Both fall through to None when the
            #  layout isn't honoured — the picker then disables Save
            #  rather than crashing.
            run_id = self._root_path.parent.name or None
            dlg = _StreamingScorePickerDialog(
                parent=self,
                root_path=self._root_path,
                database_path=self._database_path,
                run_id=run_id,
                Figure=self._Figure,
                FigureCanvas=self._FigureCanvas,
                dark=self._dark,
            )
            dlg.exec()
            return

        hist = thumbs.load_histogram(self._root_path, self._hist_path)
        dlg = _InteractivePlotDialog(
            parent=self,
            root_path=self._root_path,
            hist_path=self._hist_path,
            hist=hist,
            Figure=self._Figure,
            FigureCanvas=self._FigureCanvas,
            dark=self._dark,
        )
        dlg.exec()


# ---------------------------------------------------------------------------
# Interactive plot dialog — beefs up matplotlib with log/lin/fit/autorange.
# ---------------------------------------------------------------------------


class _InteractivePlotDialog(QtWidgets.QDialog):
    """Click-through view with a custom toolbar.

    Built on matplotlib's QtAgg backend.  Above the canvas:

      - Standard ``NavigationToolbar2QT`` — pan, zoom-to-rect, home,
        back / forward, axis-config, save-as-png.
      - Custom toolbar with shifter-relevant operations matplotlib's
        stock toolbar doesn't expose: log-X / log-Y toggles,
        auto-range, fit menu (Gaussian / linear / polynomial-N),
        clear-fit, and a stats panel that surfaces fit parameters
        + reduced χ² inline.

    1D-only operations (fit, log-Y stats) are hidden when the
    histogram is 2D — the toolbar adapts to what makes sense.
    """

    def __init__(
        self,
        parent: QtWidgets.QWidget | None,
        root_path: Path,
        hist_path: str,
        hist,                         # uproot histogram or None
        Figure,
        FigureCanvas,
        dark: bool,
    ) -> None:
        super().__init__(parent)
        self._root_path = root_path
        self._hist_path = hist_path
        self._hist = hist
        self._dark = dark
        self._Figure = Figure
        self._FigureCanvas = FigureCanvas

        # 1D vs 2D detection — drives which controls are enabled.
        self._is_1d = False
        if hist is not None:
            try:
                data = hist.to_numpy()
                self._is_1d = (len(data) == 2)
                self._numpy_data = data
            except Exception:  # noqa: BLE001
                self._numpy_data = None
        else:
            self._numpy_data = None

        self.setWindowTitle(f"{root_path.name} · {hist_path}")
        #  Larger default than before (was 1000×720) so the fit-stats
        #  line at the bottom and the axis labels don't crowd on a
        #  default open — operator-reported "still a bit small".
        self.resize(1320, 900)

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(8, 8, 8, 8)
        outer.setSpacing(6)

        # ── Figure + canvas ─────────────────────────────────────────
        self._fig = Figure(figsize=(8.5, 5.5), dpi=120, tight_layout=True)
        self._fig.patch.set_alpha(0.0)
        self._ax = self._fig.add_subplot(111)
        self._ax.patch.set_alpha(0.0)
        self._render_into_ax()
        self._canvas = FigureCanvas(self._fig)
        outer.addWidget(self._canvas, 1)

        # ── Standard mpl navigation toolbar ─────────────────────────
        try:
            from matplotlib.backends.backend_qtagg import (
                NavigationToolbar2QT as NavigationToolbar,
            )
            outer.addWidget(NavigationToolbar(self._canvas, self))
        except ImportError:
            pass

        # ── Custom shifter toolbar ──────────────────────────────────
        custom = QtWidgets.QToolBar()
        #  Bigger icon + bigger text so the "log Y / log X /
        #  auto-range / Fit / clear fit / Open in ROOT ↗" actions
        #  read clearly — at the default Qt point size the labels
        #  were hard to spot in the modal.
        custom.setIconSize(QtCore.QSize(20, 20))
        cf = custom.font()
        cf.setPointSize(cf.pointSize() + 2)
        custom.setFont(cf)
        custom.setToolButtonStyle(QtCore.Qt.ToolButtonTextOnly)
        # Log Y toggle.
        self._act_logy = QtGui.QAction("log Y", self)
        self._act_logy.setCheckable(True)
        self._act_logy.toggled.connect(self._on_toggle_logy)
        custom.addAction(self._act_logy)
        # Log X toggle.
        self._act_logx = QtGui.QAction("log X", self)
        self._act_logx.setCheckable(True)
        self._act_logx.toggled.connect(self._on_toggle_logx)
        custom.addAction(self._act_logx)
        custom.addSeparator()
        # Auto-range.
        act_auto = QtGui.QAction("auto-range", self)
        act_auto.setToolTip("Reset axis limits to fit all data")
        act_auto.triggered.connect(self._on_autorange)
        custom.addAction(act_auto)
        custom.addSeparator()
        # Fit menu — 1D only.
        fit_btn = QtWidgets.QToolButton(custom)
        fit_btn.setText("Fit ▾")
        fit_btn.setToolTip("Fit a model to the (visible) data")
        fit_btn.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        fit_menu = QtWidgets.QMenu(fit_btn)
        fit_menu.addAction("Gaussian").triggered.connect(
            lambda: self._on_fit("gauss"))
        fit_menu.addAction("Linear").triggered.connect(
            lambda: self._on_fit("linear"))
        fit_menu.addAction("Polynomial degree 2").triggered.connect(
            lambda: self._on_fit("poly2"))
        fit_menu.addAction("Polynomial degree 3").triggered.connect(
            lambda: self._on_fit("poly3"))
        fit_btn.setMenu(fit_menu)
        custom.addWidget(fit_btn)
        fit_btn.setEnabled(self._is_1d)
        # Clear fit.
        self._act_clear_fit = QtGui.QAction("clear fit", self)
        self._act_clear_fit.triggered.connect(self._on_clear_fit)
        self._act_clear_fit.setEnabled(False)
        custom.addAction(self._act_clear_fit)
        # Spacer + ROOT escape hatch (right-aligned).
        custom.addSeparator()
        spacer = QtWidgets.QWidget()
        spacer.setSizePolicy(QtWidgets.QSizePolicy.Expanding,
                             QtWidgets.QSizePolicy.Preferred)
        custom.addWidget(spacer)
        act_root = QtGui.QAction("Open in ROOT ↗", self)
        act_root.setToolTip(
            "Pop a real ROOT TCanvas for this histogram — gives you "
            "ROOT's native FitPanel, axis menus, stat-box controls, "
            "log/lin toggle, the full interactive experience."
        )
        act_root.triggered.connect(self._on_open_in_root)
        custom.addAction(act_root)
        outer.addWidget(custom)

        # ── Stats panel (read-only) ─────────────────────────────────
        self._stats_label = QtWidgets.QLabel("")
        self._stats_label.setObjectName("muted")
        self._stats_label.setStyleSheet(
            "font-family: 'Menlo','Consolas',monospace; font-size: 11px;"
            " padding: 4px 6px;"
        )
        self._stats_label.setTextInteractionFlags(
            QtCore.Qt.TextSelectableByMouse
        )
        outer.addWidget(self._stats_label)
        self._update_stats_panel()

        # ── Close button ────────────────────────────────────────────
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Close)
        buttons.rejected.connect(self.reject)
        outer.addWidget(buttons)

        # State for clearable fit overlay.
        self._fit_artists: list = []

    # ----- rendering ----------------------------------------------------

    def _render_into_ax(self) -> None:
        if self._hist is not None:
            thumbs.render_histogram(
                self._ax, self._hist,
                title=self._hist_path, dark=self._dark,
            )
        else:
            self._ax.text(0.5, 0.5, "(histogram not found)",
                          ha="center", va="center",
                          transform=self._ax.transAxes,
                          color="#9B8E8E")

    # ----- toolbar handlers --------------------------------------------

    def _on_toggle_logy(self, on: bool) -> None:
        self._ax.set_yscale("log" if on else "linear")
        # log Y can crash on bins ≤ 0; clip the floor when going log.
        if on:
            ymin, ymax = self._ax.get_ylim()
            positive_floor = max(0.5, 0.5)
            self._ax.set_ylim(positive_floor, max(ymax, positive_floor * 10))
        self._canvas.draw_idle()

    def _on_toggle_logx(self, on: bool) -> None:
        self._ax.set_xscale("log" if on else "linear")
        self._canvas.draw_idle()

    def _on_autorange(self) -> None:
        self._ax.relim()
        self._ax.autoscale()
        self._canvas.draw_idle()

    def _on_clear_fit(self) -> None:
        for art in self._fit_artists:
            try:
                art.remove()
            except Exception:  # noqa: BLE001
                pass
        self._fit_artists.clear()
        self._act_clear_fit.setEnabled(False)
        self._update_stats_panel()  # drop fit lines from stats
        self._canvas.draw_idle()

    def _on_fit(self, kind: str) -> None:
        """Fit a model to the visible 1D slice.

        Visible-only: we restrict the fit data to the current x-axis
        view so the shifter can zoom around a peak and re-fit just
        that range.  Failures fall back to a clear status line, not a
        dialog (less interruption).
        """
        if not self._is_1d or self._numpy_data is None:
            return
        import numpy as np
        try:
            from scipy.optimize import curve_fit
        except ImportError:
            self._stats_label.setText(
                "scipy not installed — install scipy to enable Fit."
            )
            return

        values, edges = self._numpy_data
        centres = 0.5 * (edges[:-1] + edges[1:])
        x_lo, x_hi = self._ax.get_xlim()
        mask = (centres >= x_lo) & (centres <= x_hi) & np.isfinite(values)
        x = centres[mask]
        y = values[mask]
        if x.size < 3:
            self._set_stats_line("fit: not enough points in view")
            return

        try:
            if kind == "gauss":
                # Initial guess: peak at argmax, sigma ~ stdev of x weighted by y.
                if y.max() <= 0:
                    self._set_stats_line("fit: data is empty / non-positive")
                    return
                w = np.clip(y, 0, None)
                wsum = w.sum() or 1.0
                mu0 = (x * w).sum() / wsum
                var0 = (((x - mu0) ** 2) * w).sum() / wsum
                sig0 = max(np.sqrt(var0), (x.max() - x.min()) / 100)
                A0 = y.max()
                def gauss(x, A, mu, sigma):
                    return A * np.exp(-0.5 * ((x - mu) / sigma) ** 2)
                popt, pcov = curve_fit(gauss, x, y,
                                       p0=[A0, mu0, sig0], maxfev=5000)
                ymodel = gauss(x, *popt)
                perr = np.sqrt(np.diag(pcov))
                self._report_fit("Gaussian",
                                 ["A", "μ", "σ"], popt, perr, x, y, ymodel)
                self._draw_model(x, ymodel)
            elif kind == "linear":
                def lin(x, a, b): return a * x + b
                popt, pcov = curve_fit(lin, x, y, maxfev=5000)
                ymodel = lin(x, *popt)
                perr = np.sqrt(np.diag(pcov))
                self._report_fit("Linear (a·x+b)",
                                 ["a", "b"], popt, perr, x, y, ymodel)
                self._draw_model(x, ymodel)
            elif kind in ("poly2", "poly3"):
                deg = 2 if kind == "poly2" else 3
                coeffs = np.polyfit(x, y, deg)
                ymodel = np.polyval(coeffs, x)
                # No covariance from polyfit unless cov=True; redo cleanly.
                coeffs, cov = np.polyfit(x, y, deg, cov=True)
                ymodel = np.polyval(coeffs, x)
                perr = np.sqrt(np.diag(cov))
                names = [f"c{deg - i}" for i in range(deg + 1)]
                self._report_fit(f"Polynomial deg {deg}",
                                 names, coeffs, perr, x, y, ymodel)
                self._draw_model(x, ymodel)
        except Exception as exc:  # noqa: BLE001
            self._set_stats_line(f"fit failed: {type(exc).__name__}: {exc}")

    # ----- helpers ------------------------------------------------------

    def _draw_model(self, x, ymodel) -> None:
        # Pre-clear any previous fit overlay so a re-fit doesn't pile up.
        for art in self._fit_artists:
            try:
                art.remove()
            except Exception:  # noqa: BLE001
                pass
        self._fit_artists.clear()
        line = self._ax.plot(x, ymodel, "-",
                             color="#0BDA51", linewidth=1.5,
                             label="fit")[0]
        self._fit_artists.append(line)
        self._act_clear_fit.setEnabled(True)
        self._canvas.draw_idle()

    def _report_fit(self, model: str, names, popt, perr, x, y, ymodel) -> None:
        import numpy as np
        resid = y - ymodel
        # χ² with Poisson-like errors √y (clamped to 1 for empty bins).
        sigma = np.where(y > 0, np.sqrt(y), 1.0)
        chi2 = float(np.sum((resid / sigma) ** 2))
        ndof = max(1, x.size - len(popt))
        chi2_red = chi2 / ndof
        param_lines = "   ".join(
            f"{n}={v:.4g}±{e:.2g}"
            for n, v, e in zip(names, popt, perr)
        )
        self._set_stats_line(
            f"{model}: {param_lines}   χ²/ndof = {chi2:.2f}/{ndof} "
            f"= {chi2_red:.3f}"
        )

    def _update_stats_panel(self) -> None:
        # Default text: basic histogram summary (entries, mean, std).
        if not self._is_1d or self._numpy_data is None:
            self._stats_label.setText("")
            return
        import numpy as np
        values, edges = self._numpy_data
        centres = 0.5 * (edges[:-1] + edges[1:])
        n = float(values.sum())
        if n > 0:
            mean = float((centres * values).sum() / n)
            var = float(((centres - mean) ** 2 * values).sum() / n)
            std = float(np.sqrt(var))
            self._set_stats_line(
                f"entries: {n:.0f}   mean: {mean:.4g}   std: {std:.4g}"
            )
        else:
            self._set_stats_line("entries: 0")

    def _set_stats_line(self, text: str) -> None:
        self._stats_label.setText(text)

    def _on_open_in_root(self) -> None:
        ok = _spawn_root_canvas(self._root_path, self._hist_path)
        if not ok:
            QtWidgets.QMessageBox.warning(
                self, "Open in ROOT",
                "Could not launch build/bin/qa_tcanvas — make sure the "
                "binary is built (cmake --build build --target qa_tcanvas).",
            )


# ---------------------------------------------------------------------------
# Streaming-score n_σ picker — interactive cut on the score canvas.
# ---------------------------------------------------------------------------


def _resolve_streaming_conf_default(root_path: Path) -> float:
    """Read ``[streaming_trigger].n_sigma_threshold`` from ``conf/streaming.toml``.

    Resolves the repo root via the standard ``Data/<run>/foo.root``
    layout (same walk as :func:`_spawn_root_canvas`) and follows the
    symlink at ``conf/streaming.toml`` to the active config under
    ``conf/working/`` or ``conf/QA/``.  Returns ``0.0`` when anything
    in the chain is missing — the picker treats that as "no usable
    default" and falls through to the crossover heuristic.
    """
    repo_root = root_path.parent.parent.parent
    streaming_conf = repo_root / "conf" / "streaming.toml"
    if not streaming_conf.is_file():
        return 0.0
    try:
        import sys as _sys
        if _sys.version_info >= (3, 11):
            import tomllib as _tomllib
        else:  # pragma: no cover
            import tomli as _tomllib  # type: ignore
        with streaming_conf.open("rb") as fh:
            doc = _tomllib.load(fh)
    except Exception:  # noqa: BLE001
        return 0.0
    trig = doc.get("streaming_trigger")
    if not isinstance(trig, dict):
        return 0.0
    v = trig.get("n_sigma_threshold")
    try:
        return float(v) if v is not None else 0.0
    except (TypeError, ValueError):
        return 0.0


class _StreamingScorePickerDialog(QtWidgets.QDialog):
    """Interactive n_σ cut on the streaming-score canvas.

    The shifter opens this by clicking the ``h_streaming_score_noise``
    or ``h_streaming_score_data`` thumbnail in QA → Lightdata.  The
    hists overlay on a shared log-Y axis (blue = DCR / first-frames,
    red = signal / data-taking, violet = in-beam bkg — same scheme the
    C++ writer paints with).  A
    draggable vertical line marks the candidate cut; the side panel
    shows four live quantities (``P(misfire)``, ``Acceptance``, ``S/N``
    above cut, raw data count above cut).  "Save to rundb" commits via
    :func:`rundb.update_run_field` with ``auto_pin`` — only the active
    run changes; downstream inherit-from-prev runs keep their merged
    view.

    Implements §1.5.2 Option C; see ``streaming_picker.py`` for the
    integral / seed math.
    """

    _SAVE_FIELD = "streaming_n_sigma_threshold"

    def __init__(
        self,
        parent: QtWidgets.QWidget | None,
        *,
        root_path: Path,
        database_path: Optional[Path],
        run_id: Optional[str],
        Figure,
        FigureCanvas,
        dark: bool,
    ) -> None:
        super().__init__(parent)
        self._root_path = root_path
        self._database_path = database_path
        self._run_id = run_id
        self._dark = dark
        self._Figure = Figure
        self._FigureCanvas = FigureCanvas

        self.setWindowTitle(
            f"Streaming-score n_σ picker · {root_path.parent.name}"
        )
        #  Side-panel needs ~280 px of headroom, canvas wants room
        #  for the legend without overlapping the high-n_σ data tail
        #  — bumped from 1180×700 to keep parity with the other
        #  inspect dialogs.
        self.resize(1440, 900)

        # ── Load both hists ──────────────────────────────────────────
        from . import streaming_picker as _sp
        h_noise = thumbs.load_histogram(root_path, "h_streaming_score_noise")
        h_data = thumbs.load_histogram(root_path, "h_streaming_score_data")
        h_inbeam = thumbs.load_histogram(root_path, "h_streaming_score_inbeam")
        self._noise_counts, edges_n = self._to_arrays(h_noise)
        self._data_counts, edges_d = self._to_arrays(h_data)
        self._inbeam_counts, edges_i = self._to_arrays(h_inbeam)
        # All three hists share a booking — pick whichever edges exist.
        # If all are missing we still show the dialog with a single-bin
        # placeholder so the operator sees what went wrong rather than
        # a silent failure.
        self._edges = edges_n if edges_n else (edges_d or edges_i or [0.0, 50.0])
        if not self._noise_counts:
            self._noise_counts = [0.0] * (len(self._edges) - 1)
        if not self._data_counts:
            self._data_counts = [0.0] * (len(self._edges) - 1)
        if not self._inbeam_counts:
            self._inbeam_counts = [0.0] * (len(self._edges) - 1)

        # ── Seed the cut ────────────────────────────────────────────
        self._rundb_saved_value = self._read_saved_rundb_value()
        self._conf_default = _resolve_streaming_conf_default(root_path)
        self._cut = _sp.seed_threshold(
            rundb_value=self._rundb_saved_value,
            conf_value=self._conf_default,
            noise_counts=self._noise_counts,
            data_counts=self._data_counts,
            edges=self._edges,
        )

        # ── Layout ──────────────────────────────────────────────────
        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(8, 8, 8, 8)
        outer.setSpacing(6)

        body = QtWidgets.QHBoxLayout()
        body.setSpacing(10)
        outer.addLayout(body, 1)

        # — Canvas (figure built but NOT painted yet — the first
        #   ``_render_axes`` + ``_refresh_stats`` calls touch the side
        #   panel labels, so they have to run after the panel below
        #   has been constructed).
        self._fig = Figure(figsize=(8.0, 5.5), dpi=110, tight_layout=True)
        self._fig.patch.set_alpha(0.0)
        self._ax = self._fig.add_subplot(111)
        self._ax.patch.set_alpha(0.0)
        self._canvas = FigureCanvas(self._fig)
        body.addWidget(self._canvas, 1)

        # — Side panel (stats + Save / Close)
        panel = QtWidgets.QVBoxLayout()
        panel.setSpacing(10)
        body.addLayout(panel, 0)

        self._cut_label = QtWidgets.QLabel()
        f = self._cut_label.font(); f.setPointSize(f.pointSize() + 6); f.setBold(True)
        self._cut_label.setFont(f)
        panel.addWidget(self._cut_label)

        intro = QtWidgets.QLabel(
            "Drag the vertical line on the canvas to pick the n_σ "
            "cut.  Numbers update live.  Save writes the value to "
            "the per-run field in the active rundb."
        )
        intro.setWordWrap(True)
        intro.setObjectName("muted")
        intro.setStyleSheet("font-style: italic; font-size: 11px;")
        panel.addWidget(intro)

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight)
        form.setFormAlignment(QtCore.Qt.AlignTop)
        form.setHorizontalSpacing(14)
        form.setVerticalSpacing(6)
        self._lbl_p_misfire = QtWidgets.QLabel("—")
        self._lbl_acceptance = QtWidgets.QLabel("—")
        self._lbl_sn = QtWidgets.QLabel("—")
        self._lbl_n_above = QtWidgets.QLabel("—")
        for lbl in (self._lbl_p_misfire, self._lbl_acceptance,
                    self._lbl_sn, self._lbl_n_above):
            sf = lbl.font(); sf.setPointSize(sf.pointSize() + 2); lbl.setFont(sf)
        form.addRow("P(misfire):", self._lbl_p_misfire)
        form.addRow("Acceptance:", self._lbl_acceptance)
        form.addRow("S/N above cut:", self._lbl_sn)
        form.addRow("N(data) above cut:", self._lbl_n_above)
        panel.addLayout(form)

        # Seed-provenance hint — operators have to know which of the
        # three priority branches put the line where it is.
        self._seed_note = QtWidgets.QLabel()
        self._seed_note.setObjectName("muted")
        self._seed_note.setStyleSheet("font-style: italic; font-size: 11px;")
        self._seed_note.setWordWrap(True)
        panel.addWidget(self._seed_note)
        self._refresh_seed_note()

        panel.addStretch(1)

        # — Buttons
        btn_row = QtWidgets.QHBoxLayout()
        btn_row.setSpacing(8)
        self._save_btn = QtWidgets.QPushButton("Save to rundb")
        self._save_btn.setToolTip(
            "Write the current cut to the active run's "
            f"`{self._SAVE_FIELD}` field.  Downstream inherit-from-"
            "prev runs are auto-pinned so only this run changes."
        )
        self._save_btn.clicked.connect(self._on_save)
        self._save_btn.setEnabled(
            self._database_path is not None and self._run_id is not None
        )
        if not self._save_btn.isEnabled():
            self._save_btn.setToolTip(
                "Save disabled: no rundb path / run id resolved for "
                "this view."
            )
        btn_row.addWidget(self._save_btn)
        btn_row.addStretch(1)
        close_btn = QtWidgets.QPushButton("Close")
        close_btn.clicked.connect(self.accept)
        btn_row.addWidget(close_btn)
        outer.addLayout(btn_row)

        # ── Drag wiring ─────────────────────────────────────────────
        self._dragging = False
        self._cid_press = self._canvas.mpl_connect(
            "button_press_event", self._on_press)
        self._cid_motion = self._canvas.mpl_connect(
            "motion_notify_event", self._on_motion)
        self._cid_release = self._canvas.mpl_connect(
            "button_release_event", self._on_release)

        # First paint — runs AFTER the side-panel labels exist so
        # ``_render_axes`` / ``_refresh_stats`` can populate them
        # without an AttributeError.
        self._render_axes()
        self._refresh_stats()
        self._canvas.draw_idle()

    # ----- hist → arrays ---------------------------------------------------

    @staticmethod
    def _to_arrays(hist) -> tuple[list[float], list[float]]:
        """``hist.to_numpy() → (counts list, edges list)``; ``([], [])``
        when the hist is missing or unconvertible."""
        if hist is None:
            return [], []
        try:
            counts, edges = hist.to_numpy()
        except Exception:  # noqa: BLE001
            return [], []
        return [float(c) for c in counts], [float(e) for e in edges]

    # ----- rundb I/O -------------------------------------------------------

    def _read_saved_rundb_value(self) -> float:
        """Look up the currently-saved ``streaming_n_sigma_threshold``
        for the active run; ``0.0`` when the rundb / run id is unknown
        or the field has never been set."""
        if self._database_path is None or self._run_id is None:
            return 0.0
        try:
            records = rundb.load_database(self._database_path)
        except Exception:  # noqa: BLE001
            return 0.0
        for r in records:
            if r.run_id == self._run_id:
                v = r.get(self._SAVE_FIELD)
                try:
                    return float(v) if v is not None else 0.0
                except (TypeError, ValueError):
                    return 0.0
        return 0.0

    # ----- rendering -------------------------------------------------------

    def _render_axes(self) -> None:
        """Initial paint — overlay the two step plots + place the cut
        line and the saved-value ghost.  Re-runs only on construction;
        subsequent updates touch just the cut-line artist."""
        ax = self._ax
        # Step hists.  ``where='post'`` mirrors how matplotlib renders
        # bin counts as constant-on-bin-interval, same convention
        # ``thumbs.render_histogram`` uses for TH1s.
        #  Colour scheme matches the C++ writer's 05_streaming_score PDF:
        #  DCR / noise (first-frames) = blue, signal / data-taking = red,
        #  in-beam bkg (pre-trigger) = violet.
        ax.step(self._edges[:-1], self._data_counts, where="post",
                color="#D04A4A" if self._dark else "#B03030",
                linewidth=1.6, label="signal (data-taking)")
        ax.step(self._edges[:-1], self._noise_counts, where="post",
                color="#4A8BD0" if self._dark else "#1F4E8B",
                linewidth=1.6, label="DCR (first-frames)")
        if any(c > 0 for c in self._inbeam_counts):
            ax.step(self._edges[:-1], self._inbeam_counts, where="post",
                    color="#B07AD0" if self._dark else "#8030B0",
                    linewidth=1.6, label="in-beam bkg (pre-trigger)")
        max_y = max([1.0, *self._noise_counts, *self._data_counts,
                     *self._inbeam_counts])
        #  Log-Y is the natural axis for the score (heavy noise tail,
        #  long data tail), but matplotlib emits a warning + flips to
        #  lin internally when both samples are empty.  Pre-empt that
        #  by staying on lin Y when there's literally nothing to show
        #  — the operator sees an empty axes box, not a stack trace.
        if max_y > 1.0:
            ax.set_yscale("log")
            ax.set_ylim(0.5, max_y * 2.0)
        else:
            ax.set_ylim(0.0, 1.0)
        ax.set_xlabel("n_σ")
        ax.set_ylabel("entries")
        ax.set_xlim(self._edges[0], self._edges[-1])
        ax.grid(True, which="both", alpha=0.25, linewidth=0.5)
        ax.legend(loc="upper right", framealpha=0.85, fontsize=9)

        # Ghost line — previously-saved rundb value, dimmed.  Skipped
        # when nothing is saved yet.
        self._ghost_artist = None
        if self._rundb_saved_value > 0:
            self._ghost_artist = ax.axvline(
                self._rundb_saved_value,
                color="#888888", linestyle=":", linewidth=1.2,
                label=f"saved ({self._rundb_saved_value:.2f})",
            )
            ax.legend(loc="upper right", framealpha=0.85, fontsize=9)

        # Live cut line — the one the operator drags.
        self._cut_artist = ax.axvline(
            self._cut,
            color="#0BDA51", linestyle="-", linewidth=1.8,
        )
        # Touch label so the X coord shows in the initial paint.
        self._cut_label.setText(f"n_σ  =  {self._cut:.2f}")

    def _refresh_stats(self) -> None:
        """Recompute the four side-panel quantities at the current cut."""
        from . import streaming_picker as _sp
        s = _sp.cut_stats(
            self._noise_counts, self._data_counts, self._edges, self._cut,
        )
        self._lbl_p_misfire.setText(self._fmt_prob(s.p_misfire))
        self._lbl_acceptance.setText(self._fmt_prob(s.acceptance))
        self._lbl_sn.setText("—" if s.sn_ratio is None else f"{s.sn_ratio:.2f}")
        self._lbl_n_above.setText(f"{s.n_above_data:.0f}")
        self._cut_label.setText(f"n_σ  =  {self._cut:.2f}")

    def _refresh_seed_note(self) -> None:
        """Surface which priority branch put the line where it is."""
        from . import streaming_picker as _sp
        if self._rundb_saved_value > 0:
            note = (f"Seeded from the saved rundb value "
                    f"({self._rundb_saved_value:.2f}).")
        elif 0 < self._conf_default < _sp.QA_DISABLE_SENTINEL:
            note = (f"Seeded from `conf/streaming.toml` default "
                    f"({self._conf_default:.2f}).")
        else:
            crossover = _sp.noise_data_crossover(
                self._noise_counts, self._data_counts, self._edges,
            )
            if crossover is not None:
                note = (f"Seeded from the noise/data crossover heuristic "
                        f"({crossover:.2f}).")
            else:
                note = "No usable seed — line placed at 5.0 as a fallback."
        self._seed_note.setText(note)

    @staticmethod
    def _fmt_prob(p: float) -> str:
        if p <= 0:
            return "0"
        if p < 1e-4:
            return f"{p:.2e}"
        return f"{p * 100:.3f} %"

    # ----- drag handling ---------------------------------------------------

    def _on_press(self, event) -> None:
        if event.inaxes is not self._ax or event.xdata is None:
            return
        if event.button != 1:  # left button only
            return
        # Tolerance: 5 % of the visible X span.  Generous enough that
        # the operator doesn't have to pixel-snipe the line; tight
        # enough that an accidental click far away doesn't snap it.
        lo, hi = self._ax.get_xlim()
        tol = 0.05 * (hi - lo)
        if abs(event.xdata - self._cut) <= tol:
            self._dragging = True
            self._set_cut(event.xdata)

    def _on_motion(self, event) -> None:
        if not self._dragging or event.inaxes is not self._ax:
            return
        if event.xdata is None:
            return
        self._set_cut(event.xdata)

    def _on_release(self, event) -> None:
        self._dragging = False

    def _set_cut(self, x: float) -> None:
        # Clamp to the booked range — dragging off-canvas would put
        # the saved value in nonsensical territory.
        x = max(self._edges[0], min(self._edges[-1], x))
        self._cut = x
        self._cut_artist.set_xdata([x, x])
        self._refresh_stats()
        self._canvas.draw_idle()

    # ----- save ------------------------------------------------------------

    def _on_save(self) -> None:
        if self._database_path is None or self._run_id is None:
            return
        try:
            rundb.update_run_field(
                self._database_path,
                self._run_id,
                self._SAVE_FIELD,
                round(float(self._cut), 4),
                auto_pin=True,
                source="dashboard:streaming_picker",
            )
        except Exception as exc:  # noqa: BLE001
            QtWidgets.QMessageBox.warning(
                self, "Save failed",
                f"Could not write `{self._SAVE_FIELD}` to "
                f"{self._database_path.name}:\n\n{type(exc).__name__}: {exc}",
            )
            return

        self._rundb_saved_value = float(self._cut)
        # Move (or paint) the saved-value ghost so the operator sees
        # confirmation without the dialog closing.
        if self._ghost_artist is None:
            self._ghost_artist = self._ax.axvline(
                self._rundb_saved_value,
                color="#888888", linestyle=":", linewidth=1.2,
                label=f"saved ({self._rundb_saved_value:.2f})",
            )
            self._ax.legend(loc="upper right", framealpha=0.85, fontsize=9)
        else:
            self._ghost_artist.set_xdata([
                self._rundb_saved_value, self._rundb_saved_value,
            ])
            self._ghost_artist.set_label(
                f"saved ({self._rundb_saved_value:.2f})"
            )
            self._ax.legend(loc="upper right", framealpha=0.85, fontsize=9)
        self._refresh_seed_note()
        self._canvas.draw_idle()


# ---------------------------------------------------------------------------
# Macros placeholder — bespoke ROOT macros to land later.
# ---------------------------------------------------------------------------


class _MacrosPlaceholder(QtWidgets.QWidget):
    def __init__(self, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(40, 40, 40, 40)
        outer.setAlignment(QtCore.Qt.AlignTop)

        card = QtWidgets.QFrame()
        card.setObjectName("cardSurface")
        v = QtWidgets.QVBoxLayout(card)
        v.setContentsMargins(20, 18, 20, 20)
        v.setSpacing(10)

        title = QtWidgets.QLabel("Macros")
        tf = title.font()
        tf.setBold(True)
        tf.setPointSize(tf.pointSize() + 3)
        title.setFont(tf)
        v.addWidget(title)

        body = QtWidgets.QLabel(
            "Curated launcher for bespoke ROOT macros — runs the chosen "
            "macro against the selected run, picks up its PDFs/outputs, "
            "and surfaces them inline.<br><br>"
            "Status: <i>placeholder — implementation pending</i>"
        )
        body.setTextFormat(QtCore.Qt.RichText)
        body.setWordWrap(True)
        body.setObjectName("muted")
        v.addWidget(body)

        outer.addWidget(card)
        outer.addStretch(1)


def _count_tree_hists(node: dict) -> int:
    """Recursive total of histograms in a ``{hists, subdirs}`` tree."""
    n = len(node.get("hists", []))
    for sub in node.get("subdirs", {}).values():
        n += _count_tree_hists(sub)
    return n


class _CollapsibleSection(QtWidgets.QFrame):
    """A collapsible General-overview chapter with a LAZILY-built body.

    A bold, clickable header (▾ / ▸) that shows/hides its body.  The body
    is constructed on the FIRST expand via the ``body_builder`` callback,
    not at creation — critical for load time, because each PDF tile blocks
    on a synchronous ``QPdfDocument`` render, so eagerly building every
    chapter's tiles made opening the QA page slow.  A collapsed chapter
    therefore costs nothing but its header.  Default collapsed.

    ``body_builder(section)`` is called once, with this section, and should
    populate it via :meth:`add_widget`.
    """

    def __init__(
        self,
        title: str,
        *,
        count_text: str = "",
        subtitle: str = "",
        expanded: bool = False,
        body_builder=None,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("cardSurface")
        self._title = title
        self._count_text = count_text
        self._body_builder = body_builder
        self._body_built = False
        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(14, 12, 14, 14)
        outer.setSpacing(8)

        self._toggle = QtWidgets.QToolButton()
        self._toggle.setCheckable(True)
        self._toggle.setChecked(expanded)
        self._toggle.setStyleSheet(
            "QToolButton { border: none; text-align: left;"
            "             padding: 2px 4px; }"
        )
        f = self._toggle.font()
        f.setBold(True)
        f.setPointSize(f.pointSize() + 1)
        self._toggle.setFont(f)
        self._toggle.toggled.connect(self._on_toggle)
        outer.addWidget(self._toggle)

        if subtitle:
            sub = QtWidgets.QLabel(subtitle)
            sub.setObjectName("muted")
            sub.setStyleSheet("font-style: italic; font-size: 11px;")
            sub.setWordWrap(True)
            outer.addWidget(sub)

        self._body = QtWidgets.QWidget()
        self._body_layout = QtWidgets.QVBoxLayout(self._body)
        self._body_layout.setContentsMargins(0, 0, 0, 0)
        self._body_layout.setSpacing(10)
        self._body.setVisible(expanded)
        outer.addWidget(self._body)

        if expanded:
            self._build_body()
        self._sync_header(expanded)

    def add_widget(self, w: QtWidgets.QWidget) -> None:
        self._body_layout.addWidget(w)

    def _build_body(self) -> None:
        if self._body_built:
            return
        self._body_built = True
        if self._body_builder is not None:
            self._body_builder(self)

    def _sync_header(self, expanded: bool) -> None:
        arrow = "▾" if expanded else "▸"
        suffix = f"    ·  {self._count_text}" if self._count_text else ""
        self._toggle.setText(f"{arrow}  {self._title}{suffix}")

    def _on_toggle(self, expanded: bool) -> None:
        if expanded:
            self._build_body()
        self._body.setVisible(expanded)
        self._sync_header(expanded)


class _CollapsibleDirCard(QtWidgets.QFrame):
    """One collapsible card per TDirectory; renders nested subdirs too.

    Default state: **collapsed** — only the header (folder name +
    histogram count) is visible.  Click the header to expand.  Sub-
    directories render as nested ``_CollapsibleDirCard`` instances
    inside the body, also collapsed by default.  Arbitrary nesting
    depth — sub-sub-folders just get a deeper indent.
    """

    def __init__(
        self,
        root_path: Path,
        name: str,
        hists: list[tuple[str, str]],
        subdirs: dict,
        dark: bool,
        FigureCanvas,
        Figure,
        *,
        depth: int = 0,
        database_path: Optional[Path] = None,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._root_path = root_path
        self._hists = hists
        self._subdirs = subdirs
        self._dark = dark
        self._FigureCanvas = FigureCanvas
        self._Figure = Figure
        self._depth = depth
        #  Threaded down from QaView so the streaming-score picker
        #  inside ``_ThumbnailTile`` knows which rundb file to write
        #  to.  None disables Save in the picker dialog.
        self._database_path = database_path
        # Lazy-build body the first time the operator expands the card,
        # so the page open cost is just headers (cheap).
        self._body_built = False

        # Outer styling — top-level uses cardSurface, nested cards
        # get a slightly subtler look to read as "inside".
        self.setObjectName("cardSurface" if depth == 0 else "")
        if depth == 0:
            self.setFrameShape(QtWidgets.QFrame.StyledPanel)

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(
            12 if depth == 0 else 6,
            8  if depth == 0 else 4,
            12 if depth == 0 else 6,
            8  if depth == 0 else 4,
        )
        outer.setSpacing(4)

        # ── Header (always visible, clickable) ──────────────────────
        total_n = len(hists) + sum(
            _count_tree_hists(s) for s in subdirs.values()
        )
        self._toggle = QtWidgets.QToolButton()
        self._toggle.setCheckable(True)
        self._toggle.setChecked(False)
        self._toggle.setText(self._header_text(name, total_n, expanded=False))
        f = self._toggle.font()
        f.setBold(True)
        # Slightly larger header at depth 0 so the top-level grouping
        # reads as the primary structure.
        if depth == 0:
            f.setPointSize(f.pointSize() + 1)
        self._toggle.setFont(f)
        self._toggle.setStyleSheet(
            "QToolButton { border: none; text-align: left;"
            "             padding: 2px 4px; }"
        )
        self._toggle.toggled.connect(self._on_toggle)
        outer.addWidget(self._toggle)

        # ── Body (hidden until first expand) ────────────────────────
        self._body = QtWidgets.QWidget()
        self._body_layout = QtWidgets.QVBoxLayout(self._body)
        self._body_layout.setContentsMargins(
            16, 4, 0, 4,    # indent body so nesting reads visually
        )
        self._body_layout.setSpacing(8)
        self._body.setVisible(False)
        outer.addWidget(self._body)

        # Remember the name for the header arrow toggle.
        self._name = name
        self._total_n = total_n

    # ----- helpers -----------------------------------------------------

    @staticmethod
    def _header_text(name: str, count: int, *, expanded: bool) -> str:
        arrow = "▾" if expanded else "▸"
        return f"{arrow}  {name}    ·  {count}"

    def _on_toggle(self, expanded: bool) -> None:
        if expanded and not self._body_built:
            self._build_body()
            self._body_built = True
        self._body.setVisible(expanded)
        self._toggle.setText(
            self._header_text(self._name, self._total_n, expanded=expanded)
        )

    def _build_body(self) -> None:
        """Populate the body with thumbnail grid + nested cards."""
        # Histogram thumbnails for THIS directory — responsive grid
        # so 1/2/3/4 thumbs per row tracks the dashboard window width.
        if self._hists:
            tiles = [
                _ThumbnailTile(
                    self._root_path, hpath, classname,
                    self._FigureCanvas, self._Figure,
                    dark=self._dark,
                    database_path=self._database_path,
                )
                for hpath, classname in self._hists
            ]
            self._body_layout.addWidget(_ResponsiveTileGrid(
                tiles, tile_min_width=_THUMB_TILE_MIN_PX,
            ))

        # Nested sub-directory cards.
        for subname, subnode in self._subdirs.items():
            self._body_layout.addWidget(_CollapsibleDirCard(
                self._root_path, subname,
                subnode["hists"], subnode["subdirs"],
                self._dark, self._FigureCanvas, self._Figure,
                depth=self._depth + 1,
                database_path=self._database_path,
            ))


class _TopicQaPage(QtWidgets.QWidget):
    """One topic sub-tab.  Aggregates PDFs from every pipeline stage
    whose basename matches ``topic_of()`` for this topic.

    Mirrors the lazy-rebuild + de-dup-on-set-run discipline of
    ``_StepQaPage`` but operates on a *cross-stage* PDF set.  Each
    matching PDF is shown under a small header that names the
    originating writer (Lightdata / Recodata / …) so the operator
    knows which writer to ping when a plot looks wrong.

    No histogram thumbnails here by design — those live on the
    "Full plots" deep-dive sub-tab, which is one level down.
    """

    def __init__(
        self,
        topic_key: str,
        label: str,
        description: str,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._topic_key = topic_key
        self._label = label
        self._description = description
        self._current_run_id: Optional[str] = None
        self._current_data_dir: Optional[Path] = None

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(0, 8, 0, 0)
        outer.setSpacing(8)

        # Topic description line — operator sees what's grouped here.
        self._desc = QtWidgets.QLabel(description)
        self._desc.setObjectName("muted")
        self._desc.setStyleSheet("font-style: italic; font-size: 11px;")
        self._desc.setWordWrap(True)
        outer.addWidget(self._desc)

        # Compact status header — what's loaded right now.
        self._status = QtWidgets.QLabel("(no run selected)")
        self._status.setObjectName("muted")
        self._status.setStyleSheet("font-style: italic; font-size: 11px;")
        outer.addWidget(self._status)

        # Scrollable body — one card per pipeline stage that contributed.
        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        self._body = QtWidgets.QWidget()
        self._body_layout = QtWidgets.QVBoxLayout(self._body)
        self._body_layout.setContentsMargins(0, 0, 0, 0)
        self._body_layout.setSpacing(12)
        self._body_layout.setAlignment(QtCore.Qt.AlignTop)
        self._scroll.setWidget(self._body)
        outer.addWidget(self._scroll, 1)

    def set_run(self, run_id: Optional[str], data_dir: Path) -> None:
        if run_id == self._current_run_id and data_dir == self._current_data_dir:
            return
        self._current_run_id = run_id
        self._current_data_dir = data_dir
        self._rebuild()

    def invalidate_cache(self) -> None:
        self._current_run_id = None
        self._current_data_dir = None

    def _rebuild(self) -> None:
        # Wipe.
        while self._body_layout.count():
            item = self._body_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()

        run_id = self._current_run_id
        data_dir = self._current_data_dir
        if not run_id or data_dir is None:
            self._status.setText("(no run selected)")
            return
        run_dir = data_dir / run_id
        if not run_dir.is_dir():
            self._status.setText(f"run directory not found: {run_dir}")
            return

        # Try the Qt PDF module — same fallback as _StepQaPage.
        try:
            from PySide6 import QtPdf, QtPdfWidgets  # noqa: F401
            have_qt_pdf = True
        except ImportError:
            have_qt_pdf = False

        # Walk every STEP, collect PDFs that route to THIS topic.
        n_pdfs_total = 0
        contributing_stages: list[str] = []
        for _key, stage_label, _root, qa_subdir in STEPS:
            stage_dir = run_dir / qa_subdir
            if not stage_dir.is_dir():
                continue
            matching = sorted(
                p for p in stage_dir.glob("*.pdf")
                if topic_of(p.name) == self._topic_key
            )
            if not matching:
                continue

            # Per-stage card.
            holder = QtWidgets.QFrame()
            holder.setObjectName("cardSurface")
            v = QtWidgets.QVBoxLayout(holder)
            v.setContentsMargins(14, 12, 14, 14)
            v.setSpacing(10)
            head = QtWidgets.QHBoxLayout()
            title = QtWidgets.QLabel(
                f"<b>{self._label}</b>  ·  from <i>{stage_label}</i>")
            title.setTextFormat(QtCore.Qt.RichText)
            head.addWidget(title)
            head.addStretch(1)
            cap = QtWidgets.QLabel(f"{len(matching)}")
            cap.setObjectName("muted")
            cap.setStyleSheet("font-size: 11px;")
            head.addWidget(cap)
            v.addLayout(head)

            tiles = [_PdfTile(p, have_qt_pdf=have_qt_pdf) for p in matching]
            v.addWidget(_ResponsiveTileGrid(
                tiles, tile_min_width=_PDF_TILE_MIN_PX,
            ))
            self._body_layout.addWidget(holder)
            contributing_stages.append(stage_label)
            n_pdfs_total += len(matching)

        bits = [f"run: {run_id}"]
        if n_pdfs_total:
            bits.append(f"{n_pdfs_total} PDF(s) from {', '.join(contributing_stages)}")
        else:
            bits.append(f"no {self._label.lower()} plots yet — writer hasn't emitted any")
        self._status.setText("  ·  ".join(bits))


# ---------------------------------------------------------------------------
# Cross-run trend tile — matplotlib line plot of one metric across the
# most-recent N runs.  Lives on the General tab below the headline PDFs.
# ---------------------------------------------------------------------------


class _TrendTile(QtWidgets.QFrame):
    """One observable plotted across the trend window.

    Mirrors the ``_ThumbnailTile`` shell (cardSurface frame +
    FigureCanvas + caption) so the General tab's two tile families
    visually agree.  Differences from ``_ThumbnailTile``:

      - data source is a :class:`cross_run_trends.TrendSeries`, not a
        ROOT histogram — no uproot involvement, no modal "Open in
        ROOT" path (trend data isn't in a TFile),
      - we draw run-id ticks on the x-axis with the time portion only
        (``HHMMSS``) so 20 ticks still fit on a ~320 px tile,
      - "no data" is a first-class state: when the series has zero
        points (writer hasn't published the metric yet) we draw a
        muted placeholder instead of an empty axes — the operator's
        question "is this metric live?" is answered at a glance.

    Errorbars are drawn iff at least one point carries ``error > 0``.
    A footer line summarises window-completeness ("18 / 20 runs · 2
    missing") — silent gaps would let a slow-failing writer look like
    flat steady-state data.
    """

    _TREND_HEIGHT_PX = 220

    def __init__(
        self,
        series: cross_run_trends.TrendSeries,
        FigureCanvas,
        Figure,
        *,
        dark: bool = False,
        window_size: int = cross_run_trends.DEFAULT_TREND_RUNS_N,
    ) -> None:
        super().__init__()
        self._series = series
        self._dark = dark
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)

        v = QtWidgets.QVBoxLayout(self)
        v.setContentsMargins(6, 6, 6, 6)
        v.setSpacing(2)

        #  Figure sized to match the histogram thumbs (3.6 × 2.4 @ 100
        #  dpi) so a row mixing headline PDFs / histograms / trends
        #  stays visually aligned.
        fig = Figure(figsize=(3.6, 2.4), dpi=100, tight_layout=True)
        fig.patch.set_alpha(0.0)
        ax = fig.add_subplot(111)
        ax.patch.set_alpha(0.0)
        self._render_into_ax(ax)
        canvas = FigureCanvas(fig)
        canvas.setFixedHeight(self._TREND_HEIGHT_PX)
        v.addWidget(canvas)

        #  Caption: metric label + (#points / #window · missing-count).
        n_pts = len(series.points)
        n_missing = len(series.missing)
        denom = max(n_pts + n_missing, window_size)
        cap_text = (
            f"<b>{series.metric.label}</b>  ·  {n_pts} / {denom} runs"
        )
        if n_missing:
            cap_text += f"  ·  {n_missing} missing"
        cap = QtWidgets.QLabel(cap_text)
        cap.setObjectName("muted")
        cap.setTextFormat(QtCore.Qt.RichText)
        cap.setStyleSheet("font-size: 9px;")
        cap.setWordWrap(True)
        cap.setToolTip(
            f"{series.metric.key}  ·  sensor={series.metric.sensor}\n"
            "Most-recent runs left-to-right (oldest → newest)."
        )
        v.addWidget(cap)

    def _render_into_ax(self, ax) -> None:
        """Draw the trend line, or a 'no data yet' placeholder.

        Theme-aware tick / label colour so the figure stays legible
        on both the light and dark cards.  Run-id strings live on
        the x-tick labels (compact ``HHMMSS`` slice) so the operator
        can correlate a step / drift with a specific run without
        leaving the tab.
        """
        fg = "#E8E8E8" if self._dark else "#2A2A2A"
        muted = "#9CA0A6" if self._dark else "#6B6F76"

        series = self._series
        if not series.points:
            ax.text(
                0.5, 0.5,
                "(no data yet)\n"
                f"writer hasn't published\n{series.metric.key}",
                ha="center", va="center", transform=ax.transAxes,
                fontsize=8, color=muted,
            )
            ax.set_xticks([])
            ax.set_yticks([])
            return

        xs = list(range(len(series.points)))
        ys = [p.value for p in series.points]
        errs = [p.error for p in series.points]
        any_err = any(e > 0 for e in errs)

        if any_err:
            ax.errorbar(
                xs, ys, yerr=errs,
                fmt="o-", markersize=3.5, linewidth=1.0,
                capsize=2.0, elinewidth=0.8,
                color=fg,
            )
        else:
            ax.plot(
                xs, ys,
                marker="o", markersize=3.5, linewidth=1.0,
                color=fg,
            )

        #  Full run id (``YYYYMMDD-HHMMSS``) on the x-ticks so the date
        #  is visible, not just the time — the label thinning below
        #  keeps it to ~8 ticks so the full ids still fit on the tile.
        labels = [p.run_id for p in series.points]
        #  Thin the labels if the window is dense — show at most
        #  ~8 ticks even at N = 20+.
        stride = max(1, len(xs) // 8)
        tick_idx = list(range(0, len(xs), stride))
        ax.set_xticks(tick_idx)
        ax.set_xticklabels(
            [labels[i] for i in tick_idx],
            rotation=45, ha="right", fontsize=7, color=fg,
        )
        ax.tick_params(axis="y", labelsize=7, colors=fg)
        for spine in ax.spines.values():
            spine.set_color(muted)
        #  Y-axis carries the metric label (so the σ tile reads
        #  "Photon-yield σ" rather than the bare unit it shares with
        #  ⟨N_γ⟩); unit appended when present.
        y_lbl = series.metric.label
        if series.metric.unit:
            y_lbl += f"  [{series.metric.unit}]"
        ax.set_ylabel(y_lbl, fontsize=7, color=fg)
        if series.metric.y_floor_zero:
            ax.set_ylim(bottom=0)
        ax.grid(True, linestyle=":", linewidth=0.4, alpha=0.5, color=muted)


class _GeneralQaPage(QtWidgets.QWidget):
    """The 'first thing the shifter sees' overview.

    Organised into four thematic rows — each row answers a single
    shifter question ("is anything happening?", "are the sensors OK?",
    "is the physics there?", "are the clocks aligned?") with 2-3
    curated PDFs.  A missing PDF is silently dropped from its row;
    a row with zero hits is hidden entirely so the page never shows
    an empty header.

    Below the four thematic rows the page surfaces a dynamic
    per-hardware-trigger anchor-Δt fan-out (one tile per registered
    trigger that actually fired) and the cross-run trend section.

    The selection is hard-coded (not regex-based) because each row's
    membership is a deliberate editorial choice, not a rule.  Order
    within a row dictates left-to-right tile order on a wide grid.
    """

    # Row layout — one card per row.  Each entry is
    # ``(row_label, ordered tuple of PDF basenames)``.
    # PDF basenames are matched after stripping the ``NN_`` writer-order
    # prefix (same convention as ``topic_of``).
    #  Curated collapsible chapters.  The per-trigger "Triggers" chapter is
    #  built separately (one row per fired trigger) and inserted after Timing.
    _GENERAL_ROWS: tuple[tuple[str, tuple[str, ...]], ...] = (
        ("Data-taking health", (
            "trigger_matrix.pdf",
            "frames_per_spill.pdf",
            "trigger_qa.pdf",
            "coverage_map_xy.pdf",          # cartesian coverage + readiness %
        )),
        ("Sensor health", (
            "dcr_per_channel.pdf",          # per-channel DCR rate (kHz) + averages
            "dcr_hitmap.pdf",
            "afterpulse_per_channel.pdf",   # per-channel afterpulse % + averages
            "afterpulse_hitmap.pdf",
        )),
        ("Calibration", (
            "anchor_dt_vs_spill.pdf",       # channel hit vs nearest anchor pulse
            "anchor_dt_1d.pdf",             # channel-anchor Δt 1D (coincidence dist.)
            "coincidence_map.pdf",          # laser spot: per-pixel coincidence map
            "anchor_consecutive_dt.pdf",    # anchor cadence: Gaussian period + rate
        )),
        ("Timing", (
            "timing_alignment.pdf",
            "timing_hit_map.pdf",           # chip-0 vs chip-1 coincidence occupancy
            "timing_dcr_per_channel.pdf",   # per-channel timing-sensor DCR (kHz) + per-chip avgs
        )),
        ("Cherenkov", (
            "trigger_cherenkov_hitmap.pdf",   # in-cut trigger-Cherenkov hits → the ring
            "ring_centre_xy.pdf",
            "N_gamma_per_ring_summary.pdf",   # N_photons per ring
            "sigma_photon_summary.pdf",       # single-photon σ per ring
            "streaming_score.pdf",
            "radial_fit_ring1.pdf",           # ring 1 radial Gauss+pol3 fit → N_γ
            "radial_fit_ring2.pdf",           # ring 2
            "radial_fit_ring1_dual.pdf",      # ring 1, dual-ring events
            "radial_fit_ring1_solo.pdf",      # ring 1, solo-ring events
        )),
    )

    #  Per-trigger standard plot set for the Triggers chapter — one row per
    #  fired trigger, these filename stems (with the trigger name appended)
    #  laid out left-to-right in this order.  Missing ones are dropped.
    _PER_TRIGGER_PLOTS: tuple[str, ...] = (
        "anchor_dt_",        # same-frame Δt vs spill (DCR baseline)
        "trigger_dt_",       # consecutive-firing Δt vs spill (rate stability)
        "trigcher_dt_",      # trigger–Cherenkov Δt (coincidence timing)
        "trigcher_hitmap_",  # in-window Cherenkov hitmap (the ring)
    )

    def __init__(self, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._current_run_id: Optional[str] = None
        self._current_data_dir: Optional[Path] = None

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(0, 8, 0, 0)
        outer.setSpacing(8)

        intro = QtWidgets.QLabel(
            "Curated overview — one or two headline plots per topic.  "
            "For everything else, jump to the topic tabs above, or "
            "Full plots for the per-writer drill-down."
        )
        intro.setObjectName("muted")
        intro.setStyleSheet("font-style: italic; font-size: 11px;")
        intro.setWordWrap(True)
        outer.addWidget(intro)

        self._status = QtWidgets.QLabel("(no run selected)")
        self._status.setObjectName("muted")
        self._status.setStyleSheet("font-style: italic; font-size: 11px;")
        outer.addWidget(self._status)

        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        self._body = QtWidgets.QWidget()
        self._body_layout = QtWidgets.QVBoxLayout(self._body)
        self._body_layout.setContentsMargins(0, 0, 0, 0)
        self._body_layout.setSpacing(12)
        self._body_layout.setAlignment(QtCore.Qt.AlignTop)
        self._scroll.setWidget(self._body)
        outer.addWidget(self._scroll, 1)

    def set_run(self, run_id: Optional[str], data_dir: Path) -> None:
        if run_id == self._current_run_id and data_dir == self._current_data_dir:
            return
        self._current_run_id = run_id
        self._current_data_dir = data_dir
        self._rebuild()

    def invalidate_cache(self) -> None:
        self._current_run_id = None
        self._current_data_dir = None

    def _rebuild(self) -> None:
        while self._body_layout.count():
            item = self._body_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()

        run_id = self._current_run_id
        data_dir = self._current_data_dir
        if not run_id or data_dir is None:
            self._status.setText("(no run selected)")
            return
        run_dir = data_dir / run_id
        if not run_dir.is_dir():
            self._status.setText(f"run directory not found: {run_dir}")
            return

        try:
            from PySide6 import QtPdf, QtPdfWidgets  # noqa: F401
            have_qt_pdf = True
        except ImportError:
            have_qt_pdf = False

        # Index every emitted PDF by its (stripped-prefix) basename so
        # the headline picks are O(K) lookups instead of O(K·N) scans.
        emitted: dict[str, Path] = {}
        for _key, _label, _root, qa_subdir in STEPS:
            stage_dir = run_dir / qa_subdir
            if not stage_dir.is_dir():
                continue
            for p in stage_dir.glob("*.pdf"):
                emitted[_strip_pdf_prefix(p.name)] = p

        # Resolve thematic rows: per row, walk the configured PDF
        # basenames and surface each one that exists on disk for this
        # run.  Missing PDFs are silently dropped (not every writer
        # has emitted every plot yet — e.g. a fresh run that hasn't
        # been through recodata yet still shows its lightdata row).
        # All tiles use square aspect so the four rows align visually.
        #  Store the PDF PATHS per chapter — the _PdfTile widgets (each a
        #  blocking render) are built lazily by the chapter's body_builder on
        #  first expand, so opening the page costs only headers.
        rows_with_paths: list[tuple[str, list[Path]]] = []
        total_headline_tiles = 0
        for row_label, picks in self._GENERAL_ROWS:
            paths = [emitted[pick] for pick in picks if pick in emitted]
            if paths:
                rows_with_paths.append((row_label, paths))
                total_headline_tiles += len(paths)

        #  Triggers chapter: one row per fired trigger.  The fired-trigger
        #  set is discovered from the per-trigger PDF stems (filesystem-
        #  driven, so a newly-wired trigger appears automatically), and each
        #  trigger's row carries the standard plot set in _PER_TRIGGER_PLOTS
        #  order (anchor Δt · consecutive Δt · trigger–Cherenkov Δt · in-window
        #  hitmap); missing plots are dropped.
        def _trigger_of(stem: str) -> Optional[str]:
            for prefix in self._PER_TRIGGER_PLOTS:
                if stem.startswith(prefix):
                    name = stem[len(prefix):]
                    #  anchor_dt_vs_spill is the pulser calibration plot
                    #  (Timing chapter), not a per-trigger fan-out member.
                    if prefix == "anchor_dt_" and name == "vs_spill":
                        return None
                    return name
            return None

        trigger_names: set[str] = set()
        for stem in emitted:
            base = stem[:-4] if stem.endswith(".pdf") else stem
            tname = _trigger_of(base)
            if tname:
                trigger_names.add(tname)

        trigger_rows: list[tuple[str, list[Path]]] = []
        n_trigger_tiles = 0
        for tname in sorted(trigger_names):
            paths = [emitted[f"{prefix}{tname}.pdf"]
                     for prefix in self._PER_TRIGGER_PLOTS
                     if f"{prefix}{tname}.pdf" in emitted]
            if paths:
                trigger_rows.append((tname, paths))
                n_trigger_tiles += len(paths)

        def _make_tiles(paths: list[Path]) -> list[QtWidgets.QWidget]:
            return [_PdfTile(p, have_qt_pdf=have_qt_pdf, aspect_override=1.0)
                    for p in paths]

        # One collapsible chapter per non-empty curated row, plus the
        # per-trigger Triggers chapter inserted right after Timing.  Chapters
        # are expanded by default — their PDF tiles render in the background
        # (see _ThumbRenderQueue), so building them all up-front no longer
        # blocks; the tiles just fill in progressively while you browse.
        def _render_curated_chapter(label: str, paths: list[Path]) -> None:
            picks = next(
                (p for lbl, p in self._GENERAL_ROWS if lbl == label), ())

            def builder(sec: _CollapsibleSection) -> None:
                sec.add_widget(_ResponsiveTileGrid(
                    _make_tiles(paths), tile_min_width=_PDF_TILE_MIN_PX))

            self._body_layout.addWidget(_CollapsibleSection(
                label, count_text=f"{len(paths)} of {len(picks)}",
                expanded=True, body_builder=builder))

        def _render_triggers_chapter() -> None:
            if not trigger_rows:
                return

            def builder(sec: _CollapsibleSection) -> None:
                for tname, paths in trigger_rows:
                    row_box = QtWidgets.QWidget()
                    rb = QtWidgets.QVBoxLayout(row_box)
                    rb.setContentsMargins(0, 0, 0, 0)
                    rb.setSpacing(4)
                    lbl = QtWidgets.QLabel(f"<b>{tname}</b>")
                    lbl.setTextFormat(QtCore.Qt.RichText)
                    rb.addWidget(lbl)
                    rb.addWidget(_ResponsiveTileGrid(
                        _make_tiles(paths), tile_min_width=_PDF_TILE_MIN_PX))
                    sec.add_widget(row_box)

            self._body_layout.addWidget(_CollapsibleSection(
                "Triggers",
                count_text=f"{len(trigger_rows)} trigger(s) fired",
                subtitle=(
                    "One row per fired trigger — anchor Δt vs spill · "
                    "consecutive-firing Δt · trigger–Cherenkov Δt · "
                    "in-window Cherenkov hitmap (the config timing cut)."),
                expanded=True, body_builder=builder))

        rendered_triggers = False
        for row_label, paths in rows_with_paths:
            _render_curated_chapter(row_label, paths)
            if row_label == "Timing":
                _render_triggers_chapter()
                rendered_triggers = True
        if not rendered_triggers:
            #  Timing chapter was empty (or absent) — append Triggers at the
            #  end so it still shows.
            _render_triggers_chapter()

        #  Cross-run trends — independent of headlines.  Even when a
        #  fresh run hasn't emitted PDFs yet, the trend tiles still
        #  surface what the writers DID publish to standard_results.toml
        #  for this and the prior runs, so the operator can spot a
        #  drift that the per-run PDFs can't show on their own.
        n_trend_pts = self._append_trend_section(data_dir)

        if (total_headline_tiles == 0
                and n_trigger_tiles == 0
                and n_trend_pts == 0):
            self._status.setText(
                f"run: {run_id}  ·  no headline plots emitted yet")
            return

        bits = []
        if total_headline_tiles:
            bits.append(
                f"{total_headline_tiles} headline plot(s) "
                f"in {len(rows_with_paths)} chapter(s)")
        if n_trigger_tiles:
            bits.append(f"{len(trigger_rows)} trigger row(s)")
        if n_trend_pts:
            bits.append(f"{n_trend_pts} trend point(s)")
        self._status.setText(f"run: {run_id}  ·  " + "  ·  ".join(bits))

    def _append_trend_section(self, data_dir: Path) -> int:
        """Append the cross-run trend tiles to the body layout.

        ``standard_results.toml`` lives next to the ``Data/`` directory
        (i.e. ``data_dir.parent / standard_results.toml``) — that's
        where the writers publish it.  Dashboard config knob
        ``[qa_general] trend_runs_n`` (default 20) sets the window.

        Returns the total number of trend points drawn across all
        tiles — used by the caller to pick the right status text.
        Returns 0 when matplotlib isn't installed (graceful degrade:
        the headline section above still shows) or no metric has any
        data — the trend section then renders nothing rather than an
        empty card frame.
        """
        try:
            import matplotlib  # noqa: F401
            matplotlib.use("QtAgg", force=False)
            from matplotlib.backends.backend_qtagg import FigureCanvas
            from matplotlib.figure import Figure
        except ImportError:
            return 0

        repo_root = data_dir.parent
        #  The writers publish to ``<data_repository>/standard_results.toml``
        #  — i.e. INSIDE the Data dir, not next to it.  (The C++ comments
        #  say "<repo>/…" but data_repository is the Data path, so the
        #  file lands in Data/.)  Read it where it's actually written;
        #  reading repo_root/ left the trends silently empty.
        results_path = data_dir / "standard_results.toml"
        config_path = repo_root / "qa_quicklook" / "qa_quicklook.toml"
        n_runs = cross_run_trends.read_trend_runs_n(config_path)
        all_series = cross_run_trends.load_trends(
            results_path, n_runs=n_runs,
        )
        if not all_series:
            return 0

        total_points = sum(len(s.points) for s in all_series)
        if total_points == 0:
            #  Every configured metric came back empty — no point
            #  rendering a card full of "(no data yet)" placeholders
            #  on the very first run of a campaign.  The status line
            #  caller handles the "writers haven't published yet"
            #  messaging.
            return 0

        dark = _dark_theme_active()
        tiles = [
            _TrendTile(s, FigureCanvas, Figure, dark=dark, window_size=n_runs)
            for s in all_series
        ]

        holder = QtWidgets.QFrame()
        holder.setObjectName("cardSurface")
        v = QtWidgets.QVBoxLayout(holder)
        v.setContentsMargins(14, 12, 14, 14)
        v.setSpacing(10)
        head = QtWidgets.QLabel(
            f"<b>Cross-run trends</b>  ·  last {n_runs} runs  ·  "
            f"{len(tiles)} metric(s)"
        )
        head.setTextFormat(QtCore.Qt.RichText)
        v.addWidget(head)
        sub = QtWidgets.QLabel(
            "Sourced from <code>standard_results.toml</code> — what the "
            "writers publish to <code>AnalysisResults</code>.  "
            "Tiles read left-to-right as time (oldest → newest)."
        )
        sub.setObjectName("muted")
        sub.setTextFormat(QtCore.Qt.RichText)
        sub.setStyleSheet("font-size: 10px;")
        sub.setWordWrap(True)
        v.addWidget(sub)
        v.addWidget(_ResponsiveTileGrid(
            tiles, tile_min_width=_TREND_TILE_MIN_PX,
        ))
        self._body_layout.addWidget(holder)
        return total_points


class _FullPlotsQaPage(QtWidgets.QWidget):
    """The legacy pipeline-stage view, demoted to a deep-dive sub-tab.

    Wraps a nested QTabBar with one ``_StepQaPage`` per pipeline stage
    (Lightdata / Recodata / Recotrack / Pulser calib) so power users
    can still drill into a specific writer's output without leaving
    the dashboard.  This is also where histograms live — the topic
    tabs above show PDFs only, by design.
    """

    def __init__(self, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._current_run_id: Optional[str] = None
        self._current_data_dir: Optional[Path] = None

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(0, 8, 0, 0)
        outer.setSpacing(4)

        intro = QtWidgets.QLabel(
            "Deep dive: PDFs + TH* histograms per pipeline writer.  "
            "Use this when the topic tabs don't surface what you need."
        )
        intro.setObjectName("muted")
        intro.setStyleSheet("font-style: italic; font-size: 11px;")
        intro.setWordWrap(True)
        outer.addWidget(intro)

        # Nested tab bar for the four stages.
        self._stage_bar = QtWidgets.QTabBar()
        self._stage_bar.setDocumentMode(True)
        self._stage_bar.setExpanding(False)
        outer.addWidget(self._stage_bar)
        self._stage_stack = QtWidgets.QStackedWidget()
        self._stage_bar.currentChanged.connect(self._stage_stack.setCurrentIndex)
        self._stage_bar.currentChanged.connect(self._on_stage_changed)
        outer.addWidget(self._stage_stack, 1)

        self._stage_pages: dict[str, _StepQaPage] = {}
        for key, label, root_name, qa_dir in STEPS:
            page = _StepQaPage(label, root_name, qa_dir)
            self._stage_bar.addTab(label)
            self._stage_stack.addWidget(page)
            self._stage_pages[key] = page

    def set_run(self, run_id: Optional[str], data_dir: Path) -> None:
        self._current_run_id = run_id
        self._current_data_dir = data_dir
        # Only forward to the visible stage; others are lazy.
        self._refresh_visible_stage()

    def set_database_path(self, path: Optional[Path]) -> None:
        """Forward the active rundb path to every stage page so the
        streaming-score picker (which lives inside the ``_StepQaPage``
        thumbnail tree) can save back to the right file."""
        for page in self._stage_pages.values():
            page.set_database_path(path)

    def invalidate_cache(self) -> None:
        for page in self._stage_pages.values():
            page.invalidate_cache()

    def _refresh_visible_stage(self) -> None:
        if not self._current_run_id or self._current_data_dir is None:
            return
        idx = self._stage_bar.currentIndex()
        if idx < 0:
            return
        page = self._stage_stack.widget(idx)
        if isinstance(page, _StepQaPage):
            page.set_run(self._current_run_id, self._current_data_dir)

    def _on_stage_changed(self, _idx: int) -> None:
        self._refresh_visible_stage()


__all__ = ["QaView"]
