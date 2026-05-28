"""Runlist tab — read-only scaffold (v1).

Left:  filterable list of every run id in
       ``run-lists/2025.database.toml``.
Right: runcard showing the **merged** view (own + forward-inherited)
       for the selected run, with a visual hint for inherited fields.

Edit semantics, auto-pin on inheritance changes, "Detect runs" import
from ``Data/``, tag search, and "save selection as runlist" are
queued in the design docs and ship in later iterations.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from PySide6 import QtCore, QtGui, QtWidgets

from . import rundb
from .dbworker import DbWorker
from .toml_form import _prettify_key


# Per-field dropdown choices.  Anything not listed renders as a
# free-text input (or a checkbox for bools).
RUNFIELD_ENUMS: dict[str, tuple[str, ...]] = {
    "polarity": ("pos", "neg"),
    "quality":  ("need QA", "good", "bad", "warning", "test", "delete"),
    # ALCOR operating modes the operator picks between; if a value
    # outside the enum shows up it's surfaced as a disabled extra
    # entry so the operator still sees it.
    "opmode":   ("0", "1", "2", "3"),
}

# Per-field units shown to the right of the editor.
RUNFIELD_UNITS: dict[str, str] = {
    "v_bias":      "V",
    "beam_energy": "GeV",
    "temperature": "°C",
    "mirror_mm":   "mm",
    "n_spills":    "spills",
    "deltathr":    "thr",
}

# Quality-tag → colour for the left-pane list.
QUALITY_COLOURS: dict[str, str] = {
    "good":    "#0BDA51",   # green
    "warning": "#E0A40A",   # amber
    "bad":     "#FF6B6B",   # coral
    "need QA": "#AFDBF5",   # sky
    "test":    "#9B8E8E",   # warm grey
    "delete":  "#6B6968",   # muted
}


# Canonical display order for the runcard skeleton.  Anything in the
# schema but absent from this tuple comes after, sorted alphabetically.
# ``notes`` is special-cased and rendered as a wide text area anchored
# at the bottom of the card.
FIELD_ORDER: tuple[str, ...] = (
    "v_bias", "beam_energy", "polarity", "beam_status",
    "mirror_mm", "temperature", "opmode", "deltathr",
    "n_spills", "quality",
)
NOTES_FIELD = "notes"
# Special-cased fields that get their own dedicated rendering instead
# of the generic label/editor/unit/origin grid cell.
RADIATORS_FIELD = "radiators"


def _schema_keys(records: list["rundb.RunRecord"]) -> set[str]:
    """Union of every field name that appears in any run's own keys.

    Augmented with the curated registries + ``FIELD_ORDER`` so the
    skeleton stays consistent even when a brand-new field hasn't been
    written on any run yet (the user still sees an empty slot for it).
    """
    seen: set[str] = set()
    for r in records:
        seen.update(r.own_fields.keys())
    seen.update(FIELD_ORDER)
    seen.update(RUNFIELD_ENUMS.keys())
    seen.update(RUNFIELD_UNITS.keys())
    seen.add(NOTES_FIELD)
    return seen


def _ordered_keys(schema: set[str]) -> list[str]:
    """Canonical order for the generic field grid.

    ``FIELD_ORDER`` first, rest alphabetical.  Skips fields that get
    their own dedicated rendering (``notes``, ``radiators``) so they
    don't show up twice in the runcard.
    """
    skip = {NOTES_FIELD, RADIATORS_FIELD}
    order = [k for k in FIELD_ORDER if k in schema and k not in skip]
    rest = sorted(k for k in schema if k not in FIELD_ORDER and k not in skip)
    return order + rest


class RunlistView(QtWidgets.QWidget):
    """Runlist tab."""

    def __init__(
        self,
        database_path: Path,
        parent: QtWidgets.QWidget | None = None,
        *,
        data_dir: Path | None = None,
        runlists_path: Path | None = None,
        db_worker: DbWorker | None = None,
    ) -> None:
        super().__init__(parent)
        self._db_path = database_path
        # Directory the picker scans for sibling databases (one per
        # year / beam test).  Defaults to the parent of the passed-in
        # database; can be overridden later if we add a global setting.
        self._database_dir = database_path.parent
        self._data_dir = data_dir
        self._runlists_path_override = runlists_path
        self._runlists_path = self._derive_runlists_path(database_path, runlists_path)
        # Background worker for blocking rundb writes.  Constructed
        # if the parent didn't share one — but the MainWindow normally
        # builds a single worker and passes it to every tab so writes
        # serialise across the whole dashboard.
        self._db_worker = db_worker if db_worker is not None else DbWorker(self)
        self._db_worker.done.connect(self._on_db_worker_done)
        # Tracks the last edit we sent so we can revert on failure.
        # tag → (run_id, field, old_value, kind)
        self._pending_edits: dict[str, tuple[str, str, object, str]] = {}
        self._records: list[rundb.RunRecord] = []
        # Union of every field name seen across all runs — drives the
        # consistent runcard skeleton.  Recomputed on each reload().
        self._schema: set[str] = set()
        self._extra_fields: set[str] = set()
        # Tracks which field matched the current filter for the
        # selected record — used to tint that field in the runcard.
        self._matched_field: Optional[str] = None
        self._active_needle: str = ""

        outer = QtWidgets.QHBoxLayout(self)
        outer.setContentsMargins(10, 10, 10, 10)
        outer.setSpacing(10)

        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        splitter.setChildrenCollapsible(False)

        # Left pane: database picker + filter + table + actions.
        left = QtWidgets.QWidget()
        left_layout = QtWidgets.QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 6, 0)
        left_layout.setSpacing(4)

        # ── Database picker ─────────────────────────────────────────
        # Scans the database dir for any *.database.toml so the
        # operator can swap between e.g. 2024 / 2025 / next-year
        # without restarting.  Scoped to the Database tab — the Run
        # Manager keeps its passed-in database for now (cross-tab
        # globalisation can come later if needed).
        db_row = QtWidgets.QHBoxLayout()
        db_row.setSpacing(6)
        db_label = QtWidgets.QLabel("Database:")
        db_label.setObjectName("muted")
        db_row.addWidget(db_label)
        self._db_combo = QtWidgets.QComboBox()
        self._db_combo.setToolTip(
            "Switch among run-lists/*.database.toml.  Typically one per "
            "year / beam test."
        )
        self._db_combo.currentIndexChanged.connect(self._on_database_picked)
        db_row.addWidget(self._db_combo, 1)
        left_layout.addLayout(db_row)
        # Populate before wiring the rest so reload() picks up the
        # right path.
        self._refresh_database_picker()

        # ── Filter row: field-scope picker + text input ─────────────
        # "any" (default) keeps the original cross-field match.
        # Picking a specific field scopes the substring search to
        # that field's value — useful for e.g. "all v_bias = 35"
        # without accidentally matching "deltathr=35" too.
        filter_row = QtWidgets.QHBoxLayout()
        filter_row.setSpacing(6)
        self._field_combo = QtWidgets.QComboBox()
        self._field_combo.setToolTip(
            "Limit the filter to a single field, or 'any' to search all."
        )
        self._field_combo.addItem("any field", "")
        # Populated with real field names on each reload (so newly-
        # added schema-extras land in the picker too).  Seed with
        # run id + the curated registries so the picker is useful
        # even before reload() runs.
        for fname in ("run_id",) + FIELD_ORDER + tuple(RUNFIELD_ENUMS.keys()):
            if self._field_combo.findData(fname) < 0:
                self._field_combo.addItem(fname, fname)
        self._field_combo.currentIndexChanged.connect(
            lambda _i: self._schedule_filter_refresh()
        )
        filter_row.addWidget(self._field_combo, 0)
        self._filter = QtWidgets.QLineEdit()
        self._filter.setPlaceholderText("filter (substring match)")
        # Debounce so a fast 5-char delete doesn't trigger 5 table
        # rebuilds — single 150 ms timer fires on idle.
        self._filter_timer = QtCore.QTimer(self)
        self._filter_timer.setSingleShot(True)
        self._filter_timer.setInterval(150)
        self._filter_timer.timeout.connect(self._refresh_list)
        self._filter.textChanged.connect(self._schedule_filter_refresh)
        filter_row.addWidget(self._filter, 1)
        left_layout.addLayout(filter_row)

        # Match-count caption — surfaces "12 of 257 matched" so the
        # operator can see the filter is actually doing something.
        self._match_caption = QtWidgets.QLabel()
        self._match_caption.setObjectName("muted")
        self._match_caption.setStyleSheet("font-size: 11px;")
        left_layout.addWidget(self._match_caption)

        # ── Run table ───────────────────────────────────────────────
        # 3 columns: Run id · Quality (colored chip) · Spills.
        # Alternating row colours, no grid, headers hidden — reads
        # as a clean compact table.  Column widths are content-driven;
        # quality + spills are narrow so the run-id always shows in full.
        self._table = QtWidgets.QTableWidget(0, 3)
        self._table.setHorizontalHeaderLabels(["Run", "Quality", "Spills"])
        # Header visible so the column meaning is self-evident at a
        # glance — the columns are now stable enough (run/quality/
        # spills) that the labels are useful context, not noise.
        hh_top = self._table.horizontalHeader()
        hh_top.setVisible(True)
        hh_top.setDefaultAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
        hh_top.setHighlightSections(False)
        self._table.verticalHeader().setVisible(False)
        self._table.setShowGrid(False)
        self._table.setAlternatingRowColors(True)
        self._table.setSelectionBehavior(
            QtWidgets.QAbstractItemView.SelectRows
        )
        self._table.setSelectionMode(
            QtWidgets.QAbstractItemView.ExtendedSelection
        )
        self._table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self._table.setFocusPolicy(QtCore.Qt.StrongFocus)
        # Sizing: quality + spills get just enough; run-id stretches.
        hh = self._table.horizontalHeader()
        hh.setSectionResizeMode(0, QtWidgets.QHeaderView.Stretch)
        hh.setSectionResizeMode(1, QtWidgets.QHeaderView.Fixed)
        hh.setSectionResizeMode(2, QtWidgets.QHeaderView.ResizeToContents)
        # Reserve enough room for the widest chip text ("warning",
        # "need QA") plus its rounded-rect padding.  Content-resize
        # was clipping the chip's right edge on some rows.
        self._table.setColumnWidth(1, 86)
        hh.setMinimumSectionSize(64)
        self._table.setSortingEnabled(False)   # source order = chronological
        self._table.setStyleSheet(
            "QTableWidget { border: none; }"
            " QTableWidget::item { padding: 4px 8px; }"
        )
        # currentCellChanged fires per-cell; we wrap so the runcard
        # re-renders only when the row changes (cheaper than per cell).
        self._table.currentCellChanged.connect(self._on_table_current_changed)
        left_layout.addWidget(self._table, 1)

        # ── Action buttons row ──────────────────────────────────────
        # +/- pattern (Apple-menu style): + opens a small menu
        # offering "Autodetect from Data/" or "Add manually…"; -
        # deletes the highlighted run(s) with inheritance preservation.
        btn_row = QtWidgets.QHBoxLayout()
        #  Bigger refresh button (see runmanager.py for the rationale).
        self._refresh_btn = QtWidgets.QPushButton(" ⟳  Refresh ")
        f = self._refresh_btn.font(); f.setPointSize(f.pointSize() + 2)
        self._refresh_btn.setFont(f)
        self._refresh_btn.setToolTip("Reload database from disk")
        self._refresh_btn.clicked.connect(self.reload)
        btn_row.addWidget(self._refresh_btn)

        self._add_btn = QtWidgets.QToolButton()
        self._add_btn.setText("＋")
        self._add_btn.setToolTip("Add run(s) — Autodetect from Data/ or add by hand")
        self._add_btn.setPopupMode(QtWidgets.QToolButton.InstantPopup)
        add_menu = QtWidgets.QMenu(self._add_btn)
        act_detect = add_menu.addAction("Autodetect from Data/")
        act_detect.triggered.connect(self._on_detect_runs)
        act_manual = add_menu.addAction("Add manually…")
        act_manual.triggered.connect(self._on_add_manually)
        self._add_btn.setMenu(add_menu)
        btn_row.addWidget(self._add_btn)

        self._remove_btn = QtWidgets.QToolButton()
        self._remove_btn.setText("−")
        self._remove_btn.setToolTip(
            "Delete the selected run(s).\n"
            "Forward-inheritance is preserved: the victim's own fields "
            "migrate to the next surviving run so downstream merged "
            "views don't silently shift."
        )
        self._remove_btn.clicked.connect(self._on_remove_runs)
        btn_row.addWidget(self._remove_btn)

        self._save_btn = QtWidgets.QPushButton("→ Runlist")
        self._save_btn.setToolTip(
            "Save the highlighted runs as a named runlist "
            "(visible in the Runlists tab)"
        )
        self._save_btn.clicked.connect(self._on_save_selection)
        btn_row.addWidget(self._save_btn)
        self._add_field_btn = QtWidgets.QPushButton("＋ Field")
        self._add_field_btn.setToolTip(
            "Add a new field to the runcard skeleton.\n"
            "Persists in [schema] extra_fields and shows as '· unset' "
            "on every existing run — no value is written anywhere "
            "until the operator fills it in on a specific run."
        )
        self._add_field_btn.clicked.connect(self._on_add_field)
        btn_row.addWidget(self._add_field_btn)
        btn_row.addStretch(1)
        left_layout.addLayout(btn_row)

        splitter.addWidget(left)

        # Right pane: editable runcard.
        right = QtWidgets.QScrollArea()
        right.setWidgetResizable(True)
        right.setFrameShape(QtWidgets.QFrame.NoFrame)
        self._card = _RunCard(on_edit=self._on_field_edited)
        right.setWidget(self._card)
        splitter.addWidget(right)

        splitter.setSizes([300, 800])
        outer.addWidget(splitter)

        self.reload()

    # ----- data ----------------------------------------------------------

    def reload(self) -> None:
        self._records = rundb.load_database(self._db_path)
        # Schema = union of every key seen on any run + curated
        # registries + operator-declared extras from [schema]
        # extra_fields.  Extras propagate backwards as "· unset" on
        # every existing run with no value written anywhere.
        self._extra_fields = set(rundb.read_schema_extras(self._db_path))
        self._schema = _schema_keys(self._records) | self._extra_fields
        self._refresh_field_picker()
        self._refresh_list()

    def _refresh_field_picker(self) -> None:
        """Sync the field-scope dropdown to the live schema.

        Preserves the operator's current selection across reloads —
        switching databases shouldn't reset their search scope.
        """
        if not hasattr(self, "_field_combo"):
            return
        current = self._field_combo.currentData() or ""
        # Build the ordered list: "any" → run_id → curated FIELD_ORDER
        # → everything else alphabetical.
        fields: list[str] = ["", "run_id"]
        for f in FIELD_ORDER:
            if f not in fields:
                fields.append(f)
        for f in sorted(self._schema):
            if f not in fields and f not in (NOTES_FIELD, RADIATORS_FIELD):
                fields.append(f)
        self._field_combo.blockSignals(True)
        self._field_combo.clear()
        for f in fields:
            label = "any field" if f == "" else f
            self._field_combo.addItem(label, f)
        idx = self._field_combo.findData(current)
        if idx >= 0:
            self._field_combo.setCurrentIndex(idx)
        self._field_combo.blockSignals(False)

    def _schedule_filter_refresh(self) -> None:
        """Restart the debounce timer; refresh fires on idle.

        Without this, a fast 5-char delete in the filter input
        triggers 5 full table rebuilds back-to-back; with the 150 ms
        timer we only refresh once the user stops typing.
        """
        self._filter_timer.start()

    def _refresh_list(self) -> None:
        """Rebuild the run table from ``self._records``.

        Cheap compared to the old per-row QWidget approach: a 250-row
        table populates in ~10 ms here, vs. ~1 s for 250 custom
        widgets (which was the bulk of the perceived "detect runs is
        very slow" lag).
        """
        needle = self._filter.text().strip().lower()
        # Active field-scope: empty string ("") = all fields.
        scope = ""
        if hasattr(self, "_field_combo"):
            scope = (self._field_combo.currentData() or "").strip()
        self._active_scope = scope
        self._active_needle = needle
        current_id = self._current_run_id()

        self._table.blockSignals(True)
        self._table.setRowCount(0)
        n_total = len(self._records)
        n_match = 0

        # Sort once into reverse-chronological order (run ids are
        # timestamps, so reverse-alphabetical = newest first).  Most
        # operators land here looking for the latest run, so we put
        # it at the top.  load_database returns chronological source
        # order; we flip on display only — the on-disk file stays
        # ascending so forward-inheritance keeps working.
        ordered = sorted(self._records, key=lambda r: r.run_id, reverse=True)

        # Pre-compute fonts / brushes once outside the loop.
        mono_font = QtGui.QFont("Menlo")
        mono_font.setStyleHint(QtGui.QFont.Monospace)
        muted_brush = QtGui.QBrush(QtGui.QColor("#9B8E8E"))

        target_row = -1
        for r in ordered:
            match_field = _match_field(r, needle, scope) if needle else None
            if needle and match_field is None:
                continue
            n_match += 1
            row = self._table.rowCount()
            self._table.insertRow(row)
            if r.run_id == current_id:
                target_row = row

            # Run-id cell — monospace, holds the run id in UserRole
            # so look-ups don't need to read the text back.
            id_item = QtWidgets.QTableWidgetItem(r.run_id)
            id_item.setData(QtCore.Qt.UserRole, r.run_id)
            id_item.setFont(mono_font)
            id_item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
            if needle and match_field and match_field != "run_id":
                # Tooltip surfaces *why* the row matched without
                # cluttering the visible text.
                id_item.setToolTip(f"matched on {match_field} = {r.get(match_field, '')}")
            self._table.setItem(row, 0, id_item)

            # Quality cell — colored chip via setCellWidget so the
            # chip can keep its rounded background.  The underlying
            # item still holds the text for tooltip / sort.
            quality = str(r.get("quality", "") or "")
            q_item = QtWidgets.QTableWidgetItem(quality)
            q_item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
            self._table.setItem(row, 1, q_item)
            if quality:
                chip = _make_quality_chip(quality)
                if chip is not None:
                    self._table.setCellWidget(row, 1, chip)

            # Spills cell — muted left-aligned number, blank when missing.
            spills_text = ""
            spills_val = r.get("n_spills")
            if spills_val not in (None, ""):
                try:
                    spills_text = f"{int(spills_val)}"
                except (TypeError, ValueError):
                    spills_text = ""
            s_item = QtWidgets.QTableWidgetItem(spills_text)
            s_item.setFont(mono_font)
            s_item.setForeground(muted_brush)
            s_item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
            if spills_text:
                s_item.setToolTip(f"{spills_text} spill(s)")
            self._table.setItem(row, 2, s_item)

        self._table.blockSignals(False)

        if needle:
            self._match_caption.setText(
                f"{n_match} of {n_total} matched on \"{needle}\""
            )
        else:
            self._match_caption.setText(f"{n_total} runs")

        # Restore selection.
        if target_row >= 0:
            self._table.setCurrentCell(target_row, 0)
        elif self._table.rowCount() > 0:
            self._table.setCurrentCell(0, 0)
        else:
            self._card.show_empty()

    def _current_run_id(self) -> Optional[str]:
        row = self._table.currentRow() if hasattr(self, "_table") else -1
        if row < 0:
            return None
        item = self._table.item(row, 0)
        return None if item is None else str(item.data(QtCore.Qt.UserRole))

    def _selected_run_ids(self) -> list[str]:
        """Run ids for every currently-selected row (multi-select aware)."""
        if not hasattr(self, "_table"):
            return []
        ids: list[str] = []
        for idx in self._table.selectionModel().selectedRows():
            item = self._table.item(idx.row(), 0)
            if item is not None:
                ids.append(str(item.data(QtCore.Qt.UserRole)))
        return ids

    def _on_table_current_changed(
        self, current_row: int, _current_col: int,
        previous_row: int, _previous_col: int,
    ) -> None:
        # currentCellChanged fires per-cell; only re-render when the
        # row actually moves.  Saves a runcard rebuild on every
        # left/right arrow press inside the same row.
        if current_row == previous_row:
            return
        if current_row < 0:
            self._card.show_empty()
            return
        item = self._table.item(current_row, 0)
        if item is None:
            self._card.show_empty()
            return
        run_id = str(item.data(QtCore.Qt.UserRole))
        record = next((r for r in self._records if r.run_id == run_id), None)
        if record is None:
            self._card.show_empty()
            return
        scope = getattr(self, "_active_scope", "") or ""
        match = (
            _match_field(record, self._active_needle, scope)
            if self._active_needle else None
        )
        self._card.show_record(record, self._schema, highlight_field=match)

    # ----- actions --------------------------------------------------------

    def _on_detect_runs(self) -> None:
        if self._data_dir is None:
            QtWidgets.QMessageBox.warning(
                self, "No data directory",
                "Runlist view wasn't given a Data/ directory to scan.",
            )
            return
        new_ids = rundb.detect_new_runs(self._db_path, self._data_dir)
        if not new_ids:
            QtWidgets.QMessageBox.information(
                self, "Detect runs",
                "No new runs under Data/ — the database is already up to date.",
            )
            return
        confirm = QtWidgets.QMessageBox.question(
            self, "Detect runs",
            f"Append {len(new_ids)} new run(s) to {self._db_path.name}?\n\n"
            + "\n".join(new_ids[:20])
            + ("\n…" if len(new_ids) > 20 else ""),
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
        )
        if confirm != QtWidgets.QMessageBox.Yes:
            return
        added = rundb.append_runs(self._db_path, new_ids)
        QtWidgets.QMessageBox.information(
            self, "Detect runs", f"Appended {added} run(s)."
        )
        self.reload()

    def _on_add_field(self) -> None:
        """Prompt for a new field name + register it in [schema] extra_fields.

        The new field is *not* written onto any run — it simply joins
        the schema so every runcard surfaces it as a ``· unset`` slot
        the operator can fill in per-run.
        """
        name, ok = QtWidgets.QInputDialog.getText(
            self, "Add field",
            "New field name (snake_case).\n\n"
            "Appears as '· unset' on every existing run until the "
            "operator types a value on a specific run.",
        )
        if not ok or not name.strip():
            return
        try:
            added = rundb.add_schema_extra(self._db_path, name.strip())
        except ValueError as exc:
            QtWidgets.QMessageBox.warning(self, "Add field", str(exc))
            return
        except Exception as exc:  # noqa: BLE001
            QtWidgets.QMessageBox.warning(
                self, "Add field", f"Could not write schema: {exc}",
            )
            return
        if not added:
            QtWidgets.QMessageBox.information(
                self, "Add field",
                f"Field {name.strip()!r} is already in the schema.",
            )
            return
        self.reload()

    def _on_save_selection(self) -> None:
        selected = [
            self._list.item(i).data(QtCore.Qt.UserRole)
            for i in range(self._list.count())
            if self._list.item(i).isSelected()
        ]
        if not selected:
            QtWidgets.QMessageBox.information(
                self, "Save selection",
                "Highlight one or more runs on the left first (Cmd/Ctrl-click for "
                "multi-select).",
            )
            return
        name, ok = QtWidgets.QInputDialog.getText(
            self, "Save selection as runlist",
            f"Name for this runlist ({len(selected)} runs):",
        )
        if not ok or not name.strip():
            return
        try:
            rundb.save_selection_as_runlist(self._runlists_path, name.strip(), selected)
        except ValueError as exc:
            QtWidgets.QMessageBox.warning(self, "Save selection", str(exc))
            return
        QtWidgets.QMessageBox.information(
            self, "Save selection",
            f"Wrote runlist {name!r} ({len(selected)} runs) → {self._runlists_path.name}",
        )

    def _on_field_edited(self, run_id: str, field: str, new_value) -> None:
        """Runcard told us a field was edited — persist with auto-pin.

        Dispatches the (potentially-slow) ``tomlkit`` write to the
        background worker so the GUI stays live.  The in-memory record
        is updated **optimistically** so the runcard repaints with the
        new "own" pill / value immediately; on disk-write failure we
        revert + show an error.
        """
        record = next((r for r in self._records if r.run_id == run_id), None)
        if record is None:
            return
        # Snapshot the previous state so we can revert on failure.
        # ``had_own`` tells us whether the field was already "own"
        # before (so we know whether to drop it back from own_fields
        # entirely on revert).
        had_own = field in record.own_fields
        old_value = record.own_fields.get(field) if had_own else None

        # Optimistic mutation — the runcard sees the new state on
        # the very next paint, even though the disk write is still
        # in flight.
        record.own_fields[field] = new_value
        self._refresh_row(run_id)
        if self._current_run_id() == run_id:
            self._card.show_record(record, self._schema)

        tag = f"edit:{run_id}:{field}:{id(self)}"
        self._pending_edits[tag] = (
            run_id, field, old_value, "had_own" if had_own else "fresh",
        )
        path = self._db_path

        def do_write() -> None:
            rundb.update_run_field(path, run_id, field, new_value, auto_pin=True)

        self._db_worker.submit(tag, do_write)

    def _on_db_worker_done(self, tag: str, ok: bool, error: str) -> None:
        """Worker finished — reload on success, revert on failure.

        We never block waiting for this: it lands on the GUI thread
        whenever the background thread is done.  Untracked tags
        (e.g. ones submitted by another view via the shared worker)
        are silently ignored.
        """
        if tag not in self._pending_edits:
            return
        run_id, field, old_value, kind = self._pending_edits.pop(tag)
        if ok:
            # Disk reflects the optimistic state — pull a fresh
            # records list so auto-pinned neighbour values are picked
            # up too, and refresh the row.  Cheap (~3 ms now).
            self._records = rundb.load_database(self._db_path)
            self._extra_fields = set(rundb.read_schema_extras(self._db_path))
            self._schema = _schema_keys(self._records) | self._extra_fields
            self._refresh_row(run_id)
            return
        # Failure — revert the optimistic mutation.
        record = next((r for r in self._records if r.run_id == run_id), None)
        if record is not None:
            if kind == "had_own":
                record.own_fields[field] = old_value
            else:
                record.own_fields.pop(field, None)
            self._refresh_row(run_id)
            if self._current_run_id() == run_id:
                self._card.show_record(record, self._schema)
        QtWidgets.QMessageBox.warning(
            self, "Edit failed",
            f"Could not write {field} on {run_id}:\n\n{error.splitlines()[0]}",
        )

    def _refresh_row(self, run_id: str) -> None:
        """Re-render a single table row in place, no full rebuild."""
        record = next((r for r in self._records if r.run_id == run_id), None)
        if record is None:
            return
        for row in range(self._table.rowCount()):
            item = self._table.item(row, 0)
            if item is None or item.data(QtCore.Qt.UserRole) != run_id:
                continue
            # Refresh quality chip + spills text in place.
            quality = str(record.get("quality", "") or "")
            self._table.removeCellWidget(row, 1)
            q_item = QtWidgets.QTableWidgetItem(quality)
            q_item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
            self._table.setItem(row, 1, q_item)
            if quality:
                chip = _make_quality_chip(quality)
                if chip is not None:
                    self._table.setCellWidget(row, 1, chip)
            spills_val = record.get("n_spills")
            spills_text = ""
            if spills_val not in (None, ""):
                try:
                    spills_text = f"{int(spills_val)}"
                except (TypeError, ValueError):
                    spills_text = ""
            s_item = self._table.item(row, 2)
            if s_item is not None:
                s_item.setText(spills_text)
                if spills_text:
                    s_item.setToolTip(f"{spills_text} spill(s)")
            return

    # ----- database picker ----------------------------------------------

    @staticmethod
    def _derive_runlists_path(
        database_path: Path,
        override: Path | None,
    ) -> Path:
        """Convention: ``<year>.database.toml`` ↔ ``<year>.runlists.toml``."""
        if override is not None:
            return override
        return database_path.parent / database_path.name.replace(
            "database", "runlists"
        )

    def _refresh_database_picker(self) -> None:
        """Repopulate the combo with every ``*.database.toml`` in the dir."""
        self._db_combo.blockSignals(True)
        self._db_combo.clear()
        found = sorted(self._database_dir.glob("*.database.toml"))
        # Always include the current path, even if a glob miss (e.g.
        # someone deletes a file under us) — keeps the picker
        # consistent with what's actually loaded.
        if self._db_path not in found:
            found.append(self._db_path)
            found.sort()
        for p in found:
            self._db_combo.addItem(p.name, str(p))
        idx = self._db_combo.findData(str(self._db_path))
        if idx >= 0:
            self._db_combo.setCurrentIndex(idx)
        # Disable when only one option — picker still visible for
        # discoverability (operator knows the feature exists) but
        # there's nothing to switch to.
        self._db_combo.setEnabled(len(found) > 1)
        self._db_combo.blockSignals(False)

    def _on_database_picked(self, idx: int) -> None:
        if idx < 0:
            return
        new_path = Path(self._db_combo.itemData(idx))
        if new_path == self._db_path:
            return
        self._db_path = new_path
        self._runlists_path = self._derive_runlists_path(
            new_path, self._runlists_path_override
        )
        # Clear filter + selection — the old selection probably
        # doesn't exist in the new database.
        self._filter.clear()
        self.reload()

    # ----- add manually / remove runs -----------------------------------

    def _on_add_manually(self) -> None:
        """Prompt for a run id + append it to the database manually.

        Useful when a run dir doesn't (yet) exist on disk — e.g. the
        operator wants to pre-populate metadata before the data
        arrives, or to track a remote run that's still being downloaded.
        """
        run_id, ok = QtWidgets.QInputDialog.getText(
            self, "Add run",
            "Run id to add (YYYYMMDD-HHMMSS):",
        )
        if not ok:
            return
        run_id = run_id.strip()
        if not run_id:
            return
        try:
            added = rundb.append_runs(self._db_path, [run_id])
        except Exception as exc:  # noqa: BLE001
            QtWidgets.QMessageBox.warning(
                self, "Add run", f"Could not append: {exc}",
            )
            return
        if not added:
            QtWidgets.QMessageBox.information(
                self, "Add run", f"Run {run_id!r} is already in the database.",
            )
            return
        self.reload()

    def _on_remove_runs(self) -> None:
        """Delete the currently-selected run(s)."""
        selected = self._selected_run_ids()
        if not selected:
            QtWidgets.QMessageBox.information(
                self, "Remove runs",
                "Select one or more runs in the list first.",
            )
            return
        confirm = QtWidgets.QMessageBox.question(
            self, "Remove runs",
            f"Delete {len(selected)} run(s) from {self._db_path.name}?\n\n"
            f"{', '.join(selected[:5])}"
            + ("…" if len(selected) > 5 else "")
            + "\n\nForward-inheritance is preserved: each victim's own "
              "fields migrate to the next surviving run so downstream "
              "merged views don't silently shift.",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
        )
        if confirm != QtWidgets.QMessageBox.Yes:
            return
        try:
            removed = rundb.delete_runs(self._db_path, selected)
        except Exception as exc:  # noqa: BLE001
            QtWidgets.QMessageBox.warning(
                self, "Remove runs", f"Could not delete: {exc}",
            )
            return
        if removed:
            self.reload()


def _make_quality_chip(quality: str) -> Optional[QtWidgets.QLabel]:
    """A colour-coded quality chip suitable for ``setCellWidget``.

    Returns ``None`` for an empty ``quality`` so callers can short-
    circuit; the table column then renders the underlying item text
    (which is also empty, so nothing shows).
    """
    if not quality:
        return None
    colour = QUALITY_COLOURS.get(quality.lower()) or QUALITY_COLOURS.get(quality)
    chip = QtWidgets.QLabel(quality)
    chip.setAlignment(QtCore.Qt.AlignCenter)
    # Margin keeps the chip from butting up against the row's
    # alternating background — looks like a properly-padded badge.
    chip.setContentsMargins(4, 1, 4, 1)
    if colour:
        chip.setStyleSheet(
            f"background: {colour}; color: #1B1A1A;"
            " font-weight: 700; font-size: 12px;"
            " padding: 1px 8px; border-radius: 4px;"
        )
    else:
        chip.setObjectName("muted")
    return chip


class _NotesEdit(QtWidgets.QTextEdit):
    """``QTextEdit`` that emits ``committed`` on focus-out.

    ``QTextEdit`` has no ``editingFinished`` equivalent — we use focus
    loss as the commit point (same UX as the line editors elsewhere
    in the card) so each keystroke doesn't trigger a database write.
    """

    committed = QtCore.Signal()

    def focusOutEvent(self, event: QtGui.QFocusEvent) -> None:
        super().focusOutEvent(event)
        self.committed.emit()


def _origin_pill(origin: str) -> QtWidgets.QLabel:
    """Small colour-coded marker showing where a field's value came from.

    ``"own"``       → coral ``✎ own``; the field is set explicitly on
                      this run.
    ``"inherited"`` → warm-grey ``↪ inherited``; the value comes from a
                      previous entry via the forward-inheritance chain.
    ``"unset"``     → muted ``· unset``; neither this run nor any
                      upstream run set this field — it's a gap in the
                      skeleton the operator can fill in.
    """
    if origin == "own":
        pill = QtWidgets.QLabel("✎ own")
        pill.setStyleSheet(
            "color: #FF6B6B;"  # coral accent
            " font-size: 10px; font-weight: 600; padding: 1px 4px;"
        )
    elif origin == "inherited":
        pill = QtWidgets.QLabel("↪ inherited")
        pill.setStyleSheet(
            "color: #9B8E8E;"  # warm grey
            " font-size: 10px; padding: 1px 4px;"
        )
    else:  # unset
        pill = QtWidgets.QLabel("· unset")
        pill.setStyleSheet(
            "color: #6B6968;"  # dim — clearly absent rather than present
            " font-size: 10px; font-style: italic; padding: 1px 4px;"
        )
    return pill


def _match_field(
    record: rundb.RunRecord,
    needle: str,
    scope: str = "",
) -> Optional[str]:
    """Return which field matched ``needle``, or ``None`` if none did.

    ``scope=""`` searches every field (returns ``"run_id"`` when the
    run identifier matches; otherwise the first field-name whose
    value contains the needle).  A non-empty ``scope`` restricts the
    search to that one field — useful for "all runs with v_bias = 35"
    without false positives from other numeric fields.
    """
    if not needle:
        return None
    if scope:
        # Field-scoped search — return the scope on match, None otherwise.
        if scope == "run_id":
            return "run_id" if needle in record.run_id.lower() else None
        val = record.merged.get(scope)
        if val is None:
            return None
        return scope if needle in str(val).lower() else None
    # Default: any-field search.
    if needle in record.run_id.lower():
        return "run_id"
    for k, v in record.merged.items():
        if v is None:
            continue
        if needle in str(v).lower():
            return k
    return None


def _matches(record: rundb.RunRecord, needle: str) -> bool:
    return _match_field(record, needle) is not None


# ---------------------------------------------------------------------------
# Runcard — right-pane read-only view of one merged run record.
# ---------------------------------------------------------------------------


class _RunCard(QtWidgets.QWidget):
    """Right-pane editable card for one ``RunRecord``.

    Every merged field is rendered; **own** fields are tagged
    ``own``, **inherited** ones ``inherited``.  Edits flow through
    the ``on_edit(run_id, field, new_value)`` callback the parent
    provides (typically wires into ``rundb.update_run_field`` with
    auto-pin).
    """

    def __init__(
        self,
        on_edit,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._on_edit = on_edit
        self._current_run_id: Optional[str] = None
        self._layout = QtWidgets.QVBoxLayout(self)
        self._layout.setContentsMargins(12, 12, 12, 12)
        self._layout.setSpacing(8)
        self._layout.setAlignment(QtCore.Qt.AlignTop)
        self.show_empty()

    def show_empty(self) -> None:
        self._reset()
        msg = QtWidgets.QLabel("Pick a run on the left.")
        msg.setObjectName("muted")
        msg.setStyleSheet("font-style: italic;")
        self._layout.addWidget(msg)

    def show_record(
        self,
        record: rundb.RunRecord,
        schema: Optional[set[str]] = None,
        highlight_field: Optional[str] = None,
    ) -> None:
        self._reset()
        self._current_run_id = record.run_id

        # Fall back to the merged-only key set when the caller didn't
        # supply a schema (preserves the old behaviour for callers that
        # haven't been updated yet — e.g. unit tests).
        if schema is None:
            schema = set(record.merged.keys()) | {NOTES_FIELD}

        title = QtWidgets.QLabel(record.run_id)
        title_font = title.font()
        title_font.setPointSize(title_font.pointSize() + 4)
        title_font.setBold(True)
        title.setFont(title_font)
        self._layout.addWidget(title)

        unset_count = sum(
            1 for k in schema if k != NOTES_FIELD and k not in record.merged
        )
        subtitle = QtWidgets.QLabel(
            f"{len(record.own_fields)} own"
            f"  ·  {len(record.inherited_fields)} inherited"
            f"  ·  {unset_count} unset"
        )
        subtitle.setObjectName("muted")
        self._layout.addWidget(subtitle)

        hint = QtWidgets.QLabel(
            "Edits auto-pin the previous value on the next inheriting run, "
            "so downstream runs don't silently shift.  Unset slots fill the "
            "skeleton — typing a value writes it as the run's own."
        )
        hint.setObjectName("muted")
        hint.setStyleSheet("font-style: italic; font-size: 11px;")
        hint.setWordWrap(True)
        self._layout.addWidget(hint)

        # Field grid — 2 cells across, each cell is label / input / unit / origin.
        grid_holder = QtWidgets.QWidget()
        grid = QtWidgets.QGridLayout(grid_holder)
        grid.setHorizontalSpacing(14)
        grid.setVerticalSpacing(6)
        grid.setContentsMargins(0, 8, 0, 0)
        ncols = 2

        ordered = _ordered_keys(schema)

        # 4 widgets per cell: label · editor · unit · origin
        for c in range(ncols * 4):
            grid.setColumnStretch(c, 0)
        for c in range(1, ncols * 4, 4):
            grid.setColumnStretch(c, 1)

        for idx, key in enumerate(ordered):
            row, col = divmod(idx, ncols)
            base_col = col * 4

            if key in record.own_fields:
                origin = "own"
            elif key in record.merged:
                origin = "inherited"
            else:
                origin = "unset"

            label = QtWidgets.QLabel(_prettify_key(key))
            label.setStyleSheet("font-weight: 600;")
            label.setToolTip(f"raw key: {key}")
            if origin == "unset":
                # Dim the label too so the row reads as a gap, not a value.
                label.setStyleSheet("font-weight: 600; color: #6B6968;")
            grid.addWidget(label, row, base_col)

            current_value = record.merged.get(key)  # None when unset
            editor = self._build_editor(key, current_value)
            if origin == "unset":
                # A muted dash placeholder makes the empty slot
                # visually distinct from "explicitly empty string".
                if isinstance(editor, QtWidgets.QLineEdit):
                    editor.setPlaceholderText("—")
            # Tint the editor's background when this field matched
            # the active search so the operator can see what tripped
            # the filter.
            if highlight_field and highlight_field == key:
                editor.setStyleSheet(
                    "background: rgba(11, 218, 81, 0.20);"
                    " border: 1px solid rgba(11, 218, 81, 0.55);"
                )
            grid.addWidget(editor, row, base_col + 1)

            unit = QtWidgets.QLabel(RUNFIELD_UNITS.get(key, ""))
            unit.setObjectName("muted")
            unit.setStyleSheet("font-size: 10px;")
            unit.setFixedWidth(46)
            grid.addWidget(unit, row, base_col + 2)

            grid.addWidget(_origin_pill(origin), row, base_col + 3)

        self._layout.addWidget(grid_holder)

        # ── Radiators — dedicated rendering ────────────────────────
        # Pulled out of the generic field grid because it's a list of
        # inline tables (Aerogel + Gas + …), each with type / refindex
        # / tag / depth columns — a flat str() into a line edit (the
        # old default) was unreadable.  Rendered as a compact table.
        self._layout.addWidget(self._build_radiators_section(record, highlight_field))

        # Stretch eats any leftover space so the notes block stays
        # *visually* anchored at the bottom of the card.
        self._layout.addStretch(1)

        # ── Notes section — full width, multi-line, anchored bottom ──
        self._layout.addWidget(self._build_notes_section(record, schema, highlight_field))

    # ---- radiators section ---------------------------------------------

    def _build_radiators_section(
        self,
        record: rundb.RunRecord,
        highlight_field: Optional[str],
    ) -> QtWidgets.QWidget:
        holder = QtWidgets.QFrame()
        holder.setObjectName("cardSurface")
        v = QtWidgets.QVBoxLayout(holder)
        v.setContentsMargins(12, 10, 12, 12)
        v.setSpacing(6)

        header = QtWidgets.QHBoxLayout()
        title = QtWidgets.QLabel("Radiators")
        tf = title.font()
        tf.setBold(True)
        tf.setPointSize(tf.pointSize() + 1)
        title.setFont(tf)
        header.addWidget(title)
        header.addStretch(1)

        if RADIATORS_FIELD in record.own_fields:
            origin = "own"
        elif RADIATORS_FIELD in record.merged:
            origin = "inherited"
        else:
            origin = "unset"
        header.addWidget(_origin_pill(origin))
        v.addLayout(header)

        radiators = record.merged.get(RADIATORS_FIELD)
        if not radiators:
            msg = QtWidgets.QLabel("(none configured)")
            msg.setObjectName("muted")
            msg.setStyleSheet("font-style: italic;")
            v.addWidget(msg)
            return holder

        # Column union — different runs may carry different fields on
        # each radiator entry.  We always show ``type`` first when
        # present so the operator sees what kind of radiator each row
        # describes; the rest follows alphabetical for stability.
        seen_cols: list[str] = []
        for r in radiators:
            if not isinstance(r, dict):
                continue
            for k in r.keys():
                if k not in seen_cols:
                    seen_cols.append(k)
        #  Curated column order: Type, n, Depth/Thickness, Side, …,
        #  Tag last so the (potentially long) free-form aerogel ID
        #  has the whole right-hand stretch column to live in.  Anything
        #  unknown falls in alphabetically between Side and Tag.
        priority = ["type", "refindex", "depth", "thickness", "side"]
        trailing = ["tag"]
        head = [c for c in priority if c in seen_cols]
        tail = [c for c in trailing if c in seen_cols]
        middle = sorted(c for c in seen_cols
                        if c not in head and c not in tail)
        seen_cols = head + middle + tail

        # Pretty column labels with units where we know them.
        unit_for = {
            "refindex": "",         # dimensionless
            "depth":    "mm",
            "thickness": "mm",
        }
        pretty_for = {
            "type":     "Type",
            "refindex": "n",
            "tag":      "Tag",
            "depth":    "Depth",
            "thickness": "Thickness",
        }
        header_text = [
            pretty_for.get(c, c.capitalize()) + (f" ({unit_for[c]})" if unit_for.get(c) else "")
            for c in seen_cols
        ]

        table = QtWidgets.QTableWidget(len(radiators), len(seen_cols))
        table.setHorizontalHeaderLabels(header_text)
        table.verticalHeader().setVisible(False)
        table.setShowGrid(False)
        table.setAlternatingRowColors(True)
        table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        table.setSelectionMode(QtWidgets.QAbstractItemView.NoSelection)
        table.setFocusPolicy(QtCore.Qt.NoFocus)
        table.setStyleSheet(
            "QTableWidget { border: none; }"
            " QTableWidget::item { padding: 3px 8px; }"
        )
        #  Numeric / short columns hug their content; the **Tag** column
        #  (curated last) stretches to fill whatever space the
        #  Radiators card has left over.  Also bump its minimum size
        #  so even when the card is narrow the operator gets enough
        #  room to read full aerogel IDs like ``AG22-J001-side-B``.
        for c, key in enumerate(seen_cols):
            mode = (
                QtWidgets.QHeaderView.Stretch
                if key == "tag"
                else QtWidgets.QHeaderView.ResizeToContents
            )
            table.horizontalHeader().setSectionResizeMode(c, mode)
        if "tag" in seen_cols:
            tag_col = seen_cols.index("tag")
            table.horizontalHeader().setMinimumSectionSize(80)
            #  Set a generous default column width as the stretch
            #  floor — Qt's Stretch mode honours minimum but not
            #  preferred, so without this the column compresses to
            #  the header text on small cards.
            table.setColumnWidth(tag_col, 220)
        else:
            # Fall back: no Tag → stretch the last column so the
            # table still fills its cell.
            if len(seen_cols):
                table.horizontalHeader().setSectionResizeMode(
                    len(seen_cols) - 1, QtWidgets.QHeaderView.Stretch,
                )
        #  Also stretch the table itself horizontally so the parent
        #  layout actually hands it the extra width.  Without this,
        #  the table sits at its Preferred size and Tag never grows.
        table.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding,
            QtWidgets.QSizePolicy.Fixed,
        )

        mono_font = QtGui.QFont("Menlo")
        mono_font.setStyleHint(QtGui.QFont.Monospace)
        for row, r in enumerate(radiators):
            if not isinstance(r, dict):
                # Defensive — old / hand-edited entries might be a
                # plain string.  Show it in the first column.
                item = QtWidgets.QTableWidgetItem(str(r))
                item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
                table.setItem(row, 0, item)
                continue
            for col, key in enumerate(seen_cols):
                val = r.get(key, "")
                text = "" if val is None else str(val)
                item = QtWidgets.QTableWidgetItem(text)
                if key != "type":
                    item.setFont(mono_font)
                item.setTextAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
                table.setItem(row, col, item)

        # Size the table to its content — no inner scroll, all rows
        # always visible.  Header + row heights + a small fudge.
        header_h = table.horizontalHeader().height()
        row_h = (
            table.verticalHeader().sectionSize(0) if table.rowCount() else 22
        )
        table.setFixedHeight(header_h + row_h * table.rowCount() + 6)
        table.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        table.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        if highlight_field == RADIATORS_FIELD:
            table.setStyleSheet(
                "QTableWidget { border: 1px solid rgba(11, 218, 81, 0.55);"
                " background: rgba(11, 218, 81, 0.10); }"
                " QTableWidget::item { padding: 3px 8px; }"
            )
        v.addWidget(table)
        return holder

    # ---- notes section -------------------------------------------------

    def _build_notes_section(
        self,
        record: rundb.RunRecord,
        schema: set[str],
        highlight_field: Optional[str],
    ) -> QtWidgets.QWidget:
        holder = QtWidgets.QFrame()
        holder.setObjectName("cardSurface")
        v = QtWidgets.QVBoxLayout(holder)
        v.setContentsMargins(12, 10, 12, 12)
        v.setSpacing(6)

        header = QtWidgets.QHBoxLayout()
        title = QtWidgets.QLabel("Notes")
        tf = title.font()
        tf.setBold(True)
        tf.setPointSize(tf.pointSize() + 1)
        title.setFont(tf)
        header.addWidget(title)
        header.addStretch(1)

        if NOTES_FIELD in record.own_fields:
            origin = "own"
        elif NOTES_FIELD in record.merged:
            origin = "inherited"
        else:
            origin = "unset"
        header.addWidget(_origin_pill(origin))
        v.addLayout(header)

        editor = _NotesEdit()
        editor.setMinimumHeight(140)
        editor.setPlaceholderText(
            "—  (no notes on this run yet — type to add)"
            if origin == "unset" else ""
        )
        current = record.merged.get(NOTES_FIELD) or ""
        editor.setPlainText(str(current))
        if highlight_field == NOTES_FIELD:
            editor.setStyleSheet(
                "background: rgba(11, 218, 81, 0.15);"
                " border: 1px solid rgba(11, 218, 81, 0.55);"
            )

        run_id = self._current_run_id
        original = str(current)

        def commit() -> None:
            text = editor.toPlainText()
            if text == original:
                return
            if text == "" and origin == "unset":
                # Nothing to write — leaving an unset field unset.
                return
            self._on_edit(run_id, NOTES_FIELD, text)

        editor.committed.connect(commit)
        v.addWidget(editor)
        return holder

    def _build_editor(self, field: str, value) -> QtWidgets.QWidget:
        """Pick a small editor matching the value's runtime type.

        ``value=None`` means the field is *unset* on this run — the
        editor renders empty and only fires ``_on_edit`` once the
        operator actually types something (no spurious empty writes).
        """
        run_id = self._current_run_id
        # Closed-choice fields (polarity, quality) render as dropdowns
        # regardless of the value's runtime type.
        choices = RUNFIELD_ENUMS.get(field)
        if choices:
            combo = QtWidgets.QComboBox()
            # Sentinel for the unset case: a blank entry the operator
            # passes over to actually select a choice.
            if value is None:
                combo.addItem("—", userData="")
            for c in choices:
                combo.addItem(c)
            current_str = "" if value is None else str(value)
            if current_str not in choices and current_str:
                combo.addItem(f"({current_str})", userData=current_str)
                combo.model().item(combo.count() - 1).setEnabled(False)
            if current_str in choices:
                combo.setCurrentText(current_str)

            def on_combo_changed(text: str, k=field, rid=run_id) -> None:
                if text in ("", "—"):
                    return  # didn't actually pick a value
                clean = text if not text.startswith("(") else text.strip("()")
                self._on_edit(rid, k, clean)

            combo.currentTextChanged.connect(on_combo_changed)
            return combo
        if isinstance(value, bool):
            cb = QtWidgets.QCheckBox()
            cb.setChecked(value)
            cb.toggled.connect(
                lambda checked, k=field, rid=run_id: self._on_edit(rid, k, bool(checked))
            )
            return cb
        # Anything not-a-bool gets a text edit; we re-coerce on commit
        # based on the original type so ints stay ints in TOML.
        line = QtWidgets.QLineEdit(str(value) if value is not None else "")
        line.setStyleSheet("font-family: 'Menlo','Consolas',monospace;")
        line.setMaximumWidth(220)
        original = value

        def commit() -> None:
            text = line.text().strip()
            # Unset slot left empty — nothing to do.
            if original is None and text == "":
                return
            if isinstance(original, int) and not isinstance(original, bool):
                try:
                    new_val = int(text)
                except ValueError:
                    line.setText(str(original))
                    return
            elif isinstance(original, float):
                try:
                    new_val = float(text.replace(",", "."))
                except ValueError:
                    line.setText(str(original))
                    return
            else:
                new_val = text
            if new_val == original:
                return
            self._on_edit(run_id, field, new_val)

        line.editingFinished.connect(commit)
        return line

    def _reset(self) -> None:
        while self._layout.count():
            item = self._layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()


__all__ = ["RunlistView"]
