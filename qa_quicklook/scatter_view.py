"""qa_quicklook/scatter_view.py — multi-run scatter tab.

Plots a first-level QA quantity (Y, from ``standard_results.toml``)
against a beam-info field (X, from the run database) over a chosen
runlist — e.g. ``N_γ vs V_bias`` — with an optional third beam-info as
the colour (Z) axis (e.g. mirror position).  Runs sharing an X value are
jittered slightly so the cluster is visible.

Data plumbing is the pure :mod:`multi_run_scatter` layer; this file is
just the Qt + matplotlib shell.
"""

from __future__ import annotations

from pathlib import Path

from PySide6 import QtCore, QtWidgets

from . import cross_run_trends, multi_run_scatter, rundb
from .runlists import _read_runlists


_NONE_LABEL = "(none)"

#  Distinct per-runlist series colours (matplotlib "tab10") used when more
#  than one runlist is overlaid on the same axes.
_SERIES_COLOURS = (
    "#1F77B4", "#FF7F0E", "#2CA02C", "#D62728", "#9467BD",
    "#8C564B", "#E377C2", "#7F7F7F", "#BCBD22", "#17BECF",
)


class _StayOpenMenu(QtWidgets.QMenu):
    """A QMenu that does NOT close when a checkable item is toggled, so the
    operator can tick several runlists in one go.  Clicking a normal
    (non-checkable) action behaves as usual."""

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 (Qt override)
        act = self.activeAction()
        if act is not None and act.isCheckable() and act.isEnabled():
            act.trigger()        # toggle the check
            return               # …but keep the menu open
        super().mouseReleaseEvent(event)


def _read_all_runlists(run_lists_dir: Path) -> dict[str, list[str]]:
    """Merge every ``*.runlists.toml`` in *run_lists_dir* into one
    ``{display_name: [run_id, …]}`` map so the scatter spans all
    campaigns, not just the newest one.  When the same runlist name
    appears in more than one campaign file the display name is suffixed
    with the campaign year so both stay reachable."""
    per_file: list[tuple[str, dict[str, list[str]]]] = []
    name_counts: dict[str, int] = {}
    for f in sorted(run_lists_dir.glob("*.runlists.toml")):
        rls = _read_runlists(f)
        year = f.name.split(".", 1)[0]  # "2026.runlists.toml" → "2026"
        per_file.append((year, rls))
        for n in rls:
            name_counts[n] = name_counts.get(n, 0) + 1
    out: dict[str, list[str]] = {}
    for year, rls in per_file:
        for n, runs in rls.items():
            disp = n if name_counts.get(n, 0) <= 1 else f"{n} ({year})"
            out[disp] = runs
    return out


