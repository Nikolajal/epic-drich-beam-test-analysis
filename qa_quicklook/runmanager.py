"""Run Manager tab — launch writers, follow them live.

v1 scope (per the design discussion):

  - **Run picker** at the top: dropdown of every sub-directory under
    ``Data/`` (each is a run id, ``YYYYMMDD-HHMMSS``).
  - **Per-writer cards** stacked below — currently lightdata and
    recodata.  Each card lays out:
      - input flags on the **left** (max spill, threads, …),
      - bool toggles on the **far right** (QA mode, force-rebuild,
        force-upstream),
      - a **Launch** button at the bottom.
  - **Stop** button up top — terminates the in-flight job (SIGTERM,
    then SIGKILL after a grace period).
  - **Live log dock** at the bottom captures the spawned writer's
    merged stdout/stderr.

Only one job at a time.  Launching disables every Launch button on
the page; the Stop button becomes available.  When the job exits,
launchers re-enable and Stop disables.

Detailed progress (elapsed / remaining / current spill) needs the
writers to emit structured ``[QA] {json}`` events first; that
protocol is queued.  Until then the log dock is the source of truth
for what's happening.
"""

from __future__ import annotations

import os
import re
import time
from pathlib import Path
from typing import Optional

from PySide6 import QtCore, QtGui, QtWidgets

from . import download as _download
from . import joblock, rundb, sanity, theme
from .datainspect import DataInspectPane
from .dbworker import DbWorker
from .runner import JobRunner, shell_quote_argv
from .writers import WRITERS, FlagSpec, WriterSpec, find_writer


# Synthetic writer tag for rsync downloads.  Reuses the JobRunner /
# joblock machinery so a download shows up in the Active runs panel,
# can be Stop'd per-run, and survives a dashboard close like any
# writer.  Not in the WRITERS catalog because there's no launchable
# binary — it's the rsync wrapper from ``qa_quicklook.download``.
_DOWNLOAD_WRITER_TAG = "download"


# Progress-line parsers — recognise the common shapes writers + rsync
# emit so the GUI progress bar updates without each writer needing a
# bespoke protocol.  Each pattern returns a float in [0, 100] or None.
#
#   - ``NN%`` / ``NN.N%``         — generic percent
#   - ``[XXXX/YYYY]`` / ``XXXX of YYYY`` — fraction (most C++ progress
#     bars use ``[i/N]`` somewhere on the line)
#   - rsync's ``to-chk=N/M``      — files-left fraction, inverted to
#     a "% done" view
_RE_PERCENT  = re.compile(r"(\d+(?:\.\d+)?)\s*%")
_RE_FRACTION = re.compile(r"(?:\[|\b)(\d+)\s*(?:/|of)\s*(\d+)(?:\])?")
_RE_RSYNC    = re.compile(r"to-chk=(\d+)/(\d+)")


def _parse_progress(line: str) -> Optional[float]:
    """Best-effort percent extracted from a writer / rsync progress line.

    Tries the cheapest patterns first so a hot tail-loop doesn't pay
    for every regex on every line.  Returns ``None`` when the line
    doesn't look like progress — the caller should leave the bar
    untouched in that case (avoid resetting on every log line).
    """
    m = _RE_PERCENT.search(line)
    if m:
        try:
            return max(0.0, min(100.0, float(m.group(1))))
        except ValueError:
            pass
    m = _RE_RSYNC.search(line)
    if m:
        # to-chk=remaining/total — invert to percent done.
        try:
            rem, tot = int(m.group(1)), int(m.group(2))
            if tot > 0:
                return max(0.0, min(100.0, 100.0 * (tot - rem) / tot))
        except ValueError:
            pass
    m = _RE_FRACTION.search(line)
    if m:
        try:
            n, d = int(m.group(1)), int(m.group(2))
            if d > 0:
                return max(0.0, min(100.0, 100.0 * n / d))
        except ValueError:
            pass
    return None


# Status dot palette → colours.  RUNNING and SUCCESS used to both be
# green which made a finished job look like it was still running.
# Distinct colours now: only RUNNING is bright green; SUCCESS is the
# muted text colour (the job is done, nothing to act on).
def _status_colour(state: str):
    pal = theme.palette()
    return {
        joblock.EFFECTIVE_RUNNING:   pal.success,     # bright green — actively going
        joblock.EFFECTIVE_SUCCESS:   pal.text_muted,  # muted — done, no action
        joblock.EFFECTIVE_ERROR:     pal.danger,
        joblock.EFFECTIVE_KILLED:    pal.danger,
        joblock.EFFECTIVE_ABANDONED: pal.warning,
        joblock.EFFECTIVE_IDLE:      pal.text_muted,
    }.get(state, pal.text_muted)


def _status_tooltip(state: str, lock) -> str:
    if lock is None:
        return "Idle — no recorded job"
    return {
        joblock.EFFECTIVE_RUNNING:   f"Running on {lock.run} (PID {lock.pid})",
        joblock.EFFECTIVE_SUCCESS:   f"Last run on {lock.run}: success (exit {lock.exit_code})",
        joblock.EFFECTIVE_ERROR:     f"Last run on {lock.run}: error (exit {lock.exit_code})",
        joblock.EFFECTIVE_KILLED:    f"Last run on {lock.run}: killed",
        joblock.EFFECTIVE_ABANDONED: f"Abandoned on {lock.run}: PID {lock.pid} is gone",
    }.get(state, state)


def _status_summary(state: str, lock) -> str:
    """One-line label next to the dot.  Running entries show elapsed."""
    if lock is None:
        return "idle"
    if state == joblock.EFFECTIVE_RUNNING:
        return f"running · {lock.run} · {_elapsed_since(lock.started_at)}"
    return {
        joblock.EFFECTIVE_SUCCESS:   f"ok · {lock.run}",
        joblock.EFFECTIVE_ERROR:     f"error · {lock.run}",
        joblock.EFFECTIVE_KILLED:    f"killed · {lock.run}",
        joblock.EFFECTIVE_ABANDONED: f"abandoned · {lock.run}",
    }.get(state, state)


def _elapsed_since(iso_ts: str) -> str:
    """Format wall-clock elapsed since ``iso_ts`` as ``Hh Mm`` / ``Ns``."""
    try:
        t0 = time.mktime(time.strptime(iso_ts, "%Y-%m-%dT%H:%M:%S"))
    except Exception:
        return "?"
    delta = max(0, int(time.time() - t0))
    h, rem = divmod(delta, 3600)
    m, s = divmod(rem, 60)
    if h:
        return f"{h}h {m:02d}m"
    if m:
        return f"{m}m {s:02d}s"
    return f"{s}s"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _list_runs(data_dir: Path) -> list[str]:
    """Real local runs under ``Data/``, most-recent first.

    Delegates to ``rundb.list_populated_runs`` so the picker shows only
    directories that (a) match the ``YYYYMMDD-HHMMSS`` run-id convention
    and (b) actually hold run content.  Phantom directories — empty
    folders left behind by a prune, or marker-only / ``.DS_Store``-only
    stubs — are filtered out so they can't fool the dashboard into
    listing a run that isn't really there.  Run ids are timestamps, so
    the reverse lexical sort is reverse-chronological (freshest on top,
    the run the operator just acquired).
    """
    return rundb.list_populated_runs(data_dir)


def _show_cascade_dialog(
    parent: QtWidgets.QWidget,
    run_id: Optional[str],
    changes: dict,
    *,
    header: Optional[str] = None,
) -> Optional[bool]:
    """Pop the "Apply N changes — pin vs cascade" dialog.

    Shared by:
      - the Run-info card's Edit-existing flow (no ``header`` — just
        list the changes),
      - the Run-info card's Add-historical flow (``header`` carries
        the upstream-insert warning so the operator knows their
        new run lands above existing entries and downstream
        merged views will shift unless they pin).

    Returns ``True`` for cascade (``auto_pin=False`` downstream),
    ``False`` for pin (``auto_pin=True``, the safer default),
    ``None`` if the user cancelled.
    """
    dlg = QtWidgets.QDialog(parent)
    dlg.setWindowTitle("Apply changes")
    lay = QtWidgets.QVBoxLayout(dlg)

    if header:
        warn = QtWidgets.QLabel(header)
        warn.setWordWrap(True)
        warn.setStyleSheet("font-weight: 600;")
        lay.addWidget(warn)

    preview = "\n".join(f"  {k} = {v}" for k, v in changes.items()) or "  (no field changes)"
    lay.addWidget(QtWidgets.QLabel(
        f"Apply {len(changes)} change(s) to {run_id}:\n\n{preview}"
    ))

    rb_pin = QtWidgets.QRadioButton(
        "Single change — only this run\n"
        "(pin the old value on the next run so downstream merged views don't shift)"
    )
    rb_cas = QtWidgets.QRadioButton(
        "Sequential cascade — let downstream runs inherit\n"
        "(every later run that doesn't override will see the new value)"
    )
    rb_pin.setChecked(True)        # safer default
    lay.addWidget(rb_pin)
    lay.addWidget(rb_cas)

    buttons = QtWidgets.QDialogButtonBox(
        QtWidgets.QDialogButtonBox.Apply | QtWidgets.QDialogButtonBox.Cancel
    )
    buttons.button(QtWidgets.QDialogButtonBox.Apply).setText("Apply")
    buttons.button(QtWidgets.QDialogButtonBox.Apply).clicked.connect(dlg.accept)
    buttons.button(QtWidgets.QDialogButtonBox.Cancel).clicked.connect(dlg.reject)
    lay.addWidget(buttons)

    if dlg.exec() != QtWidgets.QDialog.Accepted:
        return None
    return rb_cas.isChecked()


def _build_argv(spec: WriterSpec, repo_root: Path, run_id: str, flags: dict) -> list[str]:
    """Compose the argv: ``<writer> Data <run_id> [--flag value]``."""
    argv = list(spec.cmd_prefix(repo_root)) + ["Data", run_id]
    for fname, val in flags.items():
        if val is None or val is False or val == "":
            continue
        if val is True:
            argv.append(f"--{fname}")
            continue
        argv.append(f"--{fname}")
        argv.append(str(val))
    return argv


# ---------------------------------------------------------------------------
# Per-writer card
# ---------------------------------------------------------------------------


