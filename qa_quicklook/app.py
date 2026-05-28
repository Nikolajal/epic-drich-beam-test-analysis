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
import sys
from pathlib import Path

from PySide6 import QtCore, QtGui, QtWidgets

from . import theme
from .dbworker import DbWorker
from .qa import QaView
from .runlist import RunlistView
from .runlists import RunlistsView
from .runmanager import RunManagerView
from .settings import SettingsView


# Top-level tab order (visual left-to-right).  "Advanced QA" is the
# conditional one and slots between "QA" and "Settings" when enabled.
_TAB_ORDER = ("run_info", "qa", "advanced_qa", "settings")


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

        # Run Manager — real interface for launching writers.
        run_manager = RunManagerView(
            repo_root=self._repo_root,
            data_dir=self._repo_root / "Data",
            database_path=self._repo_root / "run-lists" / "2025.database.toml",
            db_worker=self._db_worker,
        )
        qa = QaView(
            database_path=self._repo_root / "run-lists" / "2025.database.toml",
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
            database_path=self._repo_root / "run-lists" / "2025.database.toml",
            data_dir=self._repo_root / "Data",
            runlists_path=self._repo_root / "run-lists" / "2025.runlists.toml",
            db_worker=self._db_worker,
        )
        runlists = RunlistsView(
            runlists_path=self._repo_root / "run-lists" / "2025.runlists.toml",
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

        self._tab_widgets = {
            "run_info": run_info,
            "qa": qa,
            "advanced_qa": advanced_qa,
            "settings": settings,
        }
        self._tab_titles = {
            "run_info": "Run Info",
            "qa": "QA",
            "advanced_qa": "Advanced QA",
            "settings": "Settings",
        }

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

    def _on_dashboard_config_changed(self, qpath: str) -> None:
        # QFileSystemWatcher drops the path on editor-renames; re-add.
        if qpath not in self._config_watcher.files():
            self._config_watcher.addPath(qpath)
        self._rebuild_tab_bar()
        # The theme may have flipped too — re-apply.
        self.apply_current_theme()

    # ----- status bar / shortcuts ---------------------------------------

    def _build_statusbar(self) -> None:
        sb = self.statusBar()
        sb.showMessage(f"repo: {self._repo_root}")

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