class MultiRunScatterView(QtWidgets.QWidget):
    """A QA-quantity-vs-beam-info scatter over a runlist."""

    def __init__(
        self,
        data_dir: Path,
        database_path: Path,
        runlists_path: Path,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._data_dir = data_dir
        self._database_path = database_path
        self._runlists_path = runlists_path

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(12, 12, 12, 12)
        outer.setSpacing(8)

        intro = QtWidgets.QLabel(
            "First-level QA quantity vs beam condition — or vs another QA "
            "quantity — across one or more runlists.  Tick several runlists "
            "to overlay them (one colour + legend entry each); pick a Y, an "
            "X (beam-info or a QA quantity), and — for a single runlist — a "
            "colour (Z).  ‘connect’ draws a sorted curve per runlist."
        )
        intro.setObjectName("muted")
        intro.setStyleSheet("font-style: italic; font-size: 11px;")
        intro.setWordWrap(True)
        outer.addWidget(intro)

        # ── Selector bar ────────────────────────────────────────────
        bar = QtWidgets.QHBoxLayout()
        bar.setSpacing(8)
        #  Multi-select runlist picker: a button whose popup menu carries one
        #  checkable action per runlist, so several can be overlaid at once.
        self._runlist_btn = QtWidgets.QToolButton()
        self._runlist_btn.setText("none")
        self._runlist_btn.setMinimumWidth(160)
        self._runlist_btn.setToolButtonStyle(QtCore.Qt.ToolButtonTextOnly)
        self._runlist_btn.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        self._runlist_menu = _StayOpenMenu(self._runlist_btn)
        self._runlist_btn.setMenu(self._runlist_menu)
        self._suspend = False  # guards _replot during menu rebuilds
        self._y_combo = QtWidgets.QComboBox()
        self._x_combo = QtWidgets.QComboBox()
        self._z_combo = QtWidgets.QComboBox()
        for m in cross_run_trends.DEFAULT_METRICS:
            self._y_combo.addItem(m.label, m.key)
        #  X can be a beam-info field (from the run database) or a measured
        #  QA quantity (from standard_results.toml).  The latter enables
        #  measured-vs-measured plots — e.g. lane-failure-rate vs DCR.
        for f in multi_run_scatter.BEAM_AXIS_FIELDS:
            self._x_combo.addItem(f, ("field", f))
        for m in cross_run_trends.DEFAULT_METRICS:
            self._x_combo.addItem(f"{m.label}  (QA)", ("metric", m.key))
        self._z_combo.addItem(_NONE_LABEL, "")
        for f in multi_run_scatter.BEAM_AXIS_FIELDS:
            self._z_combo.addItem(f, f)
        self._connect_chk = QtWidgets.QCheckBox("connect")
        self._connect_chk.setToolTip(
            "Join points with a line, sorted by X — e.g. the lane-failure-"
            "rate vs DCR resilience curve.  Disables the cluster jitter."
        )

        bar.addWidget(QtWidgets.QLabel("Runlists:"))
        bar.addWidget(self._runlist_btn)
        for lbl, w in (("Y:", self._y_combo),
                       ("X:", self._x_combo),
                       ("colour:", self._z_combo)):
            bar.addWidget(QtWidgets.QLabel(lbl))
            bar.addWidget(w)
        bar.addWidget(self._connect_chk)
        bar.addStretch(1)
        refresh = QtWidgets.QPushButton(" ⟳ ")
        refresh.setToolTip("Reload runlists + results from disk")
        refresh.clicked.connect(self.reload)
        bar.addWidget(refresh)
        outer.addLayout(bar)

        for w in (self._y_combo, self._x_combo, self._z_combo):
            w.currentIndexChanged.connect(lambda _i: self._replot())
        self._connect_chk.toggled.connect(lambda _c: self._replot())

        # ── Canvas ──────────────────────────────────────────────────
        self._have_mpl = True
        try:
            from matplotlib.backends.backend_qtagg import FigureCanvas
            from matplotlib.figure import Figure
            self._fig = Figure(figsize=(7.5, 5.0), dpi=110, tight_layout=True)
            self._fig.patch.set_alpha(0.0)
            self._ax = self._fig.add_subplot(111)
            self._canvas = FigureCanvas(self._fig)
            outer.addWidget(self._canvas, 1)
        except ImportError:  # pragma: no cover
            self._have_mpl = False
            outer.addWidget(QtWidgets.QLabel("matplotlib not available"))

        self._status = QtWidgets.QLabel("")
        self._status.setObjectName("muted")
        self._status.setStyleSheet("font-size: 11px;")
        outer.addWidget(self._status)

        self.reload()

    # ----- data + render ----------------------------------------------

    # ----- runlist multi-select -------------------------------------------

    def _selected_runlists(self) -> list[str]:
        """Checked runlist names, in menu (alphabetical) order."""
        return [a.text() for a in self._runlist_menu.actions()
                if a.isCheckable() and a.isChecked()]

    def _update_runlist_button(self) -> None:
        sel = self._selected_runlists()
        if not sel:
            self._runlist_btn.setText("none")
        elif len(sel) == 1:
            self._runlist_btn.setText(sel[0])
        else:
            self._runlist_btn.setText(f"{len(sel)} selected")

    def _on_runlist_toggled(self, _checked: bool) -> None:
        if self._suspend:
            return
        self._update_runlist_button()
        self._replot()

    def reload(self) -> None:
        """Repopulate the runlist menu from disk, preserving the checked set."""
        prev = set(self._selected_runlists()) if self._runlist_menu.actions() else set()
        try:
            #  All campaigns, not just the newest — glob the run-lists dir.
            self._runlists_map = _read_all_runlists(self._runlists_path.parent)
            names = sorted(self._runlists_map.keys())
        except Exception:  # noqa: BLE001
            self._runlists_map = {}
            names = []

        #  Rebuild the checkable actions; suspend so the per-action toggles
        #  fired while restoring state don't trigger a replot storm.
        self._suspend = True
        self._runlist_menu.clear()
        for n in names:
            act = self._runlist_menu.addAction(n)
            act.setCheckable(True)
            act.setChecked(n in prev)
            act.toggled.connect(self._on_runlist_toggled)
        #  Default to the first runlist on a cold start so the plot isn't empty.
        if names and not any(a.isChecked() for a in self._runlist_menu.actions()):
            self._runlist_menu.actions()[0].setChecked(True)
        self._suspend = False

        self._update_runlist_button()
        self._replot()

    def _replot(self) -> None:
        if not self._have_mpl:
            return
        #  Clear the whole figure (not just the axes) so a previous
        #  colour-bar axis doesn't accumulate on every re-plot.
        self._fig.clear()
        self._ax = self._fig.add_subplot(111)
        self._ax.patch.set_alpha(0.0)
        selected = self._selected_runlists()
        if not selected:
            self._status.setText("(no runlists selected)")
            self._canvas.draw_idle()
            return

        results = rundb.results_load(self._data_dir / "standard_results.toml")
        records = rundb.load_database(self._database_path)
        metric = next(
            (m for m in cross_run_trends.DEFAULT_METRICS
             if m.key == self._y_combo.currentData()),
            cross_run_trends.DEFAULT_METRICS[0],
        )
        #  X axis: a beam-info field (run database) or a measured QA
        #  quantity (standard_results.toml).
        x_sel = self._x_combo.currentData()
        x_kind, x_key = (x_sel if isinstance(x_sel, tuple)
                         else ("field", x_sel))
        x_metric = None
        x_field = None
        if x_kind == "metric":
            x_metric = next(
                (m for m in cross_run_trends.DEFAULT_METRICS
                 if m.key == x_key), None)
            x_label = (x_metric.label + (f"  [{x_metric.unit}]"
                                         if x_metric.unit else "")
                       if x_metric else x_key)
        else:
            x_field = x_key
            x_label = x_key
        z_field = self._z_combo.currentData() or None

        connect = self._connect_chk.isChecked()
        overlay = len(selected) >= 2

        #  One series per selected runlist (shared Y / X).  The z-colour is a
        #  single-runlist affordance — when overlaying, colour encodes the
        #  runlist (legend) instead, so the z-field is ignored.
        series = multi_run_scatter.build_overlay(
            results, records,
            [(rl, self._runlists_map.get(rl, [])) for rl in selected],
            metric, x_field=x_field,
            z_field=(None if overlay else z_field), x_metric=x_metric)

        total_pts = 0
        total_skipped = 0
        plotted = 0
        for i, (rl, points, skipped) in enumerate(series):
            total_skipped += len(skipped)
            if not points:
                continue
            total_pts += len(points)
            plotted += 1
            #  Curve mode connects points sorted by X (e.g. failure-rate vs
            #  DCR) and drops the cluster jitter so the line is faithful.
            xs = ([p.x for p in points] if connect
                  else multi_run_scatter.jitter_x(points))
            ys = [p.y for p in points]
            yerr = [p.y_err if p.y_err else 0.0 for p in points]

            if overlay:
                colour = _SERIES_COLOURS[i % len(_SERIES_COLOURS)]
                self._ax.scatter(xs, ys, s=42, color=colour, label=rl,
                                 edgecolors="black", linewidths=0.4, zorder=3)
            elif z_field and any(p.z is not None for p in points):
                zs = [p.z if p.z is not None else float("nan") for p in points]
                sc = self._ax.scatter(xs, ys, c=zs, cmap="viridis", s=42,
                                      edgecolors="black", linewidths=0.4, zorder=3)
                cb = self._fig.colorbar(sc, ax=self._ax, pad=0.01)
                cb.set_label(multi_run_scatter.field_label(z_field)
                             if hasattr(multi_run_scatter, "field_label")
                             else z_field)
                colour = "#1F77B4"
            else:
                self._ax.scatter(xs, ys, s=42, color="#1F77B4",
                                edgecolors="black", linewidths=0.4, zorder=3)
                colour = "#1F77B4"

            if connect:
                order = sorted(range(len(points)), key=lambda j: points[j].x)
                self._ax.plot([points[j].x for j in order],
                              [points[j].y for j in order],
                              "-", color=colour, linewidth=1.2,
                              alpha=0.8, zorder=2)
            if any(yerr):
                self._ax.errorbar(xs, ys, yerr=yerr, fmt="none",
                                ecolor="#888888", elinewidth=0.7,
                                capsize=2, zorder=2)

        if plotted == 0:
            self._status.setText(
                f"no plottable points across {len(selected)} runlist(s) "
                f"({total_skipped} skipped — missing Y quantity or X value).")
            self._ax.set_title("no data")
            self._canvas.draw_idle()
            return

        if overlay:
            self._ax.legend(fontsize=8, framealpha=0.85, loc="best")

        self._ax.set_xlabel(x_label)
        self._ax.set_ylabel(metric.label + (f"  [{metric.unit}]" if metric.unit else ""))
        if metric.y_floor_zero:
            self._ax.set_ylim(bottom=0)
        self._ax.grid(True, linestyle=":", linewidth=0.4, alpha=0.5)
        title_rl = selected[0] if not overlay else f"{len(selected)} runlists"
        self._ax.set_title(f"{metric.label}  vs  {x_label}   ·   {title_rl}")
        foot = f"{total_pts} run(s) across {plotted}/{len(selected)} runlist(s)"
        if total_skipped:
            foot += f"  ·  {total_skipped} skipped"
        self._status.setText(foot)
        self._canvas.draw_idle()


__all__ = ["MultiRunScatterView"]