class _WriterCard(QtWidgets.QFrame):
    """One writer's launch panel.

    Emits ``launch_requested(writer_name, flags_dict)`` when the user
    clicks Launch.  The parent ``RunManagerView`` owns the runner and
    knows the current run id, so this widget stays state-free.
    """

    launch_requested = QtCore.Signal(str, dict)

    def __init__(self, spec: WriterSpec, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._spec = spec
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)

        self._inputs: dict[str, QtWidgets.QLineEdit] = {}
        self._bools: dict[str, QtWidgets.QCheckBox] = {}

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(14, 12, 14, 14)
        outer.setSpacing(8)

        # Title row: collapsible toggle (carries writer name) +
        # status dot + status summary label.
        self._writer_display = spec.name.capitalize()
        title_row = QtWidgets.QHBoxLayout()
        title_row.setSpacing(8)
        self._toggle = QtWidgets.QToolButton()
        self._toggle.setCheckable(True)
        self._toggle.setChecked(False)
        self._toggle.setText("▸  " + self._writer_display)
        f = self._toggle.font()
        f.setPointSize(f.pointSize() + 3)
        f.setBold(True)
        self._toggle.setFont(f)
        self._toggle.setStyleSheet(
            "QToolButton { border: none; text-align: left; padding: 2px 4px; }"
        )
        self._toggle.toggled.connect(self._on_toggle)
        title_row.addWidget(self._toggle, 0, QtCore.Qt.AlignVCenter)
        self._status_dot = QtWidgets.QLabel("●")
        self._status_dot.setStyleSheet("color: gray; font-size: 16px;")
        self._status_dot.setToolTip("Idle")
        title_row.addWidget(self._status_dot, 0, QtCore.Qt.AlignVCenter)
        self._status_label = QtWidgets.QLabel("idle")
        self._status_label.setObjectName("muted")
        self._status_label.setStyleSheet("font-size: 11px;")
        title_row.addWidget(self._status_label, 0, QtCore.Qt.AlignVCenter)
        title_row.addStretch(1)
        outer.addLayout(title_row)

        # Body — collapsed by default.
        self._body = QtWidgets.QWidget()
        body_layout = QtWidgets.QVBoxLayout(self._body)
        body_layout.setContentsMargins(0, 4, 0, 0)
        body_layout.setSpacing(8)
        self._body.setVisible(False)
        outer.addWidget(self._body)

        # Description inside the body.
        desc = QtWidgets.QLabel(spec.description)
        desc.setObjectName("muted")
        desc.setWordWrap(True)
        body_layout.addWidget(desc)

        # ── form row: inputs (left) + bool checkboxes (far right) ─────
        form_row = QtWidgets.QHBoxLayout()
        form_row.setSpacing(20)

        # Left side: input flags as a 2-column-of-pairs QGridLayout.
        #
        # The prior single-column QFormLayout left the right half of
        # the card empty whenever a writer had more than a couple of
        # inputs (calibration has six now: max-spill + pulser-freq +
        # 3× anchor + ...).  A 2-pair grid (label · widget · label ·
        # widget) halves the vertical footprint and fills the
        # horizontal space we already have.  The card list and form
        # order from ``writers.py`` is preserved row-by-row, top-to-
        # bottom, left-to-right — so authors can deliberately pair
        # related flags by listing them consecutively (e.g. the three
        # ``anchor-*`` flags).
        inputs_grid = QtWidgets.QGridLayout()
        inputs_grid.setHorizontalSpacing(12)
        inputs_grid.setVerticalSpacing(6)
        #  Columns: 0 = label, 1 = widget, 2 = label, 3 = widget.
        #  Widget columns stretch so each pair fills its half.
        inputs_grid.setColumnStretch(0, 0)
        inputs_grid.setColumnStretch(1, 1)
        inputs_grid.setColumnStretch(2, 0)
        inputs_grid.setColumnStretch(3, 1)
        inputs = spec.input_flags()
        for idx, flag in enumerate(inputs):
            widget = self._build_input_widget(flag)
            row = idx // 2
            col = (idx % 2) * 2
            inputs_grid.addWidget(QtWidgets.QLabel(flag.label), row, col)
            inputs_grid.addWidget(widget, row, col + 1)
            self._inputs[flag.name] = widget
        if not inputs:
            inputs_grid.addWidget(QtWidgets.QLabel("(no inputs)"), 0, 0, 1, 4)

        inputs_holder = QtWidgets.QWidget()
        inputs_holder.setLayout(inputs_grid)
        form_row.addWidget(inputs_holder, 1)
        form_row.addStretch(0)

        # Right side: bool flags.
        bools_col = QtWidgets.QVBoxLayout()
        bools_col.setSpacing(4)
        bools_label = QtWidgets.QLabel("Flags")
        bools_label.setStyleSheet("font-weight: 600;")
        bools_col.addWidget(bools_label)
        for flag in spec.bool_flags():
            cb = QtWidgets.QCheckBox(flag.label)
            cb.setChecked(bool(flag.default))
            cb.setToolTip(flag.help)
            self._bools[flag.name] = cb
            bools_col.addWidget(cb)
        bools_col.addStretch(1)
        form_row.addLayout(bools_col, 0)

        body_layout.addLayout(form_row)

        # ── Launch button row ─────────────────────────────────────────
        btn_row = QtWidgets.QHBoxLayout()
        btn_row.addStretch(1)
        self._launch_btn = QtWidgets.QPushButton(f"▶  Launch {spec.name}")
        self._launch_btn.clicked.connect(self._on_launch)
        btn_row.addWidget(self._launch_btn)
        body_layout.addLayout(btn_row)

    # ----- collapsible toggle ----------------------------------------

    def _on_toggle(self, checked: bool) -> None:
        self._body.setVisible(checked)
        arrow = "▾" if checked else "▸"
        self._toggle.setText(f"{arrow}  {self._writer_display}")

    # ----- helpers ----------------------------------------------------

    def _build_input_widget(self, flag: FlagSpec) -> QtWidgets.QLineEdit:
        widget = QtWidgets.QLineEdit()
        if flag.default is not None:
            widget.setText(str(flag.default))
        widget.setPlaceholderText("(default)")
        widget.setToolTip(flag.help)
        if flag.kind == "int":
            widget.setValidator(QtGui.QIntValidator(0, 1_000_000_000, widget))
        elif flag.kind == "float":
            #  Locale-independent — operator types ``1000`` for 1 kHz,
            #  not ``1.000,00``.  Keep generous bounds; real validation
            #  lives in the CLI driver.
            v = QtGui.QDoubleValidator(0.0, 1.0e12, 6, widget)
            v.setNotation(QtGui.QDoubleValidator.StandardNotation)
            v.setLocale(QtCore.QLocale.c())
            widget.setValidator(v)
        #  Cap kept loose so 2-col-pair grid can expand the widget
        #  toward the column-stretch.  Tight cap (140) made pairs
        #  look pinched against the empty right half.
        widget.setMaximumWidth(220)
        return widget

    # ----- public API -------------------------------------------------

    def set_enabled(self, enabled: bool) -> None:
        """Toggle the Launch button (used while another job runs)."""
        self._launch_btn.setEnabled(enabled)

    def set_writer_built(self, built: bool, executable: str) -> None:
        """Show whether the writer binary actually exists on disk."""
        if built:
            self._launch_btn.setToolTip("")
        else:
            self.set_enabled(False)
            self._launch_btn.setToolTip(f"writer not built: {executable}")

    def set_status(self, state: str, lock) -> None:
        """Update the status dot + run-id label for this writer."""
        colour = _status_colour(state)
        self._status_dot.setStyleSheet(f"color: {colour}; font-size: 16px;")
        self._status_dot.setToolTip(_status_tooltip(state, lock))
        self._status_label.setText(_status_summary(state, lock))
        self._status_label.setToolTip(_status_tooltip(state, lock))

    @property
    def spec(self) -> WriterSpec:
        return self._spec

    # ----- signals ----------------------------------------------------

    def _on_launch(self) -> None:
        #  Build the flag dict honouring each FlagSpec's declared kind
        #  so a "float" stays a float (CLI11 parses both, but mixing
        #  types here used to silently downgrade floats to int via
        #  the ``int(text)``-first ladder — which broke ``1000.5 Hz``
        #  by truncating to ``1000``).  Bool flags emit True only
        #  when ticked; unticked bools simply omit themselves from
        #  the dict (matches the writer-binary CLI default).
        kind_by_name = {f.name: f.kind for f in self._spec.flags}
        flags: dict = {}
        for name, widget in self._inputs.items():
            text = widget.text().strip()
            if not text:
                continue
            kind = kind_by_name.get(name, "string")
            try:
                if kind == "int":
                    flags[name] = int(text)
                elif kind == "float":
                    flags[name] = float(text)
                else:
                    flags[name] = text
            except ValueError:
                flags[name] = text
        for name, cb in self._bools.items():
            if cb.isChecked():
                flags[name] = True
        self.launch_requested.emit(self._spec.name, flags)


# ---------------------------------------------------------------------------
# Main view
# ---------------------------------------------------------------------------


