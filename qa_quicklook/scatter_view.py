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

from PySide6 import QtWidgets

from . import cross_run_trends, multi_run_scatter, rundb
from .runlists import _read_runlists


_NONE_LABEL = "(none)"


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
            "First-level QA quantity vs beam condition across a runlist.  "
            "Pick a runlist, a Y quantity, an X beam-info, and optionally "
            "a colour (Z) axis.  Runs at the same X are nudged apart."
        )
        intro.setObjectName("muted")
        intro.setStyleSheet("font-style: italic; font-size: 11px;")
        intro.setWordWrap(True)
        outer.addWidget(intro)

        # ── Selector bar ────────────────────────────────────────────
        bar = QtWidgets.QHBoxLayout()
        bar.setSpacing(8)
        self._runlist_combo = QtWidgets.QComboBox()
        self._runlist_combo.setMinimumWidth(160)
        self._y_combo = QtWidgets.QComboBox()
        self._x_combo = QtWidgets.QComboBox()
        self._z_combo = QtWidgets.QComboBox()
        for m in cross_run_trends.DEFAULT_METRICS:
            self._y_combo.addItem(m.label, m.key)
        for f in multi_run_scatter.BEAM_AXIS_FIELDS:
            self._x_combo.addItem(f, f)
        self._z_combo.addItem(_NONE_LABEL, "")
        for f in multi_run_scatter.BEAM_AXIS_FIELDS:
            self._z_combo.addItem(f, f)

        for lbl, w in (("Runlist:", self._runlist_combo),
                       ("Y:", self._y_combo),
                       ("X:", self._x_combo),
                       ("colour:", self._z_combo)):
            bar.addWidget(QtWidgets.QLabel(lbl))
            bar.addWidget(w)
        bar.addStretch(1)
        refresh = QtWidgets.QPushButton(" ⟳ ")
        refresh.setToolTip("Reload runlists + results from disk")
        refresh.clicked.connect(self.reload)
        bar.addWidget(refresh)
        outer.addLayout(bar)

        for w in (self._runlist_combo, self._y_combo,
                  self._x_combo, self._z_combo):
            w.currentIndexChanged.connect(lambda _i: self._replot())

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

    def reload(self) -> None:
        """Repopulate the runlist picker from disk, preserving selection."""
        current = self._runlist_combo.currentText()
        self._runlist_combo.blockSignals(True)
        self._runlist_combo.clear()
        try:
            #  All campaigns, not just the newest — glob the run-lists dir.
            self._runlists_map = _read_all_runlists(self._runlists_path.parent)
            names = sorted(self._runlists_map.keys())
        except Exception:  # noqa: BLE001
            self._runlists_map = {}
            names = []
        self._runlist_combo.addItems(names)
        if current in names:
            self._runlist_combo.setCurrentText(current)
        self._runlist_combo.blockSignals(False)
        self._replot()

    def _replot(self) -> None:
        if not self._have_mpl:
            return
        #  Clear the whole figure (not just the axes) so a previous
        #  colour-bar axis doesn't accumulate on every re-plot.
        self._fig.clear()
        self._ax = self._fig.add_subplot(111)
        self._ax.patch.set_alpha(0.0)
        name = self._runlist_combo.currentText().strip()
        if not name:
            self._status.setText("(no runlists found)")
            self._canvas.draw_idle()
            return

        run_ids = getattr(self, "_runlists_map", {}).get(name, [])
        results = rundb.results_load(self._data_dir / "standard_results.toml")
        records = rundb.load_database(self._database_path)
        metric = next(
            (m for m in cross_run_trends.DEFAULT_METRICS
             if m.key == self._y_combo.currentData()),
            cross_run_trends.DEFAULT_METRICS[0],
        )
        x_field = self._x_combo.currentData()
        z_field = self._z_combo.currentData() or None

        points, skipped = multi_run_scatter.build_scatter(
            results, records, run_ids, metric, x_field, z_field)

        if not points:
            self._status.setText(
                f"{name}: no plottable points "
                f"({len(run_ids)} runs, {len(skipped)} skipped — missing "
                f"Y quantity or X value).")
            self._ax.set_title("no data")
            self._canvas.draw_idle()
            return

        xs = multi_run_scatter.jitter_x(points)
        ys = [p.y for p in points]
        yerr = [p.y_err if p.y_err else 0.0 for p in points]

        if z_field and any(p.z is not None for p in points):
            zs = [p.z if p.z is not None else float("nan") for p in points]
            sc = self._ax.scatter(xs, ys, c=zs, cmap="viridis", s=42,
                                  edgecolors="black", linewidths=0.4, zorder=3)
            cb = self._fig.colorbar(sc, ax=self._ax, pad=0.01)
            cb.set_label(z_field)
        else:
            self._ax.scatter(xs, ys, s=42, color="#1F77B4",
                            edgecolors="black", linewidths=0.4, zorder=3)
        if any(yerr):
            self._ax.errorbar(xs, ys, yerr=yerr, fmt="none",
                            ecolor="#888888", elinewidth=0.7,
                            capsize=2, zorder=2)

        self._ax.set_xlabel(x_field)
        self._ax.set_ylabel(metric.label + (f"  [{metric.unit}]" if metric.unit else ""))
        if metric.y_floor_zero:
            self._ax.set_ylim(bottom=0)
        self._ax.grid(True, linestyle=":", linewidth=0.4, alpha=0.5)
        self._ax.set_title(f"{metric.label}  vs  {x_field}   ·   {name}")
        foot = f"{len(points)} run(s)"
        if skipped:
            foot += f"  ·  {len(skipped)} skipped"
        self._status.setText(foot)
        self._canvas.draw_idle()


__all__ = ["MultiRunScatterView"]
