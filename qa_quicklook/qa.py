"""QA tab — per-step sub-tabs (Lightdata / Recodata / Recotrack / Macros).

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

from pathlib import Path
from typing import Optional

from PySide6 import QtCore, QtGui, QtWidgets

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


# One sub-tab per pipeline step.  Each entry is:
#   (key, label, canonical_root, qa_subdir).
# ``canonical_root`` is the file we walk for TH1/TH2 thumbnails;
# ``qa_subdir`` is where we look for writer-emitted PDFs.  Order
# matches the pipeline (raw → reco → reco-track → standalone calib).
STEPS: tuple[tuple[str, str, str, str], ...] = (
    ("lightdata",   "Lightdata",     "lightdata.root",       "qa/lightdata"),
    ("recodata",    "Recodata",      "recodata.root",        "qa/recodata"),
    ("recotrack",   "Recotrack",     "recotrackdata.root",   "qa/recotrack"),
    ("calibration", "Pulser calib",  "pulser_calib_qa.root", "qa/calibration"),
)

# Per-step cap on matplotlib thumbnails — too many at once chokes the
# layout system.  PDFs are not capped (they're already curated by the
# writer that emitted them).
_MAX_THUMBS_PER_STEP = 12
_GRID_COLS = 3


class QaView(QtWidgets.QWidget):
    """QA tab — see module docstring."""

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

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(12, 12, 12, 12)
        outer.setSpacing(8)

        # ── Top bar: shared run picker + refresh ────────────────────
        # The run picker scans ``data_dir`` (NOT the database) so QA
        # offers the same set of runs the Run Manager does — only
        # runs whose files are actually on disk are listed.  The
        # refresh button forces the active sub-tab to invalidate its
        # cache + re-read, which is the answer to "a writer just
        # finished, where are its plots?" without making the operator
        # re-select the run.
        top_row = QtWidgets.QHBoxLayout()
        top_row.setSpacing(8)
        top_row.addWidget(QtWidgets.QLabel("Run:"))
        self._run_combo = QtWidgets.QComboBox()
        self._run_combo.setMinimumWidth(260)
        self._run_combo.currentTextChanged.connect(self._on_run_changed)
        top_row.addWidget(self._run_combo, 1)
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
        #   - off    → grey, "● Monitor"
        #   - on     → slow green pulse (≈1.6 s period — noticeable
        #              without inducing seizures), "● Monitor (on)"
        #   - error  → solid red, "● Monitor (err)"
        # The pulse uses a separate QTimer flipping a phase bool;
        # the stylesheet swap is cheap.
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

        # ── Body: sub-tabs per pipeline step ────────────────────────
        self._sub_tabs = QtWidgets.QTabWidget()
        self._sub_tabs.setTabPosition(QtWidgets.QTabWidget.North)
        self._sub_tabs.setDocumentMode(True)
        self._sub_tabs.tabBar().setExpanding(False)
        self._sub_tabs.currentChanged.connect(self._on_sub_tab_changed)
        outer.addWidget(self._sub_tabs, 1)

        self._step_pages: dict[str, _StepQaPage] = {}
        for key, label, root_name, qa_dir in STEPS:
            page = _StepQaPage(label, root_name, qa_dir)
            self._sub_tabs.addTab(page, label)
            self._step_pages[key] = page

        # Macros sub-tab — placeholder, fills in when bespoke macros
        # get a curated launcher.
        self._macros_page = _MacrosPlaceholder()
        self._sub_tabs.addTab(self._macros_page, "Macros")

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

    def _on_sub_tab_changed(self, _idx: int) -> None:
        self._refresh_current_page()

    def _on_refresh(self) -> None:
        """Re-scan Data/ + force the active page to rebuild from disk."""
        self.reload()
        # ``reload`` already calls ``_refresh_current_page``; force the
        # active step-page to forget its cache so the rebuild actually
        # walks the disk (set_run dedupes when (run, dir) is unchanged).
        page = self._sub_tabs.currentWidget()
        if isinstance(page, _StepQaPage):
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
            page = self._sub_tabs.currentWidget()
            if isinstance(page, _StepQaPage):
                page.invalidate_cache()
                page.set_run(self._current_run_id, self._data_dir)

    def _on_monitor_anim_value(self, color) -> None:
        """Apply the interpolated colour as the button background."""
        if not isinstance(color, QtGui.QColor):
            return
        self._monitor_btn.setStyleSheet(
            f"QPushButton {{ background-color: {color.name()};"
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

          - off    → no animation, neutral theme colours, label
                     "● Monitor".
          - on     → smooth ping-pong between two greens, 2200 ms
                     per half-cycle, ease in/out.  Calm "alive"
                     breathing.  Label "● Monitor (on)".
          - error  → smooth ping-pong between two reds, 700 ms per
                     half-cycle, linear.  Faster to flag attention
                     without strobing.  Label "● Monitor (err)".

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
            self._monitor_btn.setText(" ●  Monitor (on) ")
            self._monitor_anim.start()
        elif state == "error":
            #  Faster red pulse: deep crimson ↔ brighter red.  Linear
            #  easing reads as more urgent than the green's ease-in-out
            #  without being a full strobe.
            self._monitor_anim.setDuration(700)
            self._monitor_anim.setEasingCurve(QtCore.QEasingCurve.Linear)
            self._monitor_anim.setStartValue(QtGui.QColor("#8E2820"))
            self._monitor_anim.setEndValue(QtGui.QColor("#E74C3C"))
            self._monitor_btn.setText(" ●  Monitor (err) ")
            self._monitor_anim.start()
        else:
            # Idle / off — drop the stylesheet so the theme reclaims
            # control of the button look.
            self._monitor_btn.setStyleSheet("")
            self._monitor_btn.setText(" ●  Monitor ")

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
        page = self._sub_tabs.currentWidget()
        if isinstance(page, _StepQaPage):
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

        for pdf in pdfs:
            v.addWidget(_PdfTile(pdf, have_qt_pdf=have_qt_pdf))
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
            ))
        # Sub-directories at the top level, in order.
        for subname, subnode in tree["subdirs"].items():
            v.addWidget(_CollapsibleDirCard(
                path, subname, subnode["hists"], subnode["subdirs"],
                dark, FigureCanvas, Figure, depth=0,
            ))
        return holder, total


# ---------------------------------------------------------------------------
# PDF tile: inline QtPdf rendering of one .pdf file, or "open externally"
# fallback when PySide6.QtPdf isn't available.
# ---------------------------------------------------------------------------


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
    _THUMB_HEIGHT_PX = 240

    def __init__(
        self,
        pdf_path: Path,
        *,
        have_qt_pdf: bool,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._pdf_path = pdf_path
        self._have_qt_pdf = have_qt_pdf
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)

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

        # Thumbnail (cheap to render, cached).  Click → big modal.
        thumb = self._get_or_render_thumb()
        if thumb is not None:
            tlabel = _ClickableThumb(thumb)
            tlabel.clicked.connect(self._open_modal)
            tlabel.setToolTip("Click to open inline at full size")
            v.addWidget(tlabel)
        elif self._have_qt_pdf:
            err = QtWidgets.QLabel(
                "(could not render preview — use Open ↗)"
            )
            err.setObjectName("muted")
            err.setStyleSheet("font-style: italic;")
            v.addWidget(err)
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
        dlg.resize(900, 700)
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
    """``QLabel`` that emits ``clicked()`` on left-click.

    Shows the cursor as a pointing hand so the affordance is obvious.
    """

    clicked = QtCore.Signal()

    def __init__(self, pixmap: QtGui.QPixmap, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.setPixmap(pixmap)
        self.setAlignment(QtCore.Qt.AlignCenter)
        self.setCursor(QtCore.Qt.PointingHandCursor)
        # Border so the thumbnail reads as a button.
        self.setStyleSheet(
            "QLabel { border: 1px solid rgba(155,142,142,0.25);"
            "         border-radius: 3px; padding: 2px; }"
            "QLabel:hover { border-color: #FF6B6B; }"
        )

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

    def __init__(
        self,
        root_path: Path,
        hist_path: str,
        classname: str,
        FigureCanvas,
        Figure,
        *,
        dark: bool = False,
    ) -> None:
        super().__init__()
        self._root_path = root_path
        self._hist_path = hist_path
        self._classname = classname
        self._Figure = Figure
        self._FigureCanvas = FigureCanvas
        self._dark = dark
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
        """Pop the interactive plot dialog for this histogram."""
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
        self.resize(1000, 720)

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
            import numpy as np
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
        # Histogram thumbnails for THIS directory.
        if self._hists:
            grid = QtWidgets.QGridLayout()
            grid.setContentsMargins(0, 0, 0, 0)
            grid.setHorizontalSpacing(8)
            grid.setVerticalSpacing(8)
            for i, (hpath, classname) in enumerate(self._hists):
                tile = _ThumbnailTile(
                    self._root_path, hpath, classname,
                    self._FigureCanvas, self._Figure,
                    dark=self._dark,
                )
                r, c = divmod(i, _GRID_COLS)
                grid.addWidget(tile, r, c)
            grid_holder = QtWidgets.QWidget()
            grid_holder.setLayout(grid)
            self._body_layout.addWidget(grid_holder)

        # Nested sub-directory cards.
        for subname, subnode in self._subdirs.items():
            self._body_layout.addWidget(_CollapsibleDirCard(
                self._root_path, subname,
                subnode["hists"], subnode["subdirs"],
                self._dark, self._FigureCanvas, self._Figure,
                depth=self._depth + 1,
            ))


__all__ = ["QaView"]
