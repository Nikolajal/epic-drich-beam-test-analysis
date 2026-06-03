"""GUI entry point — top-bar tabbed shell.

Run::

    ./scripts/qa_quicklook              # bootstrap (.venv) + launch
    python -m qa_quicklook.app          # if .venv already provisioned

Top-level navigation (the "top bar menu"):

  - **Run Info** — three sub-tabs: *Run Manager* (always the landing
    tab), *Database*, *Runlists*.  Everything to do with the run
    lifecycle lives here.
  - **QA** — per-step sub-tabs: *Lightdata*, *Recodata*, *Recotrack*,
    *Macros*.  Each step's sub-tab renders the curated thumbnails
    from that pipeline stage's canonical ``.root`` file.
  - **Settings** — live edit of every ``conf/*.toml`` plus the
    dashboard's own ``qa_quicklook.toml``.

An optional **Advanced QA** tab sits between QA and Settings;
visibility is driven by ``[ui] show_advanced_qa`` in
``qa_quicklook.toml``, picked up live without restarting.
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path
from typing import Optional

from PySide6 import QtCore, QtGui, QtWidgets

from . import qa_pipeline as _qa_pipeline
from . import rundb
from . import theme
from .dbworker import DbWorker
from . import sheets_sync as _sheets_sync
from .qa import QaView
from .runlist import RunlistView
from .runlists import RunlistsView
from .runmanager import RunManagerView
from .settings import SettingsView


# Top-level tab order (visual left-to-right).  "Advanced QA" is the
# conditional one and slots between "QA" and "Settings" when enabled.
_TAB_ORDER = ("run_info", "qa", "trends", "advanced_qa", "settings")


# ---------------------------------------------------------------------------
# Sub-tab container: "Run Info" wraps Run Manager / Database / Runlists.
# ---------------------------------------------------------------------------


class _RunInfoTabs(QtWidgets.QWidget):
    """Run Info top-level tab — nested sub-tabs for the run lifecycle.

    Sub-tabs (in this order, Run Manager is always the landing):
      - Run Manager  — launch writers, watch progress, set quality.
      - Database     — browse / edit the run database.
      - Runlists     — named selections drawn from the database.
    """

    def __init__(
        self,
        run_manager: QtWidgets.QWidget,
        database: QtWidgets.QWidget,
        runlists: QtWidgets.QWidget,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self._tabs = QtWidgets.QTabWidget()
        self._tabs.setTabPosition(QtWidgets.QTabWidget.North)
        self._tabs.setDocumentMode(True)
        # Sub-tab bar is content-sized — these are nested actions
        # within "Run Info", not the main app-level navigation.
        self._tabs.tabBar().setExpanding(False)
        self._tabs.addTab(run_manager, "Run Manager")
        self._tabs.addTab(database, "Database")
        self._tabs.addTab(runlists, "Runlists")
        layout.addWidget(self._tabs)
        # Landing sub-tab: Run Manager.
        self._tabs.setCurrentIndex(0)


# ---------------------------------------------------------------------------
# Placeholder tab — shared by the tabs we haven't implemented yet.
# ---------------------------------------------------------------------------


class _PlaceholderTab(QtWidgets.QWidget):
    """One-card "coming next" tab.

    The body explains what the tab will do.  Keeping the actual
    description in the constructor (rather than a docstring) so the
    operator reading the panel sees the same text we agreed on in
    the design discussion — no risk of code/spec drift.
    """

    def __init__(
        self,
        title: str,
        tagline: str,
        bullets: list[str],
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(40, 40, 40, 40)
        outer.setAlignment(QtCore.Qt.AlignTop)

        card = QtWidgets.QFrame()
        card.setObjectName("cardSurface")
        card_layout = QtWidgets.QVBoxLayout(card)
        card_layout.setContentsMargins(20, 18, 20, 20)
        card_layout.setSpacing(10)

        title_label = QtWidgets.QLabel(title)
        title_label.setObjectName("sectionTitle")
        title_font = title_label.font()
        title_font.setPointSize(title_font.pointSize() + 4)
        title_font.setBold(True)
        title_label.setFont(title_font)
        card_layout.addWidget(title_label)

        tagline_label = QtWidgets.QLabel(tagline)
        tagline_label.setObjectName("muted")
        tagline_label.setWordWrap(True)
        card_layout.addWidget(tagline_label)

        if bullets:
            # No inline `color:` here — the rich-text label inherits
            # the QLabel foreground from the theme stylesheet.
            bullets_html = "<ul style='margin: 8px 0;'>" + "".join(
                f"<li style='margin: 4px 0;'>{b}</li>" for b in bullets
            ) + "</ul>"
            bullets_label = QtWidgets.QLabel(bullets_html)
            bullets_label.setTextFormat(QtCore.Qt.RichText)
            bullets_label.setWordWrap(True)
            card_layout.addWidget(bullets_label)

        status = QtWidgets.QLabel("Status: <i>placeholder — implementation pending</i>")
        status.setObjectName("muted")
        card_layout.addWidget(status)

        outer.addWidget(card)
        outer.addStretch(1)


# ---------------------------------------------------------------------------
# Status-bar widget: live qa_pipeline progress (3 colored segments
# driven by the writer's --json events).
# ---------------------------------------------------------------------------


class _QaPipelineProgressBar(QtWidgets.QWidget):
    """Three-segment progress strip for the live qa_pipeline run.

    One coloured 36×12 chip per stage in execution order; a small
    trailing label captions the most recent transition.  Driven by
    :func:`qa_pipeline.parse_progress_line` events read from the
    spawned writer's stdout — colours map per state:

      * ``pending``       grey  — stage hasn't started yet
      * ``started``       blue  — stage is running
      * ``ok``            green — stage finished successfully
      * ``skipped``       grey  — output already existed, no work
      * ``not_run``       grey  — excluded by ``--stages``
      * ``failed``        red   — exited non-zero, pipeline stopped

    Mirrors the lifecycle of ``_livemon_status_label``: hidden until a
    qa run kicks off, re-hidden 5 s after the writer exits so the
    final outcome stays readable.
    """

    _COLORS = {
        "pending": "#4a5054",
        "started": "#5fa8d3",
        "ok":      "#62b58f",
        "skipped": "#4a5054",
        "not_run": "#4a5054",
        "failed":  "#d4565c",
    }

    def __init__(
        self,
        stages: tuple[str, ...],
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)
        self._stages = stages
        self._segments: dict[str, QtWidgets.QFrame] = {}
        for name in stages:
            seg = QtWidgets.QFrame(self)
            seg.setFixedSize(36, 12)
            self._paint(seg, "pending")
            seg.setToolTip(f"{name}: pending")
            layout.addWidget(seg)
            self._segments[name] = seg
        self._label = QtWidgets.QLabel("", self)
        self._label.setStyleSheet(
            "QLabel { color: #aaa; padding-left: 6px; }"
        )
        layout.addWidget(self._label)

    @classmethod
    def _paint(cls, seg: QtWidgets.QFrame, state: str) -> None:
        color = cls._COLORS.get(state, cls._COLORS["pending"])
        seg.setStyleSheet(
            f"QFrame {{ background: {color}; border-radius: 2px; }}"
        )

    def reset(self, run_id: str) -> None:
        """Reset all segments to pending — called when a fresh run begins."""
        for name, seg in self._segments.items():
            self._paint(seg, "pending")
            seg.setToolTip(f"{name}: pending")
        self._label.setText(f"qa_pipeline {run_id}…")

    def apply_event(self, ev: dict) -> None:
        """Apply one parsed ``{stage,state,…}`` event to the strip.

        Unknown stages and unknown states fall through silently — we'd
        rather degrade gracefully than crash the dashboard on a
        writer-side protocol bump.
        """
        stage = ev.get("stage")
        state = ev.get("state")
        if stage not in self._segments:
            return
        seg = self._segments[stage]
        self._paint(seg, state if state in self._COLORS else "pending")
        # Tooltip carries the timing + new-PDF count the operator
        # would otherwise have to dig out of the log.
        if state == "started":
            seg.setToolTip(f"{stage}: running")
            self._label.setText(f"qa_pipeline: {stage}…")
            return
        tip = f"{stage}: {state}"
        wall = ev.get("wall_s")
        if isinstance(wall, (int, float)):
            tip += f" ({wall:.1f}s)"
        new_pdfs = ev.get("new_pdfs") or []
        if new_pdfs:
            tip += f", {len(new_pdfs)} new PDF(s)"
        seg.setToolTip(tip)
        self._label.setText(f"qa_pipeline: {stage} {state}")


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, repo_root: Path) -> None:
        super().__init__()
        self._repo_root = repo_root.resolve()
        self._dashboard_config = self._repo_root / "qa_quicklook" / "qa_quicklook.toml"

        self.setWindowTitle("ePIC dRICH — Quick QA")
        self.resize(1280, 820)

        self._tabs = QtWidgets.QTabWidget()
        # North = QTabWidget's default; restating for clarity (it's the
        # "top bar menu" from the design plan).
        self._tabs.setTabPosition(QtWidgets.QTabWidget.North)
        self._tabs.setDocumentMode(True)
        # Equal-width tabs across the full window — the "top bar"
        # reads as a menu, not a row of labels of varying length.
        self._tabs.tabBar().setExpanding(True)
        self._tabs.tabBar().setUsesScrollButtons(False)
        self._tabs.tabBar().setDocumentMode(True)
        self.setCentralWidget(self._tabs)

        # Tab widgets, keyed for ordered insertion + re-show.
        self._tab_widgets: dict[str, QtWidgets.QWidget] = {}
        self._tab_titles: dict[str, str] = {}
        self._build_tab_widgets()
        self._rebuild_tab_bar()
        self._build_statusbar()
        self._build_shortcuts()

        # Watch the dashboard config so a Settings-side toggle of
        # ``[ui] show_advanced_qa`` adds/removes the Advanced QA tab
        # without a restart — same hook also re-applies the theme.
        self._config_watcher = QtCore.QFileSystemWatcher(self)
        if self._dashboard_config.is_file():
            self._config_watcher.addPath(self._dashboard_config.as_posix())
        self._config_watcher.fileChanged.connect(self._on_dashboard_config_changed)

        # React to OS dark-mode flips when theme is set to "system".
        QtGui.QGuiApplication.styleHints().colorSchemeChanged.connect(
            lambda _scheme: self.apply_current_theme()
        )

        # Prewarm matplotlib so the first QA-tab visit doesn't pay
        # the ~1 s cold-import cost — import + backend init + a tiny
        # off-screen Figure that populates the font cache.  Runs in
        # a daemon thread so the window paints first.
        self._prewarm_qa_backends()

        # Cross-shifter sync — spin up the Sheets push loop iff
        # ``[sheets_sync] enabled = true`` in the dashboard config.
        # The thread + worker survive across config reloads (a
        # disabled-→-enabled flip is handled by ``reload_config``
        # without rebuilding the thread).
        self._sheets_thread: QtCore.QThread | None = None
        self._sheets_worker = None
        self._sheets_status_label: QtWidgets.QLabel | None = None
        self._build_sheets_sync()

        # Disk-retention sweep on startup (gated by [retention]
        # sweep_on_startup, default True).  Two-tier policy: full-keep
        # for the last N runs, QA-only for the next M, fully pruned
        # beyond.  Detail in qa_quicklook/retention.py.  Anything pruned
        # is re-downloadable from the DAQ host — the operator-facing
        # log message says exactly that.
        self._retention_sweep_on_startup()

        # Live monitor — off by default; spun up only when
        # [livemon] enabled = true.  Polls the DAQ host for new runs
        # and triggers auto-QA on the previous (now-sealed) run.  See
        # qa_quicklook/remote_watcher.py for the state machine.
        self._livemon_thread: QtCore.QThread | None = None
        self._livemon_worker = None
        self._build_remote_watcher()

        # Snapshot of the config facets that drive the reactions in
        # ``_on_dashboard_config_changed``.  Everything above was just
        # built from the current file, so this reflects the applied
        # state — the change handler diffs against it and only rebuilds
        # the facet that actually moved.  Without this, every Settings
        # edit (even an unrelated key) re-applied the theme, rebuilt the
        # tab bar, and tore down + restarted the live-monitor / Sheets
        # worker threads, the last blocking the UI thread on
        # ``QThread.wait`` — which is what froze the dashboard on edit.
        self._config_facets = self._read_config_facets()

    def _prewarm_qa_backends(self) -> None:
        import threading

        def _worker():
            try:
                import matplotlib
                matplotlib.use("QtAgg", force=False)
                # The expensive bits are the import itself + the
                # font-cache build that fires on the first text()
                # render.  One Figure with a tiny text+axes does it.
                from matplotlib.figure import Figure  # noqa: F401
                from matplotlib.backends.backend_qtagg import (  # noqa: F401
                    FigureCanvas,
                )
                fig = Figure(figsize=(1, 1), dpi=50)
                ax = fig.add_subplot(111)
                ax.text(0.5, 0.5, "prewarm", ha="center")
                fig.canvas.draw_idle() if hasattr(fig, "canvas") else None
            except Exception:  # noqa: BLE001
                pass

        threading.Thread(target=_worker, name="qaq-mpl-prewarm",
                         daemon=True).start()

    # ----- tab construction ---------------------------------------------

    def _build_tab_widgets(self) -> None:
        # Single shared rundb-writes worker so the Run Manager's
        # quick-quality dropdown and the Database tab's runcard edits
        # serialise into one FIFO queue — no races on the read-modify-
        # write of the same .toml file from two views at once.
        self._db_worker = DbWorker(self)

        # Settings — live.  Exposes conf/*.toml + the dashboard config.
        conf_dir = self._repo_root / "conf"
        #  calibration_conf.toml lives in conf/calib/ (no defaults/sets/working
        #  layout) so it isn't picked up by the standard ``conf/*.toml`` scan.
        #  Pass it through as an extra file so Settings still exposes it under
        #  a "Calibration" section — operators edit it in place, same flow as
        #  qa_quicklook.toml.
        calib_conf = self._repo_root / "conf" / "calib" / "calibration_conf.toml"
        extras = [self._dashboard_config]
        if calib_conf.is_file():
            extras.append(calib_conf)
        settings = SettingsView(conf_dir, extra_files=extras)

        # Default campaign = the NEWEST run-lists/YYYY.{database,runlists}.toml
        # present, so a new year's files become the default automatically
        # (no hard-coded year to bump, no stale default pointing at last
        # campaign).  The operator can still switch in the Database /
        # Runlists selectors.
        run_lists_dir = self._repo_root / "run-lists"
        default_db = rundb.newest_campaign_file(run_lists_dir, "database")
        default_runlists = rundb.newest_campaign_file(run_lists_dir, "runlists")

        # Run Manager — real interface for launching writers.
        run_manager = RunManagerView(
            repo_root=self._repo_root,
            data_dir=self._repo_root / "Data",
            database_path=default_db,
            db_worker=self._db_worker,
        )
        qa = QaView(
            database_path=default_db,
            data_dir=self._repo_root / "Data",
        )
        advanced_qa = _PlaceholderTab(
            title="Advanced QA",
            tagline=(
                "Plots and values pulled from bespoke ROOT macros.  "
                "Hidden by default — flip <code>[ui] show_advanced_qa</code> "
                "in <code>qa_quicklook.toml</code> (Settings tab) to enable."
            ),
            bullets=[],   # intentionally empty; spec to follow
        )
        runlist = RunlistView(
            database_path=default_db,
            data_dir=self._repo_root / "Data",
            runlists_path=default_runlists,
            db_worker=self._db_worker,
        )
        runlists = RunlistsView(
            runlists_path=default_runlists,
            db_worker=self._db_worker,
        )

        # Wrap Run Manager / Database / Runlists into a single
        # "Run Info" top-level tab with nested sub-tabs.  Landing
        # is always Run Manager (operator's main workspace).
        run_info = _RunInfoTabs(
            run_manager=run_manager,
            database=runlist,
            runlists=runlists,
        )

        #  Multi-run scatter — a first-level QA quantity vs a beam-info
        #  condition across a runlist (e.g. N_γ vs V_bias, coloured by
        #  mirror position).
        from .scatter_view import MultiRunScatterView
        trends = MultiRunScatterView(
            data_dir=self._repo_root / "Data",
            database_path=default_db,
            runlists_path=default_runlists,
        )

        self._tab_widgets = {
            "run_info": run_info,
            "qa": qa,
            "trends": trends,
            "advanced_qa": advanced_qa,
            "settings": settings,
        }
        self._tab_titles = {
            "run_info": "Run Info",
            "qa": "QA",
            "trends": "Multi-run",
            "advanced_qa": "Advanced QA",
            "settings": "Settings",
        }

        # ── Cross-tab run sync ───────────────────────────────────────
        #  Keep the same run selected in the Run Manager and the QA tab,
        #  so acting on run X in one and switching to the other lands on
        #  X.  Each view broadcasts genuine picker changes via
        #  ``run_selected``; we cache the last one and push it onto the
        #  other view (the views guard against re-broadcast, so no
        #  ping-pong).  A run present in one picker but not the other
        #  (e.g. a QA-only run absent from the Run Manager's on-disk
        #  list) is a silent no-op on the side that lacks it.
        self._run_manager_view = run_manager
        self._qa_view = qa
        self._current_run: Optional[str] = None
        run_manager.run_selected.connect(self._on_run_selected_global)
        qa.run_selected.connect(self._on_run_selected_global)
        self._tabs.currentChanged.connect(self._sync_run_into_current_tab)

    def _rebuild_tab_bar(self) -> None:
        """Insert tabs in ``_TAB_ORDER``, honouring the Advanced QA toggle.

        Called once at construction and whenever the dashboard config
        changes.  We preserve the currently visible tab by key (not by
        index) so toggling Advanced QA on doesn't bounce the user to a
        different page.
        """
        current_key = None
        current_widget = self._tabs.currentWidget()
        for k, w in self._tab_widgets.items():
            if w is current_widget:
                current_key = k
                break

        show_advanced = self._read_show_advanced_qa()

        # Detach all tabs without destroying the widgets.
        while self._tabs.count():
            self._tabs.removeTab(0)

        # Re-add in canonical order, skipping advanced_qa when hidden.
        for key in _TAB_ORDER:
            if key == "advanced_qa" and not show_advanced:
                continue
            self._tabs.addTab(self._tab_widgets[key], self._tab_titles[key])

        # Restore selection.
        if current_key and current_key in self._tab_widgets:
            target_widget = self._tab_widgets[current_key]
            for i in range(self._tabs.count()):
                if self._tabs.widget(i) is target_widget:
                    self._tabs.setCurrentIndex(i)
                    break

    def _on_run_selected_global(self, run_id: str) -> None:
        """Cache the run an operator picked and mirror it onto the
        other run-aware view immediately (so a later tab switch is
        instant, and a side-by-side layout stays consistent)."""
        if not run_id or run_id == self._current_run:
            return
        self._current_run = run_id
        self._run_manager_view.set_selected_run(run_id)
        self._qa_view.set_selected_run(run_id)

    def _sync_run_into_current_tab(self, _idx: int) -> None:
        """On a top-level tab switch, push the cached run onto whichever
        run-aware view just became visible — so 'I was on run X, now I'm
        on the QA tab' lands on X."""
        if not self._current_run:
            return
        current = self._tabs.currentWidget()
        if current is self._qa_view:
            self._qa_view.set_selected_run(self._current_run)
        elif current is self._tab_widgets.get("run_info"):
            self._run_manager_view.set_selected_run(self._current_run)

    def _read_ui_config(self) -> dict:
        """Parse the ``[ui]`` table from ``qa_quicklook.toml`` once."""
        if not self._dashboard_config.is_file():
            return {}
        try:
            if sys.version_info >= (3, 11):
                import tomllib
            else:  # pragma: no cover
                import tomli as tomllib  # type: ignore
            with self._dashboard_config.open("rb") as fh:
                data = tomllib.load(fh)
        except Exception:  # noqa: BLE001
            return {}
        ui = data.get("ui", {})
        return ui if isinstance(ui, dict) else {}

    def _read_show_advanced_qa(self) -> bool:
        return bool(self._read_ui_config().get("show_advanced_qa", False))

    def _read_theme_mode(self) -> theme.Mode:
        raw = str(self._read_ui_config().get("theme", "system")).lower()
        try:
            return theme.Mode(raw)
        except ValueError:
            return theme.Mode.SYSTEM

    def apply_current_theme(self) -> None:
        """Re-read [ui].theme and re-apply the stylesheet to the running app.

        Called at construction, on dashboard-config change, and when
        the system colour scheme flips (only meaningful for
        ``theme = "system"``).

        Also pushes ``[ui].plots_theme`` down to the QA module so the
        matplotlib figures track the operator's pin (``"light"`` /
        ``"dark"``) or fall back to following the UI palette
        (``"follow"`` — the default).
        """
        app = QtWidgets.QApplication.instance()
        if app is None:
            return
        theme.apply(app, self._read_theme_mode())

        #  Plot theme override.  Import lazily — qa.py pulls in
        #  matplotlib via the prewarm path and we don't want to
        #  block app construction on a slow first import.
        try:
            from qa_quicklook import qa as _qa
        except Exception:  # noqa: BLE001
            return
        raw = str(self._read_ui_config().get("plots_theme", "follow")).lower()
        _qa.set_plots_theme_override(raw if raw in ("light", "dark") else None)

    def _on_dashboard_config_changed_livemon(self) -> None:
        """Bridge: forward the config-reload event into the live monitor.

        Operator toggling ``[livemon] enabled`` in Settings now takes
        effect without a restart — same UX as the Sheets-sync hot
        reload.  Calling ``_build_remote_watcher`` while the worker
        is already running stops + restarts it with the new settings.
        """
        try:
            self._build_remote_watcher()
        except Exception:  # noqa: BLE001
            pass

    def _read_config_facets(self) -> dict:
        """Snapshot the dashboard-config slices the change handler reacts to.

        Settings writes ``qa_quicklook.toml`` after every edit (a 500 ms
        debounce), so ``_on_dashboard_config_changed`` fires constantly.
        Each reaction it can run is expensive — re-applying the global
        stylesheet, rebuilding the tab bar, and (the costly one) tearing
        down + restarting the live-monitor / Sheets worker threads, which
        blocks the UI thread on ``QThread.wait`` for up to 2 s while an
        in-flight SSH poll drains.  Diffing these facets lets the handler
        skip the reaction whose config section didn't actually move, so
        editing an unrelated key (a retention count, an rsync path) no
        longer freezes the UI.
        """
        ui = self._read_ui_config()
        return {
            # Drives _rebuild_tab_bar (Advanced QA tab visibility).
            "show_advanced": bool(ui.get("show_advanced_qa", False)),
            # Drives apply_current_theme (UI palette + plot theme).
            "theme": (
                str(ui.get("theme", "system")).lower(),
                str(ui.get("plots_theme", "follow")).lower(),
            ),
            # Drives the Sheets-sync worker rebuild/reload.
            "sheets": self._read_dashboard_section("sheets_sync"),
            # Drives the live-monitor worker rebuild (the blocking one).
            "livemon": self._read_dashboard_section("livemon"),
        }

    def _read_dashboard_section(self, name: str) -> dict:
        """Return one top-level table from the dashboard config as a dict."""
        if not self._dashboard_config.is_file():
            return {}
        try:
            if sys.version_info >= (3, 11):
                import tomllib
            else:  # pragma: no cover
                import tomli as tomllib  # type: ignore
            with self._dashboard_config.open("rb") as fh:
                data = tomllib.load(fh)
        except Exception:  # noqa: BLE001
            return {}
        section = data.get(name, {})
        return section if isinstance(section, dict) else {}

    def _on_dashboard_config_changed(self, qpath: str) -> None:
        # QFileSystemWatcher drops the path on editor-renames; re-add.
        if qpath not in self._config_watcher.files():
            self._config_watcher.addPath(qpath)

        # Diff the new config against the applied snapshot and only run
        # the reaction whose section changed.  A blanket rebuild-all here
        # froze the UI on every Settings edit because rebuilding the
        # live-monitor / Sheets threads blocks on ``QThread.wait``.
        new = self._read_config_facets()
        old = getattr(self, "_config_facets", {})
        self._config_facets = new
        if not old:
            # Snapshot missing (shouldn't happen post-construction) —
            # fall back to the old behaviour so nothing silently stalls.
            old = {}

        if new.get("show_advanced") != old.get("show_advanced"):
            self._rebuild_tab_bar()
        if new.get("theme") != old.get("theme"):
            self.apply_current_theme()
        if new.get("sheets") != old.get("sheets"):
            # If we never spun a worker up (disabled at construction) and
            # it's now enabled, build one; otherwise let it re-read.
            if self._sheets_worker is not None:
                QtCore.QMetaObject.invokeMethod(
                    self._sheets_worker, "reload_config",
                    QtCore.Qt.QueuedConnection,
                )
            else:
                self._build_sheets_sync()
        if new.get("livemon") != old.get("livemon"):
            # _build_remote_watcher is idempotent: stops the existing
            # thread if any, starts a new one if enabled is now true.
            self._on_dashboard_config_changed_livemon()

    # ----- status bar / shortcuts ---------------------------------------

    def _build_statusbar(self) -> None:
        sb = self.statusBar()
        sb.showMessage(f"repo: {self._repo_root}")
        # Permanent right-anchored slot for the live-monitor state —
        # placed BEFORE the Sheets one so the leftmost permanent slot
        # surfaces the most operationally-active monitor.  Hidden until
        # ``[livemon] enabled = true`` flips on; same lifecycle as
        # ``_sheets_status_label``.
        self._livemon_status_label = QtWidgets.QLabel("", self)
        self._livemon_status_label.setObjectName("muted")
        self._livemon_status_label.setStyleSheet(
            "QLabel { color: #62b58f; }"  # green = healthy/idle
        )
        self._livemon_status_label.setVisible(False)
        sb.addPermanentWidget(self._livemon_status_label)
        # Permanent right-anchored slot for the live qa_pipeline
        # progress strip — hidden until a livemon-driven QA run kicks
        # off, re-hidden 5 s after it exits.  Sits between the livemon
        # label and Sheets one so the operationally-active monitors
        # cluster on the left of the permanent-right region.
        self._qa_progress_bar = _QaPipelineProgressBar(
            _qa_pipeline.STAGE_NAMES, self,
        )
        self._qa_progress_bar.setVisible(False)
        sb.addPermanentWidget(self._qa_progress_bar)
        # Buffer for line-aligning stdout chunks delivered by the
        # QProcess running qa_pipeline --json.  Lives on self because
        # the livemon chain is sequential (one qa run at a time).
        self._qa_stdout_buffer: str = ""
        # Permanent right-anchored slot for the Sheets-sync state.
        # Hidden until the worker actually starts emitting; the
        # label widget lives across config toggles so connecting +
        # disconnecting signals stays trivial.
        self._sheets_status_label = QtWidgets.QLabel("", self)
        self._sheets_status_label.setObjectName("muted")
        self._sheets_status_label.setVisible(False)
        sb.addPermanentWidget(self._sheets_status_label)

    # ----- disk retention (two-tier sweep) ------------------------------

    def _retention_settings(self) -> tuple[bool, int, int, bool, bool]:
        """Read the ``[retention]`` section of the dashboard config.

        Returns ``(sweep_on_startup, full_keep_n, qa_keep_n,
        keep_recotrackdata, has_section)``.  Defaults match
        ``qa_quicklook.toml`` so a freshly-installed copy works without
        the operator touching anything.
        """
        try:
            import tomllib  # py >= 3.11
        except ImportError:  # pragma: no cover — py3.10
            import tomli as tomllib  # type: ignore[no-redef]
        try:
            with self._dashboard_config.open("rb") as fh:
                doc = tomllib.load(fh)
        except (OSError, Exception):  # noqa: BLE001
            return (True, 5, 50, True, False)
        section = doc.get("retention") or {}
        return (
            bool(section.get("sweep_on_startup", True)),
            int(section.get("full_keep_n", 5)),
            int(section.get("qa_keep_n", 50)),
            bool(section.get("keep_recotrackdata", True)),
            bool(section),
        )

    def _data_dir_for_retention(self) -> Path | None:
        """Resolve the data-dir the sweep operates on, honouring
        ``[rsync] local_data_dir``.  Returns None if the path doesn't
        exist (e.g. fresh checkout before any download).
        """
        try:
            import tomllib  # py >= 3.11
        except ImportError:  # pragma: no cover
            import tomli as tomllib  # type: ignore[no-redef]
        try:
            with self._dashboard_config.open("rb") as fh:
                doc = tomllib.load(fh)
        except (OSError, Exception):  # noqa: BLE001
            return None
        rel = doc.get("rsync", {}).get("local_data_dir", "Data")
        cand = (self._repo_root / rel).resolve()
        return cand if cand.is_dir() else None

    def _qa_data_repo(self) -> Path:
        """Resolve the data repository the QA pipeline should target.

        Honours ``[rsync].local_data_dir`` so an operator who points the
        dashboard at a non-default data tree (e.g. a scratch dir on the
        beam-test box) gets QA run against THAT tree, not a hardcoded
        ``<repo>/Data``.  This was the single most likely first-15-min
        failure on the operator box: livemon / manual-QA silently looked
        in the wrong directory and reported EXIT_RUN_MISSING.

        Falls back to ``<repo>/Data`` when the config is unreadable or
        the configured dir doesn't exist yet (fresh checkout before any
        download).  Always returns an absolute Path.
        """
        from . import download
        try:
            cfg = download.load_config(self._dashboard_config)
            cand = (self._repo_root / cfg.local_data_dir).resolve()
            #  Use the configured dir even if it doesn't exist yet — the
            #  pipeline's own EXIT_RUN_MISSING is a clearer signal than
            #  silently falling back to a different tree.
            return cand
        except Exception:  # noqa: BLE001
            return (self._repo_root / "Data").resolve()

    def _qa_python(self) -> str:
        """Resolve the Python interpreter to launch qa_pipeline with.

        Prefers ``sys.executable`` — whatever interpreter is running the
        dashboard is the one with PySide6 + the qa_quicklook package
        importable, by definition.  Falls back to a PATH-resolved
        ``python3``, then to the repo's ``.venv`` only if it exists.

        The old code hardcoded ``<repo>/.venv/bin/python`` which broke
        for any operator who installed via conda or system pip — the
        auto-QA subprocess never started.
        """
        if sys.executable:
            return sys.executable
        which = shutil.which("python3")
        if which:
            return which
        venv = self._repo_root / ".venv" / "bin" / "python"
        return str(venv)

    def _run_retention_sweep(
        self, *, reason: str, dry_run: bool = False,
    ) -> None:
        """Run the retention sweep + apply, log the summary.

        ``reason`` is a short tag ("startup", "pre-download") surfaced
        in the log so operators can audit when each sweep fired.
        Runs with an active joblock on them are filtered out of the
        plan before ``apply`` runs — pruning a run that's mid-write
        would corrupt the output.

        ``dry_run`` skips ``apply()`` and only logs the plan summary.
        Used by the offscreen / headless launch path to keep the
        sweep visible-but-non-destructive in CI / screenshot harnesses.
        """
        from . import retention, joblock

        sweep_on_startup, full_keep_n, qa_keep_n, keep_recotrackdata, has_section = (
            self._retention_settings()
        )
        if not has_section:
            #  Operator hasn't opted in (config still old-style) — be
            #  conservative and never delete anything.  Once they edit
            #  the file the section will appear and we'll start sweeping.
            return
        data_dir = self._data_dir_for_retention()
        if data_dir is None:
            return
        plan = retention.sweep(
            data_dir, full_keep_n, qa_keep_n, keep_recotrackdata,
        )
        #  Joblock filter: any run whose effective state is "running"
        #  for ANY known writer must NOT be touched.  Iterates each
        #  writer × each (qa_only|fully_pruned) run; drops matches.
        live_runs: set[str] = set()
        for writer in ("lightdata_writer", "recodata_writer",
                       "recotrackdata_writer", "qa_pipeline"):
            for lock in joblock.list_locks():
                if lock.writer == writer and (
                    joblock.effective_state(lock) == joblock.EFFECTIVE_RUNNING
                ):
                    live_runs.add(lock.run)
        if live_runs:
            plan["qa_only"] = [
                e for e in plan["qa_only"]
                if Path(e["run"]).name not in live_runs
            ]
            plan["fully_pruned"] = [
                p for p in plan["fully_pruned"]
                if Path(p).name not in live_runs
            ]
        summary = retention.format_plan_summary(plan)
        self.statusBar().showMessage(
            f"retention ({reason}): {summary}  ·  any pruned run is "
            f"re-downloadable from the DAQ host",
            8000,
        )
        if plan["qa_only"] or plan["fully_pruned"]:
            if dry_run:
                self.statusBar().showMessage(
                    f"retention ({reason}): DRY-RUN — would prune "
                    f"{len(plan['qa_only'])} to QA-only + "
                    f"{len(plan['fully_pruned'])} fully",
                    8000,
                )
                return
            report = retention.apply(plan)
            mb = report["bytes_freed"] / (1024 * 1024)
            self.statusBar().showMessage(
                f"retention ({reason}): freed {mb:.1f} MB across "
                f"{report['n_deleted']} files/dirs  ·  "
                f"{len(report['errors'])} errors",
                8000,
            )

    def _retention_sweep_on_startup(self) -> None:
        #  Guard 1: explicit opt-out for headless / test launches.
        #  Setting QAQ_DISABLE_STARTUP_SWEEP=1 in the environment
        #  skips the sweep entirely.  Use this for any non-interactive
        #  MainWindow construction (pytest fixtures, smoke launches,
        #  CI screenshots).
        if os.environ.get("QAQ_DISABLE_STARTUP_SWEEP") == "1":
            return
        #  Guard 2: refuse to apply if the Qt platform is "offscreen"
        #  (typical CI / screenshot harness).  An offscreen launch
        #  must not perform destructive disk ops on production data —
        #  the operator never sees the result, so they can't react.
        #  Log the plan but skip apply.
        if os.environ.get("QT_QPA_PLATFORM") == "offscreen":
            self._run_retention_sweep(reason="startup-dryrun-offscreen", dry_run=True)
            return
        sweep_on_startup, *_ = self._retention_settings()
        if sweep_on_startup:
            self._run_retention_sweep(reason="startup")

    # ----- cross-shifter sync (Google Sheets push) ----------------------

    def _build_sheets_sync(self) -> None:
        """Build the Sheets-sync worker + its thread iff enabled.

        No-op when ``[sheets_sync] enabled = false`` so machines that
        don't care never even import the worker module.  Idempotent:
        re-calling after a disabled-→-enabled flip wires everything
        up; re-calling while already running stops the previous
        thread first.
        """
        cfg = _sheets_sync.load_config(self._dashboard_config)
        if not cfg.enabled:
            return
        # Lazy import — keeps the optional dependency surface (the
        # PySide6-side worker module) out of the cold-import path
        # for the no-sync case.
        from .sheets_worker import SheetsSyncWorker

        if self._sheets_thread is not None:
            # Hot-reload path: tear down then rebuild.  Cheap because
            # SheetsSyncWorker stops the timer on ``stop()``.
            try:
                QtCore.QMetaObject.invokeMethod(
                    self._sheets_worker, "stop",
                    QtCore.Qt.BlockingQueuedConnection,
                )
            except Exception:  # noqa: BLE001
                pass
            self._sheets_thread.quit()
            self._sheets_thread.wait(2000)
            self._sheets_thread = None
            self._sheets_worker = None

        self._sheets_thread = QtCore.QThread(self)
        self._sheets_thread.setObjectName("qaq-sheets-sync")
        worker = SheetsSyncWorker(
            cfg, self._repo_root, self._dashboard_config,
        )
        worker.moveToThread(self._sheets_thread)
        worker.state_text.connect(self._on_sheets_state_text)
        worker.pushed.connect(self._on_sheets_pushed)
        worker.error.connect(self._on_sheets_error)
        self._sheets_thread.started.connect(worker.start)
        self._sheets_thread.finished.connect(worker.deleteLater)
        self._sheets_worker = worker
        self._sheets_thread.start()

    def _on_sheets_state_text(self, text: str) -> None:
        if self._sheets_status_label is None:
            return
        self._sheets_status_label.setText(f"Sheets: {text}")
        self._sheets_status_label.setToolTip(text)
        self._sheets_status_label.setVisible(True)

    def _on_sheets_pushed(
        self, last_push_at: str, edits_applied: int, edits_skipped: int,
    ) -> None:
        # The state_text signal already lit the label up with the
        # essentials — this slot is the place to surface anything
        # interesting in the activity log once the dashboard grows
        # one.  For v1 we keep it as a tooltip-only enrichment.
        if self._sheets_status_label is None:
            return
        tip = (
            f"Last push: {last_push_at}\n"
            f"Reverse edits applied this push: {edits_applied}\n"
            f"Skipped-in-sync (snapshot stale): {edits_skipped}"
        )
        self._sheets_status_label.setToolTip(tip)

    def _on_sheets_error(self, message: str) -> None:
        if self._sheets_status_label is None:
            return
        self._sheets_status_label.setToolTip(message)

    # ----- live-mon orchestration ---------------------------------------

    def _livemon_settings(self) -> tuple[bool, int, str]:
        """Read ``[livemon]`` section.  Returns (enabled, interval, notify_cmd)."""
        try:
            import tomllib
        except ImportError:  # pragma: no cover
            import tomli as tomllib  # type: ignore[no-redef]
        try:
            with self._dashboard_config.open("rb") as fh:
                doc = tomllib.load(fh)
        except (OSError, Exception):  # noqa: BLE001
            return (False, 20, "")
        section = doc.get("livemon") or {}
        return (
            bool(section.get("enabled", False)),
            int(section.get("poll_interval_s", 20)),
            str(section.get("notify_command", "")),
        )

    def _build_remote_watcher(self) -> None:
        """Spin up the remote watcher on its own QThread iff enabled.

        Idempotent: re-calling after a config flip stops the existing
        thread and starts a fresh one.  Off by default — the watcher
        only exists in memory when ``[livemon] enabled = true``.
        """
        enabled, interval, _notify_cmd = self._livemon_settings()
        existing = getattr(self, "_livemon_thread", None)
        if existing is not None:
            self._livemon_thread.quit()
            self._livemon_thread.wait(2000)
            self._livemon_thread = None
            self._livemon_worker = None
        if not enabled:
            return
        from . import download
        from .remote_watcher import RemoteWatcherWorker

        cfg = download.load_config(self._dashboard_config)
        self._livemon_thread = QtCore.QThread(self)
        self._livemon_thread.setObjectName("qaq-remote-watcher")
        worker = RemoteWatcherWorker(
            cfg, self._repo_root, poll_interval_s=interval,
        )
        worker.moveToThread(self._livemon_thread)
        worker.new_sealed_run.connect(self._on_livemon_sealed_run)
        worker.state_text.connect(self._on_livemon_state)
        worker.error_occurred.connect(self._on_livemon_error)
        self._livemon_thread.started.connect(worker.start)
        self._livemon_thread.finished.connect(worker.deleteLater)
        self._livemon_worker = worker
        self._livemon_thread.start()

    @QtCore.Slot(str)
    def _on_livemon_state(self, text: str) -> None:
        """Surface live-monitor state in the status bar.

        Updates both the transient main message (auto-clears in 6 s)
        AND the permanent right-anchored slot so the operator always
        sees that the monitor is alive even after the transient line
        clears.
        """
        self.statusBar().showMessage(f"livemon: {text}", 6000)
        label = getattr(self, "_livemon_status_label", None)
        if label is not None:
            label.setText(f"livemon: {text}")
            label.setToolTip(text)
            label.setVisible(True)

    @QtCore.Slot(str)
    def _on_livemon_error(self, message: str) -> None:
        """SSH listing failed — modal dialog + stop the worker.

        Per operator decision (2026-05-29): no silent retries.  The
        shifter explicitly acknowledges the failure so they're aware
        the DAQ host is unreachable.
        """
        if getattr(self, "_livemon_worker", None) is not None:
            self._livemon_worker.stop()
            self._livemon_thread.quit()
            self._livemon_thread.wait(2000)
            self._livemon_thread = None
            self._livemon_worker = None
        QtWidgets.QMessageBox.critical(
            self,
            "Live monitor stopped",
            f"The live monitor stopped because the SSH listing failed:\n\n"
            f"{message}\n\n"
            f"Once the DAQ host is reachable again, toggle "
            f"[livemon] enabled in Settings to restart.",
        )

    @QtCore.Slot(str)
    def _on_livemon_sealed_run(self, run_id: str) -> None:
        """A run sealed on the remote — kick off auto-download + qa_pipeline.

        Workflow (chained via QProcess finish signals):
            1. rsync the run dir into local Data/
            2. on success, launch qa_pipeline --QA on the run
            3. on completion, fire the AUTO notification (Hero.aiff +
               distinct title) so the shifter can tell this was the
               monitor, not their last manual click
            4. optionally fire ``livemon.notify_command`` with run_id
        """
        _enabled, _interval, notify_cmd = self._livemon_settings()
        self.statusBar().showMessage(
            f"livemon: auto-downloading sealed run {run_id}", 8000,
        )
        #  Chain: spawn the rsync via QProcess so we can connect to
        #  its finished signal and only then kick the qa_pipeline.
        from . import download
        try:
            cfg = download.load_config(self._dashboard_config)
            argv = download.build_argv(cfg, run_id, self._repo_root)
        except Exception as exc:  # noqa: BLE001
            self._livemon_notify_failure(run_id, f"download argv: {exc}")
            return
        rsync = QtCore.QProcess(self)
        rsync.setProgram(argv[0])
        rsync.setArguments(argv[1:])
        rsync.finished.connect(
            lambda code, _status, rid=run_id, cmd=notify_cmd:
            self._on_livemon_download_done(rid, code, cmd)
        )
        rsync.start()

    def _on_livemon_download_done(
        self, run_id: str, exit_code: int, notify_cmd: str,
    ) -> None:
        """Rsync finished — if OK, launch qa_pipeline."""
        if exit_code != 0:
            self._livemon_notify_failure(
                run_id, f"rsync exited {exit_code}",
            )
            return
        self.statusBar().showMessage(
            f"livemon: running qa_pipeline on {run_id}", 6000,
        )
        qa = QtCore.QProcess(self)
        #  Interpreter + data-repo resolved via helpers so the livemon
        #  path honours [rsync].local_data_dir and works under conda /
        #  system-pip installs (not just the repo .venv).  See
        #  _qa_python / _qa_data_repo.
        qa.setProgram(self._qa_python())
        qa.setArguments([
            "-m", "qa_quicklook.qa_pipeline",
            run_id, "--max-spill", "15", "--notify", "macos,file",
            "--data-repo", str(self._qa_data_repo()),
            #  Clean slate before every GUI-driven run — purge the
            #  regenerable QA artifacts (qa/ tree, writer output roots,
            #  stray run-root h_*.pdf) so the dashboard never shows a
            #  mix of fresh + stale plots.  Allowlist-protected: never
            #  touches raw device dirs or calibration files.
            "--clean",
            "--json",
        ])
        #  Reset + reveal the per-stage progress strip BEFORE start()
        #  so the first ``started`` event lands on a clean widget.
        self._qa_stdout_buffer = ""
        self._qa_progress_bar.reset(run_id)
        self._qa_progress_bar.setVisible(True)
        qa.readyReadStandardOutput.connect(
            lambda p=qa: self._on_qa_pipeline_stdout(p)
        )
        qa.finished.connect(
            lambda code, _s, rid=run_id, cmd=notify_cmd, p=qa:
            self._on_livemon_qa_done(rid, code, cmd, p)
        )
        qa.start()

    def _on_qa_pipeline_stdout(self, proc: QtCore.QProcess) -> None:
        """Drain qa_pipeline --json stdout, dispatch events to the bar.

        QProcess delivers byte slices that may split a JSON line; we
        buffer across reads via :func:`consume_progress_buffer` and
        only act on complete lines.  Non-JSON noise is dropped by the
        parser, so a writer that accidentally prints to stdout under
        --json never desyncs the strip.
        """
        chunk = bytes(proc.readAllStandardOutput()).decode(
            "utf-8", errors="replace",
        )
        events, self._qa_stdout_buffer = _qa_pipeline.consume_progress_buffer(
            self._qa_stdout_buffer + chunk,
        )
        for ev in events:
            self._qa_progress_bar.apply_event(ev)

    def _on_livemon_qa_done(
        self, run_id: str, exit_code: int, notify_cmd: str,
        proc: QtCore.QProcess | None = None,
    ) -> None:
        """qa_pipeline finished — fire the distinct auto notification."""
        #  Drain any final stdout that arrived between the last
        #  readyReadStandardOutput tick and the finished signal so the
        #  terminal-state segment colour is correct before we schedule
        #  the hide.
        if proc is not None:
            self._on_qa_pipeline_stdout(proc)
        #  Hide the progress strip 5 s after exit regardless of
        #  outcome — long enough for the operator to read the final
        #  state, short enough to keep the status bar uncluttered.
        QtCore.QTimer.singleShot(
            5000, lambda: self._qa_progress_bar.setVisible(False),
        )
        if exit_code != 0:
            self._livemon_notify_failure(
                run_id, f"qa_pipeline exited {exit_code}",
            )
            return
        self._livemon_notify_success(run_id)
        #  Operator-provided post-hook (Slack webhook etc.).  Pass the
        #  run id via argv (NOT shell interpolation) per the security
        #  note in PARAM_DESCRIPTIONS.
        if notify_cmd:
            try:
                import subprocess
                subprocess.Popen(
                    ["sh", "-c", notify_cmd, "sh", run_id],
                )
            except OSError:
                pass

    def _livemon_notify_success(self, run_id: str) -> None:
        """Auto-QA done — distinct banner + sound from the manual flow.

        Per operator decision: manual runs use Glass.aiff "QA done";
        auto-monitor catches use Hero.aiff "Auto-QA caught one".
        Different alert tone telegraphs which path produced the result.
        Linux falls back to notify-send; Windows / headless is silent
        (the status bar update still fires).

        Sound-file fallback ladder: Hero.aiff → Sosumi.aiff → Glass.aiff
        → no sound.  Older macOS installs ship without Hero; we never
        want a missing file to break the notification.
        """
        import platform
        title = "⚡ Auto-QA done"
        body = f"Live monitor caught {run_id} — PDFs are ready"
        try:
            if platform.system() == "Darwin":
                import subprocess
                subprocess.Popen([
                    "osascript", "-e",
                    f'display notification "{body}" with title "{title}"',
                ])
                #  Sound fallback ladder — first-existing wins.  Hero
                #  is the distinct "auto" tone; the fallbacks keep the
                #  notification audible on systems that don't ship it.
                for snd in (
                    "/System/Library/Sounds/Hero.aiff",
                    "/System/Library/Sounds/Sosumi.aiff",
                    "/System/Library/Sounds/Glass.aiff",
                ):
                    if Path(snd).is_file():
                        subprocess.Popen(["afplay", snd])
                        break
            elif platform.system() == "Linux":
                import subprocess
                subprocess.Popen(
                    ["notify-send", "-u", "normal", title, body],
                )
            # Windows / unknown: silent — status bar already updated.
        except OSError:
            pass
        self.statusBar().showMessage(
            f"livemon: ✓ {run_id} — auto-QA complete", 12000,
        )

    def _livemon_notify_failure(self, run_id: str, reason: str) -> None:
        self.statusBar().showMessage(
            f"livemon: ✗ {run_id} — {reason}", 12000,
        )

    def _build_shortcuts(self) -> None:
        # Cmd-1..9 / Ctrl-1..9 jump to a tab by *current* position.  We
        # bind nine actions once; they're cheap and adapt automatically
        # when the Advanced QA tab toggles in or out.
        for i in range(9):
            act = QtGui.QAction(self)
            act.setShortcut(QtGui.QKeySequence(f"Ctrl+{i + 1}"))
            act.triggered.connect(lambda _checked=False, idx=i: self._jump_to_tab(idx))
            self.addAction(act)

    def _jump_to_tab(self, idx: int) -> None:
        if 0 <= idx < self._tabs.count():
            self._tabs.setCurrentIndex(idx)

    # -----------------------------------------------------------------------
    # Manual QA pipeline launcher — same QProcess machinery as the livemon
    # auto-trigger path, but invoked from the Run Manager button.  No rsync
    # prelude, no failure-notifier (the operator is in front of the
    # dashboard already), but full status-bar progress strip.
    # -----------------------------------------------------------------------

    def _launch_qa_pipeline_manual(self, run_id: str) -> None:
        """Run the lightdata→recodata→recotrackdata cascade on ``run_id``.

        Wires the same QProcess + stdout-parser + progress-strip pipe the
        livemon flow uses (see ``_on_livemon_download_done``).  Diverges
        in two places:

        - No rsync prelude — the operator points at a local run that's
          already on disk.
        - No livemon notify-failure (the operator is at the dashboard
          and will see the status-bar message + the per-stage strip
          turn red themselves).
        """
        qa = QtCore.QProcess(self)
        #  Interpreter + data-repo via the shared resolvers (honour
        #  [rsync].local_data_dir + non-.venv installs).
        qa.setProgram(self._qa_python())
        qa.setArguments([
            "-m", "qa_quicklook.qa_pipeline",
            run_id,
            "--max-spill", "15",
            "--notify", "macos,file",
            "--data-repo", str(self._qa_data_repo()),
            #  Clean slate before every GUI-driven run (see the livemon
            #  launch site for the rationale).  Allowlist-protected.
            "--clean",
            "--json",
        ])
        #  Reset + reveal the per-stage progress strip BEFORE start()
        #  so the first ``started`` event lands on a clean widget.
        self._qa_stdout_buffer = ""
        self._qa_progress_bar.reset(run_id)
        self._qa_progress_bar.setVisible(True)
        qa.readyReadStandardOutput.connect(
            lambda p=qa: self._on_qa_pipeline_stdout(p)
        )
        qa.finished.connect(
            lambda code, _s, rid=run_id, p=qa:
            self._on_manual_qa_done(rid, code, p)
        )
        self.statusBar().showMessage(
            f"qa_pipeline: launched on {run_id}", 4000,
        )
        qa.start()

    def _on_manual_qa_done(
        self, run_id: str, exit_code: int, proc: QtCore.QProcess,
    ) -> None:
        """Manual qa_pipeline finished — surface result, no notify-failure path."""
        if exit_code == 0:
            self.statusBar().showMessage(
                f"qa_pipeline: ✓ {run_id}", 8000,
            )
        else:
            self.statusBar().showMessage(
                f"qa_pipeline: ✗ {run_id} (exit {exit_code})", 8000,
            )
        #  Schedule deferred deletion so signal handlers complete first.
        proc.deleteLater()

    # -----------------------------------------------------------------------
    # Shutdown — stop background workers cleanly so Qt doesn't abort the
    # process with `QThread: Destroyed while thread '<name>' is still running`
    # and macOS doesn't show the "Python quit unexpectedly" dialog.
    # -----------------------------------------------------------------------

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # noqa: N802 (Qt API)
        """Stop the sheets-sync + remote-watcher workers before close.

        Both workers live on their own QThreads.  Without an explicit
        stop+wait here, MainWindow's C++ destructor releases the thread
        objects while their event loops are still running — Qt 6 aborts
        the process with a `QThread: Destroyed while thread …` warning,
        which on macOS surfaces as a "Python quit unexpectedly" dialog
        on every close.

        Hot-reload paths (``_build_sheets_worker``, ``_build_remote_watcher``)
        already implement the same teardown shape; this method just
        applies it to both threads on the close path.
        """
        #  Sheets-sync: invokes the worker's ``stop`` slot synchronously
        #  to stop its internal QTimer, then quits + waits on the thread.
        #  Wrapped in try/except because invokeMethod throws if the
        #  worker has already been deleteLater'd or the connection is
        #  cross-thread-broken at shutdown.
        #  Close is not a crucial path — don't block the operator on it.
        #  Use a NON-blocking stop (queued, so we don't wait behind an
        #  in-flight Sheets network op) and a brief wait budget: idle,
        #  QTimer-driven workers drain their event loop in well under this,
        #  so close is effectively instant; the short cap just avoids hanging
        #  if a worker is mid-op.
        kCloseWaitMs = 300
        if self._sheets_thread is not None:
            try:
                QtCore.QMetaObject.invokeMethod(
                    self._sheets_worker, "stop",
                    QtCore.Qt.QueuedConnection,
                )
            except Exception:  # noqa: BLE001
                pass
            self._sheets_thread.quit()
            self._sheets_thread.wait(kCloseWaitMs)
            self._sheets_thread = None
            self._sheets_worker = None

        #  Remote-watcher: RemoteWatcherWorker doesn't expose a public
        #  ``stop`` slot — it's QTimer-driven on its own thread, so
        #  thread.quit() drains the event loop and the timer dies with it.
        if self._livemon_thread is not None:
            self._livemon_thread.quit()
            self._livemon_thread.wait(kCloseWaitMs)
            self._livemon_thread = None
            self._livemon_worker = None

        super().closeEvent(event)


# ---------------------------------------------------------------------------
# main()
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="qa_quicklook")
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path("."),
        help="Project root.  Defaults to the current working directory; "
             "the launcher script cd's into the repo before invoking us.",
    )
    args = parser.parse_args(argv)

    if not (args.repo_root / "conf").is_dir():
        print(
            f"error: {args.repo_root}/conf not found — run from the repo root "
            f"or pass --repo-root.",
            file=sys.stderr,
        )
        return 2

    app = QtWidgets.QApplication(sys.argv[:1])
    window = MainWindow(args.repo_root)
    # Apply the chosen theme before showing the window so we never
    # flash the platform default first.
    window.apply_current_theme()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