class RunManagerView(QtWidgets.QWidget):
    """Run Manager tab — see module docstring."""

    #  Emitted when the operator picks a different run, so the app can
    #  keep the QA tab's picker in sync (and vice versa).  Only fires on
    #  genuine selection changes, never on a programmatic ``set_selected
    #  _run`` from the sync itself (guarded by ``_syncing_run``).
    run_selected = QtCore.Signal(str)

    def __init__(
        self,
        repo_root: Path,
        data_dir: Path,
        parent: QtWidgets.QWidget | None = None,
        *,
        database_path: Path | None = None,
        db_worker: DbWorker | None = None,
    ) -> None:
        super().__init__(parent)
        self._repo_root = repo_root.resolve()
        self._data_dir = data_dir.resolve()
        #  Guards programmatic combo changes (from cross-tab sync) so
        #  they don't re-broadcast and bounce the selection back.
        self._syncing_run = False
        self._database_path = database_path or rundb.newest_campaign_file(
            repo_root / "run-lists", "database")
        # Shared FIFO worker for rundb writes — avoids GUI freeze on
        # the tomlkit dumps that take ~1.7 s on production-sized DBs.
        # Falls back to a local worker if the parent didn't share one
        # (e.g. when the view is constructed in isolation for tests).
        self._db_worker = db_worker if db_worker is not None else DbWorker(self)
        self._runner = JobRunner(self) if JobRunner else None
        if self._runner is not None:
            self._runner.log_line.connect(self._on_log_line)
            self._runner.log_overwrite.connect(self._on_log_overwrite)
            self._runner.started.connect(self._on_runner_started)
            self._runner.finished.connect(self._on_runner_finished)

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(14, 14, 14, 14)
        outer.setSpacing(10)

        # Top bar — always visible regardless of sidebar state.
        top = QtWidgets.QHBoxLayout()
        top.setSpacing(8)
        top.addWidget(QtWidgets.QLabel("Run:"))
        self._run_combo = QtWidgets.QComboBox()
        self._run_combo.setMinimumWidth(260)
        top.addWidget(self._run_combo, 1)
        # Refresh status dots + preview + info whenever the selected
        # run changes — a writer that was "running" for run A might
        # be "idle" for run B.
        self._run_combo.currentTextChanged.connect(lambda _t: self._refresh_status_dots())
        self._run_combo.currentTextChanged.connect(lambda _t: self._refresh_preview())
        #  Broadcast genuine selection changes for cross-tab sync.
        self._run_combo.currentTextChanged.connect(self._on_run_selected_by_user)
        self._run_combo.currentTextChanged.connect(lambda _t: self._refresh_run_info())
        #  Refresh button — was icon-only ``⟳`` at default tool-button
        #  size and operators couldn't see it.  Spell out the word
        #  and bump the glyph so the call-to-action reads at a
        #  glance.  Same treatment applied to every other refresh
        #  button across the dashboard.
        self._refresh_btn = QtWidgets.QPushButton(" ⟳  Refresh ")
        f = self._refresh_btn.font(); f.setPointSize(f.pointSize() + 2)
        self._refresh_btn.setFont(f)
        self._refresh_btn.setToolTip("Re-scan Data/ for new runs")
        self._refresh_btn.clicked.connect(self._refresh_runs)
        top.addWidget(self._refresh_btn)
        top.addStretch(1)
        # Download → rsync a run directory from the DAQ host into
        # local Data/.  Prompts for the run id (pre-fills with the
        # current selection so re-pulling an existing run is fast),
        # then streams rsync through the JobRunner so it lands in the
        # Active runs panel with a Stop button + survives dashboard
        # close.  Tooltip surfaces the not-configured state without
        # making the user click the button to find out.
        self._download_btn = QtWidgets.QPushButton("⇣  Download")
        self._update_download_button_tip()
        self._download_btn.clicked.connect(self._on_download)
        top.addWidget(self._download_btn)
        # Inspect → launch ROOT TBrowser on every .root file in the
        # current run's directory.  Spawned detached so the
        # dashboard keeps responding while the operator browses.
        self._inspect_btn = QtWidgets.QPushButton("🔍  Inspect")
        self._inspect_btn.setToolTip(
            "Launch ROOT TBrowser (web=off) on the selected run's .root files"
        )
        self._inspect_btn.clicked.connect(self._on_inspect)
        top.addWidget(self._inspect_btn)
        # ── QA pipeline ──
        # Manually trigger the qa_pipeline cascade
        # (lightdata → recodata → recotrackdata) on the selected run.
        # Mirrors the livemon auto-trigger path; progress lands on the
        # status-bar strip MainWindow already owns.  Skip-stage-if-output
        # is on by default, so re-running a finished run is cheap.
        self._qa_pipeline_btn = QtWidgets.QPushButton("▶  QA pipeline")
        self._qa_pipeline_btn.setToolTip(
            "Run the lightdata → recodata → recotrackdata cascade on the "
            "selected run.  Stages with an existing output ROOT are skipped "
            "(use --force-rebuild semantics by passing the writer's CLI flag "
            "directly for a forced re-run)."
        )
        self._qa_pipeline_btn.clicked.connect(self._on_qa_pipeline)
        top.addWidget(self._qa_pipeline_btn)
        # Progress bar — picks up % from writer / rsync progress
        # lines.  Indeterminate (busy stripes) while a job runs but no
        # % was parsed yet; hidden when idle.  Width-capped so it
        # doesn't dominate the bar.
        self._progress = QtWidgets.QProgressBar()
        self._progress.setRange(0, 100)
        self._progress.setValue(0)
        self._progress.setFormat("idle")
        self._progress.setTextVisible(True)
        self._progress.setFixedWidth(220)
        self._progress.setMaximumHeight(20)
        self._progress.setToolTip(
            "Progress parsed from the running writer / rsync output."
        )
        top.addWidget(self._progress)
        self._stop_btn = QtWidgets.QPushButton("✕  Stop")
        self._stop_btn.setObjectName("dangerButton")
        self._stop_btn.setEnabled(False)
        self._stop_btn.clicked.connect(self._on_stop)
        top.addWidget(self._stop_btn)
        outer.addLayout(top)

        # Main area — left sidebar (preview) + right main (writer
        # cards + log) inside a horizontal splitter.
        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        splitter.setChildrenCollapsible(False)

        # --- Left sidebar ----------------------------------------------
        sidebar = QtWidgets.QWidget()
        sidebar_layout = QtWidgets.QVBoxLayout(sidebar)
        sidebar_layout.setContentsMargins(0, 0, 6, 0)
        sidebar_layout.setSpacing(8)

        self._run_info = _RunInfoCard(
            dashboard_config=self._repo_root / "qa_quicklook" / "qa_quicklook.toml",
        )
        self._run_info.set_quality_callback(self._on_quality_changed_from_card)
        self._run_info.set_save_callback(self._on_run_info_save)
        sidebar_layout.addWidget(self._run_info)

        # Active runs: every writer currently RUNNING (across this and
        # any other dashboard instance) with a per-run Stop button.
        # The log dock doesn't exist yet at this point — we wire it in
        # later once it's built (see end of __init__).
        self._active = _ActiveRunsPanel()
        sidebar_layout.addWidget(self._active)

        self._preview = DataInspectPane()
        sidebar_layout.addWidget(self._preview)
        sidebar_layout.addStretch(1)

        splitter.addWidget(sidebar)

        # --- Right main pane ------------------------------------------
        main = QtWidgets.QWidget()
        main_layout = QtWidgets.QVBoxLayout(main)
        main_layout.setContentsMargins(6, 0, 0, 0)
        main_layout.setSpacing(8)

        self._cards: list[_WriterCard] = []
        for spec in WRITERS:
            card = _WriterCard(spec)
            card.launch_requested.connect(self._on_launch_requested)
            card.set_writer_built(spec.exists(self._repo_root), spec.executable)
            main_layout.addWidget(card)
            self._cards.append(card)

        # Live log dock.
        log_label = QtWidgets.QLabel("Live log")
        log_label.setStyleSheet("font-weight: 600; padding-top: 4px;")
        main_layout.addWidget(log_label)
        self._log = QtWidgets.QPlainTextEdit()
        self._log.setReadOnly(True)
        self._log.setMaximumBlockCount(5000)
        font = QtGui.QFont("Menlo")
        font.setStyleHint(QtGui.QFont.Monospace)
        font.setPointSize(11)
        self._log.setFont(font)
        # Terminal-style dock — fixed dark palette regardless of
        # theme, so writer output reads like a real shell.  Selection
        # uses the accent for visibility.
        self._log.setStyleSheet(
            "QPlainTextEdit {"
            " background: #0F0F10;"
            " color: #DDE3D1;"
            " border: 1px solid #2A2929;"
            " selection-background-color: #FF6B6B;"
            " selection-color: #FFFFFF;"
            " padding: 8px;"
            "}"
        )
        main_layout.addWidget(self._log, 1)
        # Now that the log dock exists, hand it to the active panel
        # so its Stop / status messages go there too.
        self._active.set_log_dock(self._log)

        splitter.addWidget(main)
        splitter.setSizes([320, 880])
        outer.addWidget(splitter, 1)

        # 2-second liveness poll so an externally-killed PID (e.g. the
        # operator ``kill``ed the writer from a shell) flips its dot
        # to ``abandoned`` without needing a click.
        self._status_timer = QtCore.QTimer(self)
        self._status_timer.setInterval(2000)
        self._status_timer.timeout.connect(self._refresh_status_dots)
        self._status_timer.timeout.connect(self._refresh_active)
        # Self-heal the run-picker enabled state against reality.  The
        # running-flag used to be latched: _set_running(True) on launch,
        # cleared only by the runner's `finished` signal.  If a writer
        # died WITHOUT a clean finished (killed externally, an attached
        # survivor that exited, a missed signal), the picker stayed
        # greyed-out forever.  Reconciling against `_runner.is_running()`
        # every tick re-enables it within 2 s once nothing is actually
        # running — and disables it if an external run appears.
        self._status_timer.timeout.connect(self._poll_running_state)
        # Cheap: re-read [rsync] so a Settings-side edit shows up in
        # the Download button's tooltip without waiting for a click.
        self._status_timer.timeout.connect(self._update_download_button_tip)
        self._status_timer.start()

        # Periodic re-map of the locally-held runs so the picker stays
        # in sync with what's actually on disk — a run that finished
        # downloading, or one whose folder was pruned away, shows up /
        # disappears within one tick without the operator hitting ⟳.
        # Slower cadence than the liveness poll (30 s) because the
        # on-disk run SET changes rarely, and the re-scan touches every
        # run dir; ``_refresh_runs`` is a no-op-safe rebuild that
        # preserves the current selection when it's still present.
        self._runs_remap_timer = QtCore.QTimer(self)
        self._runs_remap_timer.setInterval(30_000)
        self._runs_remap_timer.timeout.connect(self._remap_runs_if_changed)
        self._runs_remap_timer.start()

        self._refresh_runs()
        self._refresh_status_dots()
        self._refresh_preview()
        self._refresh_run_info()
        # If a writer survived a previous dashboard close, re-attach
        # so we can show its log + spot its exit.
        self._maybe_attach_survivor()

    def _maybe_attach_survivor(self) -> None:
        if self._runner is None or self._runner.is_running():
            return
        for lock in joblock.list_locks():
            if joblock.effective_state(lock) != joblock.EFFECTIVE_RUNNING:
                continue
            self._log.appendPlainText(
                f"[note] re-attaching to {lock.writer} on {lock.run} (PID {lock.pid})"
            )
            self._runner.attach_external(lock.writer, lock.run, lock.pid)
            self._set_running(True)
            return

    # ----- internals --------------------------------------------------

    def _refresh_runs(self) -> None:
        current = self._run_combo.currentText()
        self._run_combo.clear()
        runs = _list_runs(self._data_dir)
        self._run_combo.addItems(runs)
        if current in runs:
            self._run_combo.setCurrentText(current)
        if not runs:
            self._log.appendPlainText(f"[note] no runs found under {self._data_dir}")
        self._refresh_status_dots()

    def _remap_runs_if_changed(self) -> None:
        """Periodic re-map (30 s timer): rebuild the picker ONLY when the
        set of real local runs actually changed.

        Rebuilding the combo every tick would fight the operator (drop
        an open dropdown, reset edit focus), so we diff the on-disk run
        set against what the combo currently holds and rebuild only on a
        real delta — a finished download appearing, or a pruned folder
        disappearing.  Cheap: ``_list_runs`` is a shallow scan.
        """
        on_disk = _list_runs(self._data_dir)
        in_combo = [self._run_combo.itemText(i)
                    for i in range(self._run_combo.count())]
        if on_disk != in_combo:
            self._refresh_runs()

    def _refresh_preview(self) -> None:
        run_id = self._run_combo.currentText().strip()
        run_dir = (self._data_dir / run_id) if run_id else None
        self._preview.set_run(run_dir)

    def _on_quality_changed_from_card(self, run_id: str, quality: str) -> None:
        """Quick set-quality from the Run Manager sidebar.

        Dispatches through the background DB worker so the GUI
        doesn't freeze during the (~1.7 s) tomlkit write.  The
        run-info card refreshes on the worker's done signal.  Empty
        string is a no-op for now (no "clear" helper yet).
        """
        if not quality:
            return
        path = self._database_path
        # Free-form tag so we can recognise our own submissions in
        # the worker's done signal (shared worker → other views also
        # use the same channel).
        tag = f"rm-quality:{run_id}:{id(self)}"
        # Connect once on first use — idempotent reconnects are
        # blocked by Qt's UniqueConnection.
        self._db_worker.done.connect(
            self._on_db_worker_done,
            QtCore.Qt.UniqueConnection,
        ) if not getattr(self, "_db_worker_connected", False) else None
        self._db_worker_connected = True

        def do_write() -> None:
            rundb.update_run_field(
                path, run_id, "quality", quality, auto_pin=True,
            )
        self._db_worker.submit(tag, do_write)

    def _on_db_worker_done(self, tag: str, ok: bool, error: str) -> None:
        # Only react to our own submissions.  Tag prefix routes the
        # callback: ``rm-quality:`` for the legacy quality dropdown,
        # ``rm-info:`` for the full run-info card edits (Add / Edit).
        if f":{id(self)}" not in tag:
            return
        if tag.startswith("rm-quality:"):
            if not ok:
                self._log.appendPlainText(f"[ERROR] set quality failed: {error.splitlines()[0]}")
                return
            self._refresh_run_info()
            return
        if tag.startswith("rm-info:"):
            if not ok:
                self._log.appendPlainText(
                    f"[ERROR] run-info save failed: {error.splitlines()[0]}"
                )
                return
            #  Disk landed — re-read the database so the card and any
            #  pin/cascade neighbour reflect the new state.
            self._refresh_run_info()
            return

    def _on_run_info_save(
        self,
        run_id: str,
        changes: dict,
        is_new_run: bool,
        cascade: bool,
    ) -> None:
        """Persist edits from the Run Info card.

        Dispatches via the shared ``DbWorker`` so the (slow) tomlkit
        write doesn't freeze the GUI.  For new runs we
        ``append_runs`` first (forward-inheritance fills the
        gaps), then loop ``update_run_field`` with ``auto_pin=True``
        (cascade is meaningless on a freshly-appended tail run —
        nothing downstream exists yet).  For existing runs the
        operator picked cascade-or-pin in the dialog; we honour it.
        """
        path = self._database_path
        tag = f"rm-info:{run_id}:{id(self)}"
        self._db_worker.done.connect(
            self._on_db_worker_done, QtCore.Qt.UniqueConnection,
        ) if not getattr(self, "_db_worker_connected", False) else None
        self._db_worker_connected = True

        #  ── Upstream-insert detection ───────────────────────────────
        #  When we're adding a NEW run whose timestamp puts it before
        #  at least one existing entry, the insert lands upstream of
        #  those entries.  Forward-inheritance then makes the new
        #  run's fields propagate down the chain unless we explicitly
        #  pin them on the next inheriting run.  Pop the same
        #  cascade dialog the Edit flow uses, with extra wording so
        #  the operator knows what they're touching.  No prompt for
        #  the common tail-append case (run id newer than every
        #  existing one) — there's nothing downstream to worry about.
        if is_new_run and changes:
            try:
                existing_ids = [r.run_id for r in rundb.load_database(path)]
            except Exception:  # noqa: BLE001
                existing_ids = []
            n_downstream = sum(1 for rid in existing_ids if rid > run_id)
            if n_downstream > 0:
                header = (
                    f"⚠ Adding {run_id} upstream of {n_downstream} existing "
                    f"run(s).\n\nForward-inheritance means the values you "
                    f"enter here will flow into every later run that "
                    f"doesn't already set the same field, unless you pin "
                    f"them now."
                )
                choice = _show_cascade_dialog(
                    self, run_id, changes, header=header,
                )
                if choice is None:
                    return  # user cancelled the add
                cascade = choice

        auto_pin = not cascade

        def do_write() -> None:
            if is_new_run:
                rundb.append_runs(path, [run_id])
                #  Honour the cascade choice for upstream inserts.  For
                #  a tail-append (n_downstream == 0 above ⇒ cascade
                #  stays False ⇒ auto_pin=True) this is the same as
                #  before — pin is a no-op when nothing is downstream.
                for field, value in changes.items():
                    rundb.update_run_field(
                        path, run_id, field, value, auto_pin=auto_pin,
                        source="dashboard",
                    )
            else:
                for field, value in changes.items():
                    rundb.update_run_field(
                        path, run_id, field, value, auto_pin=auto_pin,
                        source="dashboard",
                    )

        self._db_worker.submit(tag, do_write)

    def _refresh_run_info(self) -> None:
        run_id = self._run_combo.currentText().strip()
        if not run_id:
            self._run_info.show_empty()
            return
        try:
            records = rundb.load_database(self._database_path)
        except Exception:  # noqa: BLE001
            self._run_info.show_empty()
            return
        record = next((r for r in records if r.run_id == run_id), None)
        self._run_info.show_record(record, run_id)

    def _refresh_active(self) -> None:
        self._active.refresh()

    def _on_run_selected_by_user(self, run_id: str) -> None:
        """Re-broadcast a genuine run-picker change for cross-tab sync.

        Suppressed while ``_syncing_run`` is set (i.e. the change came
        FROM the sync), so the two tabs can't ping-pong the selection.
        """
        if not self._syncing_run and run_id:
            self.run_selected.emit(run_id)

    def set_selected_run(self, run_id: str) -> None:
        """Programmatically select ``run_id`` without re-broadcasting.

        No-op when the run isn't in the picker (e.g. a QA-only run not
        present in the Run Manager's filesystem list) or already
        selected.  The combo's own refresh signals still fire so the
        status dots / preview follow, but ``run_selected`` does not —
        ``_syncing_run`` gates it.
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

    def _poll_running_state(self) -> None:
        """Reconcile the run-picker enabled state with reality.

        ``_set_running`` is the only thing that disables the run combo,
        and it used to be cleared solely by the runner's ``finished``
        signal — so a writer that died without firing it left the
        picker greyed-out indefinitely.  Here we re-derive the running
        flag from ``_runner.is_running()`` (which polls the live popen /
        attached external pid) and only touch the UI when it has drifted
        from the picker's current state, so a stuck disabled picker
        re-enables within one 2 s tick.
        """
        actually_running = (
            self._runner is not None and self._runner.is_running()
        )
        #  Picker enabled iff NOT running.  Reconcile only on drift so we
        #  don't thrash the cards' enabled state every tick.
        if self._run_combo.isEnabled() == actually_running:
            self._set_running(actually_running)

    def _refresh_status_dots(self) -> None:
        """Update each writer card's dot from the on-disk lock files.

        Status is reported *for the currently selected run*:

          - If the writer is currently running on **any** run we surface
            that (the operator needs to know the writer is busy, even
            if the dashboard is parked on a different run).
          - Otherwise we look for a finished lock matching the
            *selected* run id.  A "success" from a different run is
            irrelevant context here — it would falsely imply the
            selected run had been processed.
          - Otherwise: idle.
        """
        by_writer: dict[str, list[joblock.JobLock]] = {}
        for lock in joblock.list_locks():
            by_writer.setdefault(lock.writer, []).append(lock)

        selected_run = self._run_combo.currentText().strip()

        for card in self._cards:
            locks = by_writer.get(card.spec.name, [])
            # Prefer a still-running one (any run — alerts the operator).
            running = [l for l in locks if joblock.effective_state(l) == joblock.EFFECTIVE_RUNNING]
            if running:
                # Should be at most one (single subprocess per writer).
                lock = running[0]
            elif selected_run:
                # Finished status is only meaningful for the selected run.
                matching = [l for l in locks if l.run == selected_run]
                lock = max(
                    matching,
                    key=lambda l: (l.finished_at or "", l.started_at or ""),
                ) if matching else None
            else:
                lock = None
            state = joblock.effective_state(lock)
            card.set_status(state, lock)

    def _set_running(self, running: bool) -> None:
        for card in self._cards:
            # Re-applying the built-or-not state on enable so a
            # missing-binary card stays disabled even after a job ends.
            if running:
                card.set_enabled(False)
            else:
                spec = find_writer(card.spec.name)
                built = spec is not None and spec.exists(self._repo_root)
                card.set_enabled(built)
        self._stop_btn.setEnabled(running)
        self._run_combo.setEnabled(not running)

    # ----- signals ----------------------------------------------------

    def _on_launch_requested(self, writer_name: str, flags: dict) -> None:
        if self._runner is None:
            self._log.appendPlainText("[ERROR] JobRunner unavailable (PySide6 import failed?)")
            return
        run_id = self._run_combo.currentText().strip()
        if not run_id:
            self._log.appendPlainText("[ERROR] no run selected")
            return
        spec = find_writer(writer_name)
        if spec is None:
            self._log.appendPlainText(f"[ERROR] unknown writer: {writer_name}")
            return
        if not spec.exists(self._repo_root):
            self._log.appendPlainText(f"[ERROR] writer not built: {spec.executable}")
            return

        # Per-run mutex: another writer might already be running on
        # this run id (possibly from another dashboard instance).
        # Refuse rather than race for the same files.
        for other_spec in WRITERS:
            if other_spec.name == writer_name:
                continue
            other_lock = joblock.read_lock(other_spec.name, run_id)
            if other_lock and joblock.effective_state(other_lock) == joblock.EFFECTIVE_RUNNING:
                QtWidgets.QMessageBox.warning(
                    self, "Run is busy",
                    f"Cannot launch {writer_name} on run {run_id}: "
                    f"{other_spec.name} is already running on this run "
                    f"(PID {other_lock.pid}).\n\nStop it first, or wait "
                    f"for it to finish.",
                )
                self._log.appendPlainText(
                    f"[ERROR] {writer_name} blocked — {other_spec.name} already running on {run_id}"
                )
                return

        argv = _build_argv(spec, self._repo_root, run_id, flags)
        confirm = QtWidgets.QMessageBox(self)
        confirm.setWindowTitle(f"Launch {writer_name}?")
        confirm.setIcon(QtWidgets.QMessageBox.Question)
        confirm.setText(f"About to launch {writer_name} on run {run_id}.")
        confirm.setInformativeText(shell_quote_argv(argv))
        confirm.setStandardButtons(
            QtWidgets.QMessageBox.Ok | QtWidgets.QMessageBox.Cancel
        )
        if confirm.exec() != QtWidgets.QMessageBox.Ok:
            return

        self._log.clear()
        self._set_running(True)
        self._runner.launch(
            argv,
            default_source=writer_name,
            writer=writer_name,
            run=run_id,
        )
        self._refresh_status_dots()

    def _on_stop(self) -> None:
        if self._runner is not None:
            self._runner.stop()

    # ----- Download (rsync) ---------------------------------------------

    def _dashboard_config_path(self) -> Path:
        return self._repo_root / "qa_quicklook" / "qa_quicklook.toml"

    def _prompt_for_rsync_address(
        self, current: "_download.RsyncConfig"
    ) -> Optional["_download.RsyncConfig"]:
        """Pop a 2-field dialog, persist on Accept, return updated config.

        Returns ``None`` if the user cancelled.  Returns the reloaded
        ``RsyncConfig`` on success — caller checks ``is_configured``
        again because the user might have left one field blank.
        Errors writing the file surface as a warning box; the user
        is then expected to fix things via the Settings tab.
        """
        dlg = QtWidgets.QDialog(self)
        dlg.setWindowTitle("Set rsync remote")
        form = QtWidgets.QFormLayout(dlg)

        intro = QtWidgets.QLabel(
            "The rsync remote isn't set yet.  Fill it in here to "
            "enable Download — the values are persisted to "
            "<code>qa_quicklook.toml</code> so this dialog only "
            "appears once.",
            dlg,
        )
        intro.setWordWrap(True)
        intro.setTextFormat(QtCore.Qt.RichText)
        form.addRow(intro)

        host_edit = QtWidgets.QLineEdit(current.remote_host, dlg)
        host_edit.setPlaceholderText("e.g. drich-daq.local or user@host")
        form.addRow("Remote host", host_edit)

        dir_edit = QtWidgets.QLineEdit(current.remote_data_dir, dlg)
        dir_edit.setPlaceholderText("absolute path on the DAQ host, e.g. /data/runs")
        form.addRow("Remote data dir", dir_edit)

        #  ── SSH key auth probe ──────────────────────────────────────
        #  One-shot "Test SSH" affordance so the operator can verify
        #  passwordless auth landed (after running ``ssh-copy-id``)
        #  without leaving the GUI.  Runs ``ssh -o BatchMode=yes``
        #  against whatever's currently in the host field — any
        #  prompt-required state (no key / wrong key / unknown host)
        #  fails fast and the result label tells them what to do.
        probe_row = QtWidgets.QHBoxLayout()
        probe_btn = QtWidgets.QPushButton("Test SSH key auth")
        probe_btn.setToolTip(
            "Runs ssh -o BatchMode=yes against the host field — green ✓ "
            "means passwordless rsync will work; anything else surfaces "
            "ssh's first error line so you can fix it."
        )
        probe_result = QtWidgets.QLabel("")
        probe_result.setWordWrap(True)

        def on_probe() -> None:
            host = host_edit.text().strip()
            if not host:
                probe_result.setText(
                    "<span style='color:#E0A40A;'>fill in the host first</span>"
                )
                return
            probe_result.setText("…probing…")
            QtWidgets.QApplication.processEvents()
            ok, msg = _download.probe_ssh_keyauth(host)
            colour = "#0BDA51" if ok else "#FF6B6B"
            mark = "✓" if ok else "✗"
            probe_result.setTextFormat(QtCore.Qt.RichText)
            probe_result.setText(
                f"<span style='color:{colour};'>{mark} {msg}</span>"
                + ("" if ok else (
                    "<br><span style='color:#9B8E8E;'>Fix: run "
                    f"<code>ssh-copy-id {host}</code> in a terminal "
                    "(see the chat thread for the full setup).</span>"
                ))
            )

        probe_btn.clicked.connect(on_probe)
        probe_row.addWidget(probe_btn)
        probe_row.addWidget(probe_result, 1)
        form.addRow(probe_row)

        buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel,
            QtCore.Qt.Horizontal,
            dlg,
        )
        buttons.accepted.connect(dlg.accept)
        buttons.rejected.connect(dlg.reject)
        form.addRow(buttons)

        if dlg.exec() != QtWidgets.QDialog.Accepted:
            return None

        host = host_edit.text().strip()
        rdir = dir_edit.text().strip()
        if not host or not rdir:
            QtWidgets.QMessageBox.warning(
                self, "Download — incomplete address",
                "Both fields are required.  Re-open Download to try "
                "again, or fill them in on the Settings tab.",
            )
            return None

        try:
            _download.save_address(
                self._dashboard_config_path(),
                remote_host=host,
                remote_data_dir=rdir,
            )
        except OSError as exc:
            QtWidgets.QMessageBox.warning(
                self, "Download — write failed",
                f"Could not write qa_quicklook.toml: {exc}\n\n"
                "Fix it on the Settings tab and try again.",
            )
            return None

        #  Update the tooltip and re-read the file (proves the write
        #  landed cleanly and picks up the existing local_data_dir /
        #  extra_args that the dialog didn't touch).
        self._update_download_button_tip()
        return _download.load_config(self._dashboard_config_path())

    def _update_download_button_tip(self) -> None:
        """Tooltip surfaces the configured-state without a click."""
        cfg = _download.load_config(self._dashboard_config_path())
        if cfg.is_configured:
            self._download_btn.setToolTip(
                f"rsync from {cfg.remote_host}:{cfg.remote_data_dir}/<run>\n"
                f"into {cfg.local_data_dir}/  (extra args: {cfg.extra_args})"
            )
        else:
            self._download_btn.setToolTip(
                "rsync remote is not configured — fill in "
                "[rsync].remote_host and [rsync].remote_data_dir on the "
                "Settings tab to enable Download."
            )

    def _on_download(self) -> None:
        """Prompt for a run id, then rsync it from the DAQ host."""
        if self._runner is None:
            self._log.appendPlainText("[ERROR] JobRunner unavailable")
            return
        cfg = _download.load_config(self._dashboard_config_path())
        if not cfg.is_configured:
            #  Pop the address-prompt dialog instead of bailing.  On
            #  Accept we persist back to qa_quicklook.toml (Settings
            #  tab still owns the durable editor; this is the "fix
            #  it on first use" path) and re-read the config before
            #  continuing the download flow.  Cancel → return as
            #  before.
            cfg = self._prompt_for_rsync_address(cfg)
            if cfg is None or not cfg.is_configured:
                return

        default_run = self._run_combo.currentText().strip()
        #  Try the listing-picker first — operator browses the DAQ
        #  host's run directory, picks visually, already-mirrored runs
        #  are badged so re-download mistakes are obvious.  The picker
        #  has its own "Type id manually…" escape hatch and we also
        #  fall back automatically when the listing never produced any
        #  entries (e.g. ssh broken at exactly the moment of the click).
        picker = _RemoteRunsPickerDialog(
            cfg, self._repo_root,
            default_run=default_run,
            parent=self,
        )
        if picker.exec() != QtWidgets.QDialog.Accepted:
            return   # user cancelled
        run_id = "" if picker.manual_entry else picker.selected_run_id

        if not run_id:
            #  Fallback path: typed-id input.  Either the operator
            #  explicitly clicked "Type id manually…" or the picker
            #  didn't yield a selection (rare race — kept for safety).
            run_id, ok = QtWidgets.QInputDialog.getText(
                self, "Download run",
                "Run id to fetch (YYYYMMDD-HHMMSS):",
                QtWidgets.QLineEdit.Normal,
                default_run,
            )
            if not ok or not run_id.strip():
                return
            run_id = run_id.strip()

        # Refuse if a download is already going for this run id —
        # the joblock mutex would catch it anyway, but a clear
        # message up-front beats the cryptic rsync error.
        existing = joblock.read_lock(_DOWNLOAD_WRITER_TAG, run_id)
        if existing and joblock.effective_state(existing) == joblock.EFFECTIVE_RUNNING:
            QtWidgets.QMessageBox.information(
                self, "Download already running",
                f"A download for run {run_id} is already in flight "
                f"(PID {existing.pid}).  Wait for it to finish or "
                f"Stop it from the Active runs panel.",
            )
            return

        try:
            argv = _download.build_argv(cfg, run_id, self._repo_root)
        except ValueError as exc:
            QtWidgets.QMessageBox.warning(self, "Download", str(exc))
            return

        dest = _download.expected_local_path(cfg, run_id, self._repo_root)
        confirm = QtWidgets.QMessageBox(self)
        confirm.setWindowTitle(f"Download {run_id}?")
        confirm.setIcon(QtWidgets.QMessageBox.Question)
        confirm.setText(f"rsync run {run_id} into {dest}")
        confirm.setInformativeText(shell_quote_argv(argv))
        confirm.setStandardButtons(
            QtWidgets.QMessageBox.Ok | QtWidgets.QMessageBox.Cancel
        )
        if confirm.exec() != QtWidgets.QMessageBox.Ok:
            return

        # Retention sweep BEFORE the rsync lands — keeps disk free for
        # the incoming run + grooms older tiers in one go.  Bubbles up
        # to MainWindow which owns the config + status-bar; fail-quiet
        # so a sweep error never blocks the download.
        try:
            mw = self.window()
            if hasattr(mw, "_run_retention_sweep"):
                mw._run_retention_sweep(reason="pre-download")
        except Exception:  # noqa: BLE001
            pass

        # Reuse the same launch path as writers so the Active runs
        # panel + log dock + per-run Stop all work out of the box.
        self._log.clear()
        self._set_running(True)

        #  Sweep audit (2026-05-30) — three-state retention model.
        #  Drop the `.qa_managed` marker into the run directory before
        #  rsync starts populating it.  Two reasons:
        #    1. The retention sweep uses the marker to distinguish
        #       auto-downloaded runs from user-managed ones.  This is
        #       the ONLY thing that makes a run eligible for the sweep —
        #       without the marker, the sweep treats the run as
        #       user-managed and never touches it (by design).
        #    2. Partial/failed downloads should be eligible for
        #       cleanup; dropping the marker at launch time (not at
        #       successful completion) lets the next sweep clean them.
        try:
            from qa_quicklook import retention as _retention
            target_dir = _download.expected_local_path(
                cfg, run_id, self._repo_root)
            _retention.mark_qa_managed(target_dir)
        except (OSError, ImportError) as exc:
            #  Marker drop is best-effort — a failure here doesn't
            #  block the download itself.  Surface in the log so the
            #  operator knows retention won't see this run as managed.
            self._log.appendPlainText(
                f"[WARN] .qa_managed marker not dropped: {exc}; "
                "this run won't be eligible for the retention sweep "
                "until the marker is added (touch the file manually "
                "or use `python -m qa_quicklook.retention pin`).")

        self._runner.launch(
            argv,
            default_source=_DOWNLOAD_WRITER_TAG,
            writer=_DOWNLOAD_WRITER_TAG,
            run=run_id,
        )
        self._refresh_status_dots()

    def _on_inspect(self) -> None:
        """Launch ROOT TBrowser on the selected run's .root files.

        Spawns ``build/bin/qa_tbrowser`` — a tiny stand-alone binary
        that owns its own ``TApplication`` + ``TBrowser`` + GUI event
        loop.  See ``macros/utilities/qa_tbrowser.cpp`` for why we
        moved off the ``root -e 'new TBrowser'`` / ``osascript+Terminal``
        path: both were fragile (no-TTY / shell-quoting nightmare /
        macOS-only).  A real binary just works.
        """
        run_id = self._run_combo.currentText().strip()
        if not run_id:
            self._log.appendPlainText("[ERROR] no run selected for inspection")
            return
        run_dir = self._data_dir / run_id
        if not run_dir.is_dir():
            self._log.appendPlainText(f"[ERROR] run directory missing: {run_dir}")
            return
        root_files = sorted(run_dir.glob("*.root"))
        if not root_files:
            self._log.appendPlainText(f"[note] no .root files under {run_dir}")
            return

        binary = self._repo_root / "build" / "bin" / "qa_tbrowser"
        if not binary.is_file():
            self._log.appendPlainText(
                f"[ERROR] qa_tbrowser not built: {binary}.  "
                "Run scripts/install.sh or rebuild the C++ side."
            )
            return
        argv = [binary.as_posix()] + [f.as_posix() for f in root_files]
        # ``startDetached`` returns the spawned PID via overload; the
        # bool we care about is the truthy return.  Detached so the
        # dashboard stays decoupled from the TBrowser's lifetime.
        ok = QtCore.QProcess.startDetached(argv[0], argv[1:])
        if not ok:
            self._log.appendPlainText(
                "[ERROR] could not spawn qa_tbrowser — check the binary "
                "is executable + ROOT libs are findable."
            )
            return
        self._log.appendPlainText(
            f"[inspect] qa_tbrowser launched on "
            f"{len(root_files)} file(s) from {run_dir.name}"
        )

    def _on_qa_pipeline(self) -> None:
        """Manually launch the qa_pipeline cascade on the selected run.

        Delegates to ``MainWindow._launch_qa_pipeline_manual`` so the
        status-bar progress strip and the QProcess lifetime live with
        the same owner that handles the livemon-auto path — no
        duplicated stdout-parsing, finished-handler, or cleanup.

        Skip-stage-when-output-exists is on by default in qa_pipeline;
        the operator gets the cascade to skip what's already produced
        rather than redo expensive work.  For a forced rebuild use the
        per-writer Launch buttons with the writer's ``--force-rebuild``
        flag in the launcher card.
        """
        run_id = self._run_combo.currentText().strip()
        if not run_id:
            self._log.appendPlainText(
                "[ERROR] no run selected for QA pipeline"
            )
            return
        run_dir = self._data_dir / run_id
        if not run_dir.is_dir():
            self._log.appendPlainText(
                f"[ERROR] run directory missing: {run_dir}"
            )
            return
        window = self.window()
        if not hasattr(window, "_launch_qa_pipeline_manual"):
            self._log.appendPlainText(
                "[ERROR] dashboard missing qa_pipeline launcher — "
                "older MainWindow version detected."
            )
            return
        window._launch_qa_pipeline_manual(run_id)
        self._log.appendPlainText(
            f"[qa_pipeline] launched on {run_id}"
        )

    def _maybe_update_progress(self, line: str) -> None:
        """Feed a log line to the parser; update the bar on a hit.

        Untouched on a miss — we don't want to reset the bar back to
        0% just because the writer printed an unrelated info line
        between two progress updates.
        """
        pct = _parse_progress(line)
        if pct is None:
            return
        # Switch out of indeterminate ("busy stripes") mode now that
        # we have an actual reading.
        if self._progress.minimum() == 0 and self._progress.maximum() == 0:
            self._progress.setRange(0, 100)
        self._progress.setValue(int(round(pct)))
        self._progress.setFormat(f"{pct:.0f}%")

    def _on_log_overwrite(self, line: str) -> None:
        """Terminal-style ``\\r`` update — replace the last visible line.

        Two subtleties that bite if you don't handle them:

          1. ``QTextCursor.StartOfBlock`` only selects from the start
             of the *current* block; after a previous ``appendPlainText``,
             the cursor sits at the start of a *new* empty block, so
             selecting back overwrites nothing.  We explicitly walk
             one block left when the last block is empty.
          2. Replacing a long progress line with a shorter one leaves
             trailing characters from the previous render.  Track the
             length of the last overwrite and pad short replacements
             with spaces.
        """
        cursor = self._log.textCursor()
        cursor.movePosition(QtGui.QTextCursor.End)
        cursor.movePosition(QtGui.QTextCursor.StartOfBlock)
        # If we're on an empty block (e.g. just after appendPlainText
        # of a previous regular log line), step back one block so we
        # overwrite the actual last visible line.
        if cursor.atEnd() and cursor.position() > 0:
            # The block is empty — nothing to overwrite — just insert.
            pass
        cursor.movePosition(QtGui.QTextCursor.EndOfBlock, QtGui.QTextCursor.KeepAnchor)
        prev_len = len(getattr(self, "_last_overwrite_text", ""))
        padded = line.ljust(prev_len)
        cursor.insertText(padded)
        self._last_overwrite_text = line
        bar = self._log.verticalScrollBar()
        bar.setValue(bar.maximum())
        # Progress lines tend to come via \r-overwrite ("[####  ] 42%"),
        # so this is the path that updates the GUI progress bar most
        # of the time.
        self._maybe_update_progress(line)

    def _on_log_line(self, line: str) -> None:
        # Reset the "last overwrite" memory whenever a real new line
        # lands so subsequent \r updates don't pad to a stale width.
        self._last_overwrite_text = ""
        self._log.appendPlainText(line)
        # Some writers emit progress on real new lines ("[10/100]…")
        # — same parser handles either form.
        self._maybe_update_progress(line)

    def _on_runner_started(self, argv_str: str) -> None:
        ts = time.strftime("%H:%M:%S")
        self._log.appendPlainText(f"── [{ts}] starting: {argv_str}")
        # Kick the progress bar into indeterminate mode (busy stripes)
        # — switches to a real percent as soon as a progress line lands.
        self._progress.setRange(0, 0)
        self._progress.setFormat("running…")

    def _on_runner_finished(self, exit_code: int) -> None:
        ts = time.strftime("%H:%M:%S")
        verdict = "ok" if exit_code == 0 else f"exit {exit_code}"
        self._log.appendPlainText(f"── [{ts}] finished: {verdict}")
        self._set_running(False)
        self._refresh_status_dots()
        # The writer (or rsync download) probably just produced new
        # files — refresh the preview and re-scan Data/ so any newly
        # downloaded run dir appears in the picker.
        self._refresh_runs()
        self._refresh_preview()
        # Park the progress bar back to "idle".  Range back to 0..100
        # so the next launch can switch to indeterminate cleanly.
        self._progress.setRange(0, 100)
        self._progress.setValue(100 if exit_code == 0 else 0)
        self._progress.setFormat("done" if exit_code == 0 else f"exit {exit_code}")


# ---------------------------------------------------------------------------
# Read-only run-info card for the Run Manager sidebar.
# ---------------------------------------------------------------------------


class _RunInfoCard(QtWidgets.QFrame):
    """Shows V_bias / beam / polarity / mirror / gas / aerogel for the
    selected run.  Read-only — sourced from the run database with
    forward-inheritance applied.

    Layout is operator-tuned rather than mechanical:
      - Beam is **one composite row** "energy · polarity · ●ON/OFF"
        because shifters read those three together, not separately.
      - Radiators get their own **grid** (one row per radiator, columns
        for type / n / tag / depth) — the old single-QLabel/\\n
        render was hard to scan.
    """

    # No "regular" fields any more — every interesting knob is bundled
    # into a composite row (Beam / Detector) or its own section
    # (Radiators).  Empty tuple here means the generic grid is never
    # populated, and we can keep the rendering single-grid + clean.
    _FIELDS: tuple[tuple[str, str, str], ...] = ()

    def __init__(
        self,
        parent: QtWidgets.QWidget | None = None,
        *,
        dashboard_config: Path | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        # Path to qa_quicklook.toml — for the V_bias × T band table.
        # The bands are reloaded on every refresh so the operator can
        # tune them in Settings and see the chip update without restart.
        self._dashboard_config = dashboard_config

        #  ── State tracked across show / edit / save ────────────────
        #  Edit mode and the editor-widget map.  We keep the last
        #  shown record + run_id so Cancel can rebuild the read-only
        #  view without re-fetching the database, and Save can
        #  compute the diff (only changed fields hit ``rundb``).
        self._edit_mode: bool = False
        self._current_record = None             # rundb.RunRecord | None
        self._current_run_id: Optional[str] = None
        self._in_db: bool = False
        self._editors: dict[str, QtWidgets.QWidget] = {}
        #  Parent-supplied save handler (set via set_save_callback).
        #  Signature: cb(run_id, changes, is_new_run, cascade).
        self._on_save_cb = None

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(12, 10, 12, 12)
        outer.setSpacing(6)

        # ── Header row: title + action buttons (Edit / Add / Save / Cancel)
        head_row = QtWidgets.QHBoxLayout()
        head_row.setContentsMargins(0, 0, 0, 0)
        head = QtWidgets.QLabel("Run info")
        head_font = head.font()
        head_font.setBold(True)
        head_font.setPointSize(head_font.pointSize() + 2)
        head.setFont(head_font)
        head_row.addWidget(head)
        head_row.addStretch(1)

        #  Three context-dependent buttons.  Only the relevant subset
        #  is visible at any time — visibility is the state-machine
        #  switch instead of swapping QPushButton instances.
        self._btn_edit = QtWidgets.QPushButton("✎ Edit")
        self._btn_edit.setToolTip("Edit Beam / Detector values in place.")
        self._btn_edit.clicked.connect(self._enter_edit_mode)
        head_row.addWidget(self._btn_edit)

        self._btn_add = QtWidgets.QPushButton("✚ Add to database")
        self._btn_add.setToolTip(
            "Create an entry for this run in the database and fill in "
            "its Beam / Detector values."
        )
        self._btn_add.clicked.connect(self._enter_edit_mode_new)
        head_row.addWidget(self._btn_add)

        self._btn_save = QtWidgets.QPushButton("✓ Save")
        self._btn_save.setStyleSheet("font-weight: 600;")
        self._btn_save.clicked.connect(self._on_save_clicked)
        head_row.addWidget(self._btn_save)

        self._btn_cancel = QtWidgets.QPushButton("✗ Cancel")
        self._btn_cancel.clicked.connect(self._on_cancel_clicked)
        head_row.addWidget(self._btn_cancel)

        outer.addLayout(head_row)

        self._title = QtWidgets.QLabel()
        self._title.setObjectName("muted")
        outer.addWidget(self._title)

        # ── Single tabular grid ─────────────────────────────────────
        # All sections (Beam, Detector, every Radiator row) share one
        # QGridLayout so the *value* columns line up across rows —
        # the operator reads the card as a real table, not three
        # independently-formatted lines.  Column scheme:
        #
        #   0 — section label (Beam / Detector / Radiators)
        #   1 — value 1 (energy        / V_bias    / type)
        #   2 — value 2 (polarity      / Δthr      / refindex)
        #   3 — value 3 (—             / temp      / tag)
        #   4 — value 4 (—             / —         / depth)
        #   5 — trailing widget (beam status dot; nothing else uses it)
        #
        # The section label only appears on the first row of each
        # multi-row section (only Radiators today); empty for repeats.
        self._grid_holder = QtWidgets.QWidget()
        self._grid = QtWidgets.QGridLayout(self._grid_holder)
        self._grid.setContentsMargins(0, 4, 0, 0)
        self._grid.setHorizontalSpacing(12)
        self._grid.setVerticalSpacing(4)
        # Label column at natural width; value columns at content;
        # trailing dot stays right-edge.  No stretch — the card sits
        # in a sidebar with finite width, and content-driven columns
        # give the cleanest visual.
        outer.addWidget(self._grid_holder)

        # Quick set-quality dropdown — writes back to the database
        # via rundb.update_run_field with auto_pin so downstream runs
        # don't silently shift.  Hidden when no run is selected.
        quality_row = QtWidgets.QHBoxLayout()
        quality_row.setContentsMargins(0, 6, 0, 0)
        quality_row.addWidget(QtWidgets.QLabel("Quality:"))
        self._quality_combo = QtWidgets.QComboBox()
        self._quality_combo.addItem("(unset)", "")
        for q in ("need QA", "good", "warning", "bad", "test", "delete"):
            self._quality_combo.addItem(q, q)
        self._quality_combo.currentTextChanged.connect(self._on_quality_changed)
        self._quality_suppress = False
        quality_row.addWidget(self._quality_combo, 1)
        outer.addLayout(quality_row)
        # Signals reach the parent via a callback set externally.
        self._on_quality_change_cb = None

        self.show_empty()

    def set_quality_callback(self, cb) -> None:
        """Wire the quality-dropdown change to a parent handler.

        Signature: ``cb(run_id: str, new_quality: str)``.
        """
        self._on_quality_change_cb = cb

    def set_save_callback(self, cb) -> None:
        """Wire the Save / Add buttons to a parent handler.

        Signature: ``cb(run_id, changes, is_new_run, cascade)``.

          - ``run_id``    : the run being edited.
          - ``changes``   : ``dict[str, object]`` of new values for
                            fields that actually differ from the
                            pre-edit state.
          - ``is_new_run``: True when the user pressed "Add to
                            database" (caller must ``append_runs``
                            first).
          - ``cascade``   : True ⇒ let downstream runs inherit the
                            new value via forward-inheritance
                            (``auto_pin=False``).  False ⇒ pin
                            downstream (``auto_pin=True``).  For new
                            runs this is meaningless — there's
                            nothing downstream yet — and the caller
                            should ignore it.
        """
        self._on_save_cb = cb

    def show_empty(self) -> None:
        self._edit_mode = False
        self._current_record = None
        self._current_run_id = None
        self._in_db = False
        self._title.setText("(no run selected)")
        self._rebuild_grid(
            beam=(None, None, None),
            detector=(None, None, None),
            radiators=None,
        )
        self._set_quality_silently("")
        self._update_button_visibility()

    def show_record(self, record, run_id: str) -> None:  # record: rundb.RunRecord | None
        #  Coming from disk / parent refresh — always exit edit mode
        #  so a background DB update from elsewhere can't quietly
        #  reset the user's in-progress edits.  Cancel is the only
        #  way to silently bail.
        self._edit_mode = False
        self._editors = {}
        self._current_record = record
        self._current_run_id = run_id
        self._in_db = record is not None

        if record is None:
            self._title.setText(f"{run_id}  ·  not in database")
            self._rebuild_grid(
                beam=(None, None, None),
                detector=(None, None, None),
                radiators=None,
            )
            self._set_quality_silently("")
            self._update_button_visibility()
            return

        self._title.setText(run_id)
        self._set_quality_silently(str(record.get("quality", "") or ""))
        self._current_run_id_for_quality = run_id
        merged = record.merged
        self._rebuild_grid(
            beam=(
                merged.get("beam_energy"),
                merged.get("polarity"),
                merged.get("beam_status"),
            ),
            detector=(
                merged.get("v_bias"),
                merged.get("deltathr"),
                merged.get("temperature"),
            ),
            radiators=merged.get("radiators"),
        )
        self._update_button_visibility()

    # ---- button state machine ------------------------------------------

    def _update_button_visibility(self) -> None:
        """Show only the buttons that apply to the current state.

        Three states:
          - no run selected     → all hidden
          - run in db, view     → Edit
          - run not in db, view → Add to database
          - any, edit mode      → Save + Cancel
        """
        has_run = self._current_run_id is not None
        editing = self._edit_mode
        self._btn_edit.setVisible(has_run and self._in_db and not editing)
        self._btn_add.setVisible(has_run and not self._in_db and not editing)
        self._btn_save.setVisible(has_run and editing)
        self._btn_cancel.setVisible(has_run and editing)

    # ---- edit-mode entry / exit ----------------------------------------

    def _enter_edit_mode(self) -> None:
        """Flip the card to editable mode pre-filled with current values."""
        if self._current_run_id is None:
            return
        self._edit_mode = True
        merged = self._current_record.merged if self._current_record else {}
        self._rebuild_grid_editing(
            beam_energy=merged.get("beam_energy"),
            polarity=merged.get("polarity"),
            beam_status=merged.get("beam_status"),
            v_bias=merged.get("v_bias"),
            deltathr=merged.get("deltathr"),
            temperature=merged.get("temperature"),
        )
        self._update_button_visibility()

    def _enter_edit_mode_new(self) -> None:
        """Flip the card to editable mode with all fields blank.

        Used by the "Add to database" path — the run isn't in the
        database yet, so there's nothing to pre-fill from.  Save
        will trigger ``append_runs`` + ``update_run_field`` per
        non-empty field.
        """
        if self._current_run_id is None:
            return
        self._edit_mode = True
        self._rebuild_grid_editing(
            beam_energy=None, polarity=None, beam_status=None,
            v_bias=None, deltathr=None, temperature=None,
        )
        self._update_button_visibility()

    def _on_cancel_clicked(self) -> None:
        """Drop the in-progress edits and re-render the read-only view."""
        if self._current_run_id is None:
            self.show_empty()
            return
        #  show_record resets edit_mode + button visibility itself.
        self.show_record(self._current_record, self._current_run_id)

    def _on_save_clicked(self) -> None:
        """Collect the diff vs current values and dispatch to the parent.

        For an existing run we ask once for the cascade behaviour
        (single dialog covering all changed fields).  For a new run
        the cascade question is meaningless — there's nothing
        downstream — so we just confirm the creation.
        """
        if self._current_run_id is None or self._on_save_cb is None:
            return
        changes = self._collect_changes()
        is_new = not self._in_db

        if not changes and is_new:
            #  Operator hit Save on an Add flow without filling
            #  anything in — create the empty run row anyway, since
            #  that's a meaningful action (forward-inheritance picks
            #  up everything from the previous run).
            ok = QtWidgets.QMessageBox.question(
                self, "Add run to database",
                f"Create empty entry for {self._current_run_id}?\n\n"
                "All fields will inherit from the previous run.",
            ) == QtWidgets.QMessageBox.Yes
            if not ok:
                return
            self._on_save_cb(self._current_run_id, {}, True, False)
            return

        if not changes:
            #  Existing run, no changes — just leave edit mode.
            self._on_cancel_clicked()
            return

        if is_new:
            preview = "\n".join(f"  {k} = {v}" for k, v in changes.items())
            confirm = QtWidgets.QMessageBox.question(
                self, "Add run to database",
                f"Create {self._current_run_id} with:\n\n{preview}",
            ) == QtWidgets.QMessageBox.Yes
            if not confirm:
                return
            self._on_save_cb(self._current_run_id, changes, True, False)
            return

        #  Existing run: ask cascade-or-pin.  Default is pin downstream
        #  (the safer choice — only this run changes), matching the
        #  existing quality-dropdown behaviour.
        cascade = self._prompt_cascade(changes)
        if cascade is None:
            return  # user cancelled
        self._on_save_cb(self._current_run_id, changes, False, cascade)

    def _prompt_cascade(self, changes: dict) -> Optional[bool]:
        """Ask the operator how the edits should propagate downstream.

        Returns ``True``  → cascade (let later runs inherit),
                ``False`` → single change (pin downstream so only this
                            run's merged view shifts),
                ``None``  → user cancelled.
        """
        return _show_cascade_dialog(
            self, self._current_run_id, changes, header=None,
        )

    # ---- collect / compare ---------------------------------------------

    def _collect_changes(self) -> dict:
        """Diff editor values against the pre-edit merged view.

        Returns the dict that needs to land in the database — only
        fields whose new value differs (and isn't blank when the
        old value was unset) appear.
        """
        merged = self._current_record.merged if self._current_record else {}
        out: dict[str, object] = {}
        for field, widget in self._editors.items():
            new_val = self._read_editor(widget)
            old_val = merged.get(field)
            #  Treat blank-string as "unset"; "unset → unset" is no
            #  change.  For numeric fields we coerce both sides to
            #  strings for the comparison (database stores them as
            #  TOML scalars but tomlkit returns python types that
            #  may be ints/floats while the editor produces strings).
            if new_val in (None, "") and old_val in (None, ""):
                continue
            if new_val == old_val:
                continue
            #  Float/int comparison: ``35.0`` vs ``35`` shouldn't
            #  trigger a write.  Try numeric comparison when both
            #  parse cleanly.
            if self._numeric_equal(new_val, old_val):
                continue
            out[field] = new_val
        return out

    @staticmethod
    def _numeric_equal(a, b) -> bool:
        #  Explicit None-check up front — relying on
        #  ``float(None) → TypeError → False`` worked but masked
        #  intent (a maintainer reading the body wouldn't know None
        #  was an expected input).  Either side being None ⇒ not a
        #  numeric match; let the textual equality path upstream
        #  catch "both unset" cases.
        if a is None or b is None:
            return False
        try:
            return float(a) == float(b)
        except (TypeError, ValueError):
            return False

    @staticmethod
    def _read_editor(widget: QtWidgets.QWidget):
        """Pull the typed value out of one editor cell.

        Number-like text gets coerced to float for stable round-trip
        through the database (matches existing rundb scalars).
        Bare strings (polarity / beam_status from a combobox) stay
        strings — TOML doesn't care.
        """
        if isinstance(widget, QtWidgets.QComboBox):
            #  Each item carries its real value in ``itemData`` —
            #  ``"(unset)"`` maps to ``""``, the real choices map to
            #  themselves.  This guards against the display string
            #  leaking into the database as a value.
            data = widget.currentData()
            if data is None or data == "":
                return None
            return str(data).strip() or None
        if isinstance(widget, QtWidgets.QLineEdit):
            text = widget.text().strip()
            if not text:
                return None
            try:
                f = float(text)
                #  Preserve ints as ints — tomlkit renders ``35`` vs
                #  ``35.0`` differently and the operator probably
                #  meant the simpler form.
                return int(f) if f.is_integer() else f
            except ValueError:
                return text
        return None

    # ---- edit-mode grid ------------------------------------------------

    def _rebuild_grid_editing(
        self,
        *,
        beam_energy, polarity, beam_status,
        v_bias, deltathr, temperature,
    ) -> None:
        """Symmetric to ``_rebuild_grid`` but renders editor widgets.

        Same column scheme (label · v1 · v2 · v3 · v4 · trailing) so
        the editable view drops into the same visual ruler as the
        read-only one.  Radiators stay read-only in v1 — editing a
        list of dicts inline is a bigger UX problem (master-detail,
        add/remove rows) and would dwarf this card.
        """
        # Wipe.
        while self._grid.count():
            item = self._grid.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()
        self._editors = {}

        def section_label(text: str) -> QtWidgets.QLabel:
            lab = QtWidgets.QLabel(text)
            lab.setStyleSheet("font-weight: 600;")
            return lab

        def line_edit(initial, placeholder: str, *, kind: str = "float") -> QtWidgets.QLineEdit:
            le = QtWidgets.QLineEdit()
            if initial not in (None, ""):
                le.setText(str(initial))
            le.setPlaceholderText(placeholder)
            le.setMaximumWidth(110)
            if kind == "float":
                v = QtGui.QDoubleValidator(-1e9, 1e9, 6, le)
                v.setNotation(QtGui.QDoubleValidator.StandardNotation)
                v.setLocale(QtCore.QLocale.c())
                le.setValidator(v)
            return le

        def enum_combo(initial, choices: tuple[str, ...]) -> QtWidgets.QComboBox:
            cb = QtWidgets.QComboBox()
            cb.addItem("(unset)", "")
            for c in choices:
                cb.addItem(c, c)
            if initial:
                idx = cb.findText(str(initial))
                cb.setCurrentIndex(idx if idx >= 0 else 0)
            cb.setMaximumWidth(110)
            return cb

        row = 0

        # ── Beam ───────────────────────────────────────────────────
        self._grid.addWidget(section_label("Beam"), row, 0)
        le_energy = line_edit(beam_energy, "GeV")
        self._editors["beam_energy"] = le_energy
        self._grid.addWidget(le_energy, row, 1)

        cb_pol = enum_combo(polarity, ("pos", "neg"))
        self._editors["polarity"] = cb_pol
        self._grid.addWidget(cb_pol, row, 2)

        cb_status = enum_combo(beam_status, ("ON", "OFF"))
        self._editors["beam_status"] = cb_status
        self._grid.addWidget(cb_status, row, 3)
        row += 1

        # ── Detector ───────────────────────────────────────────────
        self._grid.addWidget(section_label("Detector"), row, 0)
        le_vbias = line_edit(v_bias, "V")
        self._editors["v_bias"] = le_vbias
        self._grid.addWidget(le_vbias, row, 1)

        le_dthr = line_edit(deltathr, "Δthr", kind="any")
        self._editors["deltathr"] = le_dthr
        self._grid.addWidget(le_dthr, row, 2)

        le_temp = line_edit(temperature, "°C")
        self._editors["temperature"] = le_temp
        self._grid.addWidget(le_temp, row, 3)
        row += 1

        # ── Radiators (read-only in v1) ────────────────────────────
        merged = self._current_record.merged if self._current_record else {}
        radiators = merged.get("radiators") if merged else None
        self._grid.addWidget(section_label("Radiators"), row, 0)
        if not radiators:
            lab = QtWidgets.QLabel("(read-only in edit mode)")
            lab.setObjectName("muted")
            self._grid.addWidget(lab, row, 1, 1, 4)
            return
        for i, r in enumerate(radiators):
            if i > 0:
                #  Leave col 0 blank on subsequent rows so the
                #  "Radiators" label only anchors the block.
                pass
            text = str(r) if not isinstance(r, dict) else \
                " · ".join(f"{k}={v}" for k, v in r.items())
            lab = QtWidgets.QLabel(text)
            lab.setObjectName("muted")
            self._grid.addWidget(lab, row, 1, 1, 4)
            row += 1

    # ---- shared tabular grid -------------------------------------------

    def _rebuild_grid(
        self,
        *,
        beam: tuple[object, object, object],
        detector: tuple[object, object, object],
        radiators,
    ) -> None:
        """Lay Beam / Detector / Radiators into one shared QGridLayout.

        Wipes the grid and rebuilds — cheap (≤ ~12 small QLabels per
        refresh).  Cells use the same set of columns across rows so
        Beam's "polarity" lines up under Detector's "Δthr" lines up
        under each Radiator's "n=…", etc.  That's the tabular feel
        the operator asked for: every section reads off the same
        column ruler.
        """
        # Wipe.
        while self._grid.count():
            item = self._grid.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()

        mono = QtGui.QFont("Menlo")
        mono.setStyleHint(QtGui.QFont.Monospace)

        def section_label(text: str) -> QtWidgets.QLabel:
            lab = QtWidgets.QLabel(text)
            lab.setStyleSheet("font-weight: 600;")
            return lab

        def value(text: str, *, dim: bool = False) -> QtWidgets.QLabel:
            lab = QtWidgets.QLabel(text)
            lab.setFont(mono)
            if dim:
                lab.setObjectName("muted")
            return lab

        row = 0

        # ── Beam ───────────────────────────────────────────────────
        energy, polarity, status = beam
        self._grid.addWidget(section_label("Beam"), row, 0)
        self._grid.addWidget(
            value(f"{energy} GeV" if energy not in (None, "") else "— GeV"),
            row, 1,
        )
        self._grid.addWidget(
            value(str(polarity) if polarity not in (None, "") else "—"),
            row, 2,
        )
        # Status dot in the trailing column.
        dot, tip = self._beam_dot_for(status)
        dot_lab = QtWidgets.QLabel("●")
        dot_lab.setStyleSheet(f"color: {dot}; font-size: 14px;")
        dot_lab.setToolTip(tip)
        self._grid.addWidget(dot_lab, row, 5, QtCore.Qt.AlignVCenter)
        row += 1

        # ── Detector ───────────────────────────────────────────────
        v_bias, deltathr, temperature = detector
        self._grid.addWidget(section_label("Detector"), row, 0)
        self._grid.addWidget(
            value(f"{v_bias} V" if v_bias not in (None, "") else "— V"),
            row, 1,
        )
        self._grid.addWidget(
            value(
                f"Δthr {deltathr}" if deltathr not in (None, "") else "Δthr —"
            ),
            row, 2,
        )
        self._grid.addWidget(
            value(
                f"{temperature} °C" if temperature not in (None, "") else "— °C"
            ),
            row, 3,
        )
        # V_bias × T sanity chip — green ✓ / amber ⚠ / grey ? per band
        # table in qa_quicklook.toml.  Sits in the trailing column so
        # it lines up under the Beam row's status dot.
        chip = self._vbias_chip(v_bias, temperature)
        if chip is not None:
            self._grid.addWidget(chip, row, 5, QtCore.Qt.AlignVCenter)
        row += 1

        # ── Radiators ──────────────────────────────────────────────
        # Section label only on the first radiator row; subsequent
        # rows leave col 0 empty so the radiators read as a continued
        # block, not three separate entries.
        if not radiators:
            self._grid.addWidget(section_label("Radiators"), row, 0)
            empty = QtWidgets.QLabel("(none)")
            empty.setObjectName("muted")
            self._grid.addWidget(empty, row, 1, 1, 4)
            row += 1
            return

        for i, r in enumerate(radiators):
            if i == 0:
                self._grid.addWidget(
                    section_label("Radiators"), row, 0, QtCore.Qt.AlignTop,
                )
            if not isinstance(r, dict):
                # Defensive — old / hand-edited entries might be a
                # bare string.  Span the value cells.
                lab = QtWidgets.QLabel(str(r))
                lab.setObjectName("muted")
                self._grid.addWidget(lab, row, 1, 1, 4)
                row += 1
                continue
            # Col 1: type (bold anchor for the row).
            kind = QtWidgets.QLabel(str(r.get("type", "?")))
            kind.setStyleSheet("font-weight: 600;")
            self._grid.addWidget(kind, row, 1)
            # Col 2: refindex "n=…".
            if "refindex" in r:
                self._grid.addWidget(value(f"n={r['refindex']}"), row, 2)
            # Col 3: tag.
            if "tag" in r:
                self._grid.addWidget(value(str(r["tag"]), dim=True), row, 3)
            # Col 4: depth / thickness.
            depth = r.get("depth", r.get("thickness"))
            if depth is not None:
                self._grid.addWidget(value(f"{depth} mm"), row, 4)
            row += 1

    def _vbias_chip(self, v_bias, temperature) -> Optional[QtWidgets.QLabel]:
        """Build the green/amber/grey V_bias × T sanity chip.

        Returns ``None`` when no chip should be shown — either we
        have no V_bias / T at all (nothing to opine on), or the
        dashboard-config path isn't set.  The "no bands configured"
        case still renders a grey "?" so the operator knows the check
        exists but isn't producing a verdict.
        """
        if self._dashboard_config is None:
            return None
        if v_bias in (None, "") and temperature in (None, ""):
            return None
        bands = sanity.load_bands(self._dashboard_config)
        result = sanity.check_vbias(v_bias, temperature, bands)
        if result.level == "ok":
            text, colour = "✓", "#0BDA51"   # green
        elif result.level == "warn":
            text, colour = "⚠", "#E0A40A"   # amber
        else:  # unknown
            text, colour = "?", "#6B6968"   # muted grey
        chip = QtWidgets.QLabel(text)
        chip.setStyleSheet(
            f"color: {colour}; font-weight: 700; font-size: 14px;"
        )
        chip.setToolTip(result.message)
        return chip

    @staticmethod
    def _beam_dot_for(status) -> tuple[str, str]:
        """Map a status value to (colour, tooltip) for the status dot."""
        s = (str(status).strip().lower() if status is not None else "")
        if s in ("on", "true", "1", "yes"):
            return "#0BDA51", f"Beam status: {status}"
        if s in ("off", "false", "0", "no"):
            return "#FF6B6B", f"Beam status: {status}"
        if not s:
            return "#6B6968", "Beam status: unknown"
        # E.g. "ramping" — amber + the raw value.
        return "#E0A40A", f"Beam status: {status}"


    def _set_quality_silently(self, q: str) -> None:
        self._quality_suppress = True
        try:
            idx = self._quality_combo.findData(q)
            if idx < 0:
                idx = 0  # unset
            self._quality_combo.setCurrentIndex(idx)
        finally:
            self._quality_suppress = False

    def _on_quality_changed(self, _text: str) -> None:
        if self._quality_suppress or self._on_quality_change_cb is None:
            return
        new = self._quality_combo.currentData()
        run_id = getattr(self, "_current_run_id_for_quality", None)
        if not run_id:
            return
        self._on_quality_change_cb(run_id, new or "")


# ---------------------------------------------------------------------------
# Active-runs panel — every writer currently RUNNING (per joblock)
# with a per-run Stop button.  Includes runs spawned by other
# dashboard instances since lock files are the source of truth.
# ---------------------------------------------------------------------------


class _ActiveRunsPanel(QtWidgets.QFrame):
    def __init__(self, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        # Log dock is wired in *after* construction (the RunManagerView
        # builds the log dock later in its own __init__ and calls
        # ``set_log_dock`` to inject it here).
        self._log: QtWidgets.QPlainTextEdit | None = None

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(12, 10, 12, 12)
        outer.setSpacing(6)

        head = QtWidgets.QLabel("Active runs")
        head_font = head.font()
        head_font.setBold(True)
        head_font.setPointSize(head_font.pointSize() + 1)
        head.setFont(head_font)
        outer.addWidget(head)

        self._caption = QtWidgets.QLabel("(none running)")
        self._caption.setObjectName("muted")
        outer.addWidget(self._caption)

        self._rows_holder = QtWidgets.QWidget()
        self._rows = QtWidgets.QVBoxLayout(self._rows_holder)
        self._rows.setContentsMargins(0, 4, 0, 0)
        self._rows.setSpacing(4)
        outer.addWidget(self._rows_holder)

        self.refresh()

    def set_log_dock(self, dock: QtWidgets.QPlainTextEdit) -> None:
        self._log = dock

    def _log_line(self, text: str) -> None:
        if self._log is not None:
            self._log.appendPlainText(text)

    def refresh(self) -> None:
        # Clear existing rows.
        while self._rows.count():
            item = self._rows.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()

        running = [
            lock for lock in joblock.list_locks()
            if joblock.effective_state(lock) == joblock.EFFECTIVE_RUNNING
        ]
        #  Most-recently-started job at the top, matching the
        #  newest-first convention used by every other run-list in
        #  the dashboard.
        running.sort(key=lambda l: l.started_at or "", reverse=True)
        if not running:
            self._caption.setText("(none running)")
            return
        self._caption.setText(f"{len(running)} writer(s) running")
        for lock in running:
            self._rows.addWidget(self._build_row(lock))

    def _build_row(self, lock: joblock.JobLock) -> QtWidgets.QWidget:
        row = QtWidgets.QFrame()
        h = QtWidgets.QHBoxLayout(row)
        h.setContentsMargins(2, 2, 2, 2)
        h.setSpacing(8)

        dot = QtWidgets.QLabel("●")
        dot.setStyleSheet(f"color: {theme.palette().success}; font-size: 14px;")
        h.addWidget(dot)

        label = QtWidgets.QLabel(f"<b>{lock.writer}</b>  ·  {lock.run}")
        h.addWidget(label)

        elapsed = QtWidgets.QLabel(_elapsed_since(lock.started_at))
        elapsed.setObjectName("muted")
        elapsed.setStyleSheet("font-size: 11px;")
        h.addWidget(elapsed)
        h.addStretch(1)

        stop_btn = QtWidgets.QPushButton("Stop")
        stop_btn.setObjectName("dangerButton")
        stop_btn.clicked.connect(lambda _checked=False, lk=lock: self._stop_lock(lk))
        h.addWidget(stop_btn)
        return row

    def _stop_lock(self, lock: joblock.JobLock) -> None:
        confirm = QtWidgets.QMessageBox.question(
            self, "Stop writer",
            f"Send SIGTERM to {lock.writer} (PID {lock.pid}) on run {lock.run}?",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
        )
        if confirm != QtWidgets.QMessageBox.Yes:
            return
        try:
            import signal as _sig
            os.kill(lock.pid, _sig.SIGTERM)
        except ProcessLookupError:
            self._log_line(f"[active] PID {lock.pid} already gone — refreshing")
        except Exception as exc:  # noqa: BLE001
            self._log_line(f"[active] stop failed: {exc}")
            return
        self._log_line(
            f"[active] SIGTERM → {lock.writer} (PID {lock.pid}) on {lock.run}"
        )
        # Update lock so other dashboards know we asked it to stop.
        joblock.update_lock(
            lock.writer, lock.run,
            state=joblock.EFFECTIVE_KILLED,
            finished_at=joblock.now_iso(),
        )
        self.refresh()


# ---------------------------------------------------------------------------
# Remote-runs picker — table view over the DAQ host's run directory,
# so an operator can pick from "what's available now" instead of typing
# a run id from memory.  Single-select; multi-select can graduate
# later when batch-download is on the table.
# ---------------------------------------------------------------------------


class _RemoteRunsPickerDialog(QtWidgets.QDialog):
    """Browse the DAQ host's run directory and pick one to rsync.

    Three exit paths the caller distinguishes:

      - ``Accept`` with a row selected → ``selected_run_id`` is the
        picked id, ``manual_entry`` is False.
      - ``Type id manually…`` button → ``manual_entry`` is True and
        the caller should fall back to its ``QInputDialog.getText``
        flow (the dialog also accepts).  Provided as an escape
        hatch when the listing path fails or the operator already
        knows the id.
      - ``Cancel`` / dialog rejected → caller treats as user-abort.

    The listing call is synchronous (``processEvents`` while it
    runs) — matches the existing ``probe_ssh_keyauth`` pattern in
    ``_prompt_for_rsync_address`` and keeps the dialog state
    machine simple.  The 20 s hard timeout in ``list_remote_runs``
    bounds the worst-case GUI freeze.
    """

    def __init__(
        self,
        cfg: "_download.RsyncConfig",
        repo_root: Path,
        *,
        default_run: str = "",
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._cfg = cfg
        self._repo_root = repo_root
        self._default_run = default_run
        self.selected_run_id: str = ""
        self.manual_entry: bool = False

        self.setWindowTitle(f"Available runs on {cfg.remote_host}")
        self.resize(560, 480)

        outer = QtWidgets.QVBoxLayout(self)
        outer.setSpacing(8)

        intro = QtWidgets.QLabel(
            f"Listing <code>{cfg.remote_data_dir.rstrip('/')}/</code> "
            f"on <code>{cfg.remote_host}</code>.  "
            "Already-mirrored runs are tagged so you don't re-download "
            "them by accident.",
            self,
        )
        intro.setWordWrap(True)
        intro.setTextFormat(QtCore.Qt.RichText)
        outer.addWidget(intro)

        #  Filter + refresh row.  Filter does substring-match on the
        #  run id; refresh re-hits ssh.  Both leave the existing
        #  selection alone when the row survives the new view.
        controls = QtWidgets.QHBoxLayout()
        self._filter = QtWidgets.QLineEdit(self)
        self._filter.setPlaceholderText("filter (substring on run id)")
        self._filter.textChanged.connect(self._apply_filter)
        controls.addWidget(self._filter, 1)
        self._refresh_btn = QtWidgets.QPushButton("Refresh", self)
        self._refresh_btn.clicked.connect(self._refresh)
        controls.addWidget(self._refresh_btn)
        outer.addLayout(controls)

        #  Status line — count of entries + last error, both lit up
        #  by ``_refresh``.  Lives above the table so a 0-row error
        #  is impossible to miss.
        self._status = QtWidgets.QLabel("…fetching…", self)
        self._status.setObjectName("muted")
        self._status.setWordWrap(True)
        outer.addWidget(self._status)

        self._table = QtWidgets.QTableWidget(0, 3, self)
        self._table.setHorizontalHeaderLabels(["Run id", "Modified (local)", "Status"])
        self._table.setSelectionBehavior(QtWidgets.QTableWidget.SelectRows)
        self._table.setSelectionMode(QtWidgets.QTableWidget.SingleSelection)
        self._table.setEditTriggers(QtWidgets.QTableWidget.NoEditTriggers)
        self._table.verticalHeader().setVisible(False)
        hdr = self._table.horizontalHeader()
        hdr.setSectionResizeMode(0, QtWidgets.QHeaderView.ResizeToContents)
        hdr.setSectionResizeMode(1, QtWidgets.QHeaderView.ResizeToContents)
        hdr.setSectionResizeMode(2, QtWidgets.QHeaderView.Stretch)
        #  Double-click on a row = OK with that row selected.  Same
        #  affordance as every other picker in the dashboard.
        self._table.itemDoubleClicked.connect(self._on_accept)
        outer.addWidget(self._table, 1)

        #  Live-run guard — when the user clicks the newest row, this
        #  checkbox appears and the OK button stays disabled until it's
        #  ticked.  Hidden for any other row (older runs are sealed by
        #  construction).  ``_sync_ok_enabled`` controls visibility.
        self._live_ack_check = QtWidgets.QCheckBox(
            "I confirm data-taking has STOPPED on this run "
            "(downloading a still-active run gives truncated files)",
            self,
        )
        self._live_ack_check.setStyleSheet(
            "QCheckBox { color: #E07B00; font-weight: 600; }"
        )
        self._live_ack_check.setVisible(False)
        self._live_ack_check.toggled.connect(self._sync_ok_enabled)
        outer.addWidget(self._live_ack_check)

        #  Buttons.  "Type id manually…" sits on the left so it
        #  doesn't read as a destructive alternative to Cancel.
        btn_row = QtWidgets.QHBoxLayout()
        self._manual_btn = QtWidgets.QPushButton("Type id manually…", self)
        self._manual_btn.setToolTip(
            "Skip the listing and enter a run id directly.  Useful "
            "when the SSH listing is slow or you already know the id."
        )
        self._manual_btn.clicked.connect(self._on_manual)
        btn_row.addWidget(self._manual_btn)
        btn_row.addStretch(1)
        buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel,
            QtCore.Qt.Horizontal,
            self,
        )
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        self._ok_btn = buttons.button(QtWidgets.QDialogButtonBox.Ok)
        self._ok_btn.setEnabled(False)
        self._table.itemSelectionChanged.connect(self._sync_ok_enabled)
        btn_row.addWidget(buttons)
        outer.addLayout(btn_row)

        #  Defer the first refresh to the next event-loop tick so the
        #  dialog paints fully before we start blocking on ssh.
        QtCore.QTimer.singleShot(0, self._refresh)

    # ----- listing --------------------------------------------------

    def _refresh(self) -> None:
        self._refresh_btn.setEnabled(False)
        self._status.setText(
            f"…fetching listing from {self._cfg.remote_host}…"
        )
        QtWidgets.QApplication.processEvents()
        try:
            entries, err = _download.list_remote_runs(
                self._cfg, self._repo_root,
            )
        finally:
            self._refresh_btn.setEnabled(True)
        self._all_entries = entries
        if err:
            #  Show the error but leave whatever table contents
            #  survived from the previous refresh in place — operator
            #  can still pick from a stale view if the SSH path
            #  flapped.
            self._status.setText(
                f"<span style='color:#FF6B6B;'>listing failed:</span> {err}"
            )
            self._status.setTextFormat(QtCore.Qt.RichText)
        else:
            local_n = sum(1 for e in entries if e.local_present)
            ts = time.strftime("%H:%M:%S")
            self._status.setText(
                f"{len(entries)} run(s) — {local_n} already local · "
                f"fetched at {ts}"
            )
            self._status.setTextFormat(QtCore.Qt.PlainText)
        self._apply_filter(self._filter.text())

    def _apply_filter(self, needle: str) -> None:
        needle = (needle or "").strip().lower()
        rows = [
            e for e in getattr(self, "_all_entries", [])
            if not needle or needle in e.run_id.lower()
        ]
        self._table.setRowCount(len(rows))
        default_selected_row = -1
        #  The lex-max run id in the FULL listing (not the filtered
        #  view) is the "newest" — might still be acquiring.  Computed
        #  via download.newest_run_id so the picker + the live monitor
        #  agree on "what counts as the latest".
        self._newest_remote_id = _download.newest_run_id(
            getattr(self, "_all_entries", [])
        )
        for r, entry in enumerate(rows):
            is_newest = (entry.run_id == self._newest_remote_id)
            #  Tag the newest row with a visible "LIVE?" marker — the
            #  Unicode dot prefix is intentionally loud so the operator
            #  can't miss it scanning the list.
            id_text = f"🔴 {entry.run_id}" if is_newest else entry.run_id
            id_item = QtWidgets.QTableWidgetItem(id_text)
            id_item.setData(QtCore.Qt.UserRole, entry.run_id)
            mtime_item = QtWidgets.QTableWidgetItem(entry.mtime_iso or "—")
            if is_newest:
                status_text = "latest — may still be acquiring"
            elif entry.local_present:
                status_text = "✓ local"
            else:
                status_text = "remote only"
            status_item = QtWidgets.QTableWidgetItem(status_text)
            if entry.local_present and not is_newest:
                #  Muted so it reads as "already done", not as a
                #  warning.  Same tone as the Active runs panel's
                #  ``muted`` style.  Newest stays at full intensity
                #  even when local — the LIVE? warning trumps the
                #  "already done" cue.
                muted = QtGui.QBrush(QtGui.QColor("#9B8E8E"))
                for it in (id_item, mtime_item, status_item):
                    it.setForeground(muted)
            if is_newest:
                #  Distinct foreground for the newest row so the warning
                #  reads at a glance, paired with a tooltip explaining
                #  the live-run-guard mechanism.
                warn = QtGui.QBrush(QtGui.QColor("#E07B00"))
                for it in (id_item, mtime_item, status_item):
                    it.setForeground(warn)
                    it.setToolTip(
                        "Newest run on the remote — may still be acquiring. "
                        "If you really want this run, tick the acknowledgement "
                        "checkbox below before OK enables."
                    )
            self._table.setItem(r, 0, id_item)
            self._table.setItem(r, 1, mtime_item)
            self._table.setItem(r, 2, status_item)
            if entry.run_id == self._default_run:
                default_selected_row = r
        if default_selected_row >= 0:
            self._table.selectRow(default_selected_row)
        self._sync_ok_enabled()

    def _sync_ok_enabled(self) -> None:
        items = self._table.selectedItems()
        has_selection = len(items) > 0
        #  Live-run guard: if the selected row IS the newest on the
        #  remote, the shifter must tick the "data-taking is finished"
        #  acknowledgement before OK enables.  For any other row OK
        #  enables on selection as before — older runs are sealed by
        #  construction.
        selected_run = None
        if has_selection:
            id_item = self._table.item(items[0].row(), 0)
            if id_item is not None:
                selected_run = id_item.data(QtCore.Qt.UserRole)
        newest_id = getattr(self, "_newest_remote_id", None)
        is_newest_selected = (
            selected_run is not None and selected_run == newest_id
        )
        ack = getattr(self, "_live_ack_check", None)
        if ack is not None:
            ack.setVisible(is_newest_selected)
        if is_newest_selected and ack is not None:
            self._ok_btn.setEnabled(has_selection and ack.isChecked())
        else:
            self._ok_btn.setEnabled(has_selection)

    # ----- button handlers ------------------------------------------

    def _on_manual(self) -> None:
        self.manual_entry = True
        self.accept()

    def _on_accept(self, *_args) -> None:
        items = self._table.selectedItems()
        if not items:
            return
        row = items[0].row()
        id_item = self._table.item(row, 0)
        if id_item is None:
            return
        self.selected_run_id = id_item.data(QtCore.Qt.UserRole) or id_item.text()
        self.accept()


__all__ = ["RunManagerView"]
