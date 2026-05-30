"""Runlists tab — browse / edit ``run-lists/*.runlists.toml``.

A *runlist* is a named selection of run ids drawn from the
``database.toml`` file alongside it.  The Database tab's
"→ Runlist" button saves the current row selection here; the
Runlists tab is where the operator views / renames / deletes those
named selections and inspects their member runs.

File layout::

    [runlists.<name>]
    runs = ["YYYYMMDD-HHMMSS", "YYYYMMDD-HHMMSS", …]

Multiple runlist files can live side-by-side under ``run-lists/``
(one per year / beam test, matching the database picker).  The tab
picks them up automatically.

Scope (v1):
  - Left pane: picker for runlist-file + list of named runlists.
  - Right pane: member runs for the selected runlist, in source
    order; basic rename + delete actions on the named list.
  - Read of the runs is read-only — the runs themselves live in the
    Database tab, not here.

Editing the run *membership* of a runlist (add/remove specific runs)
ships in a follow-up; the v1 interface focuses on the lifecycle of
the named-selection itself.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Optional

import tomlkit
from PySide6 import QtCore, QtGui, QtWidgets

from .dbworker import DbWorker

if sys.version_info >= (3, 11):
    import tomllib as _tomllib
else:  # pragma: no cover
    import tomli as _tomllib  # type: ignore


def _read_runlists(path: Path) -> dict[str, list[str]]:
    """Parse a ``*.runlists.toml`` file to ``{name: [run_id, …]}``.

    Uses stdlib ``tomllib`` (fast — same approach as the database
    reader).  Empty / missing / unparseable file → empty dict.
    """
    if not path.is_file():
        return {}
    try:
        with path.open("rb") as fh:
            doc = _tomllib.load(fh)
    except (OSError, _tomllib.TOMLDecodeError):
        return {}
    rls = doc.get("runlists") or {}
    out: dict[str, list[str]] = {}
    if isinstance(rls, dict):
        for name, body in rls.items():
            if isinstance(body, dict):
                runs = body.get("runs") or []
                if isinstance(runs, list):
                    out[str(name)] = [str(r) for r in runs]
    return out


def _create_runlist(path: Path, name: str) -> bool:
    """Add an empty ``[runlists.<name>]`` block.

    Returns False if the name already exists.  Creates the file
    (with a ``[runlists]`` table) when it doesn't exist yet, so a
    fresh database directory can be bootstrapped without manual
    file creation.
    """
    name = name.strip()
    if not name:
        raise ValueError("runlist name cannot be empty")
    text = path.read_text() if path.is_file() else ""
    doc = tomlkit.parse(text) if text else tomlkit.document()
    rls = doc.get("runlists")
    if rls is None:
        rls = tomlkit.table()
        doc["runlists"] = rls
    if name in rls:
        return False
    entry = tomlkit.table()
    entry["runs"] = tomlkit.array()
    rls[name] = entry
    new_text = tomlkit.dumps(doc)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, path)
    return True


def _set_runlist_runs(path: Path, name: str, run_ids: list[str]) -> bool:
    """Replace the ``runs`` array on ``[runlists.<name>]`` wholesale.

    Used both for "add runs" (caller merges + dedupes the new list)
    and "remove runs" (caller filters the existing list).  Keeps
    one shared TOML round-trip path instead of two specialised ones.
    """
    if not path.is_file():
        return False
    doc = tomlkit.parse(path.read_text())
    rls = doc.get("runlists")
    if rls is None or name not in rls:
        return False
    entry = rls[name]
    arr = tomlkit.array()
    for r in run_ids:
        arr.append(r)
    entry["runs"] = arr
    new_text = tomlkit.dumps(doc)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, path)
    return True


def _delete_runlist(path: Path, name: str) -> bool:
    """Remove ``[runlists.<name>]`` from ``path``.  Returns True on success."""
    if not path.is_file():
        return False
    doc = tomlkit.parse(path.read_text())
    rls = doc.get("runlists")
    if rls is None or name not in rls:
        return False
    del rls[name]
    new_text = tomlkit.dumps(doc)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, path)
    return True


def _rename_runlist(path: Path, old_name: str, new_name: str) -> bool:
    """Rename a runlist in-place, preserving its run list + comments."""
    if not path.is_file():
        return False
    new_name = new_name.strip()
    if not new_name or new_name == old_name:
        return False
    doc = tomlkit.parse(path.read_text())
    rls = doc.get("runlists")
    if rls is None or old_name not in rls:
        return False
    if new_name in rls:
        raise ValueError(f"a runlist named {new_name!r} already exists")
    rls[new_name] = rls[old_name]
    del rls[old_name]
    new_text = tomlkit.dumps(doc)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, path)
    return True


class RunlistsView(QtWidgets.QWidget):
    """Runlists tab — see module docstring."""

    def __init__(
        self,
        runlists_path: Path,
        parent: QtWidgets.QWidget | None = None,
        *,
        db_worker: DbWorker | None = None,
    ) -> None:
        super().__init__(parent)
        self._rl_path = runlists_path
        self._rl_dir = runlists_path.parent
        self._db_worker = db_worker if db_worker is not None else DbWorker(self)
        self._db_worker.done.connect(self._on_db_worker_done)
        self._runlists: dict[str, list[str]] = {}
        self._pending: set[str] = set()  # tags we're waiting on

        outer = QtWidgets.QHBoxLayout(self)
        outer.setContentsMargins(10, 10, 10, 10)
        outer.setSpacing(10)

        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        splitter.setChildrenCollapsible(False)

        # ── Left pane: file picker + list of named runlists ─────────
        left = QtWidgets.QWidget()
        lv = QtWidgets.QVBoxLayout(left)
        lv.setContentsMargins(0, 0, 6, 0)
        lv.setSpacing(4)

        file_row = QtWidgets.QHBoxLayout()
        file_row.setSpacing(6)
        file_lbl = QtWidgets.QLabel("File:")
        file_lbl.setObjectName("muted")
        file_row.addWidget(file_lbl)
        self._file_combo = QtWidgets.QComboBox()
        self._file_combo.setToolTip(
            "Switch among run-lists/*.runlists.toml.  Typically one "
            "per year / beam test."
        )
        self._file_combo.currentIndexChanged.connect(self._on_file_picked)
        file_row.addWidget(self._file_combo, 1)
        lv.addLayout(file_row)
        self._refresh_file_picker()

        cap = QtWidgets.QLabel("Named runlists")
        cap.setObjectName("muted")
        lv.addWidget(cap)

        self._list = QtWidgets.QListWidget()
        self._list.setSelectionMode(
            QtWidgets.QAbstractItemView.SingleSelection
        )
        self._list.currentItemChanged.connect(self._on_runlist_picked)
        lv.addWidget(self._list, 1)

        btn_row = QtWidgets.QHBoxLayout()
        #  Bigger refresh button (see runmanager.py for the rationale).
        self._refresh_btn = QtWidgets.QPushButton(" ⟳  Refresh ")
        f = self._refresh_btn.font(); f.setPointSize(f.pointSize() + 2)
        self._refresh_btn.setFont(f)
        self._refresh_btn.setToolTip("Reload runlists from disk")
        self._refresh_btn.clicked.connect(self.reload)
        btn_row.addWidget(self._refresh_btn)
        self._new_btn = QtWidgets.QToolButton()
        self._new_btn.setText("＋")
        self._new_btn.setToolTip("Create a new empty runlist")
        self._new_btn.clicked.connect(self._on_new_runlist)
        btn_row.addWidget(self._new_btn)
        self._delete_btn = QtWidgets.QToolButton()
        self._delete_btn.setText("−")
        self._delete_btn.setToolTip(
            "Delete the selected runlist (member runs stay in the database)"
        )
        self._delete_btn.clicked.connect(self._on_delete)
        btn_row.addWidget(self._delete_btn)
        self._rename_btn = QtWidgets.QPushButton("Rename…")
        self._rename_btn.clicked.connect(self._on_rename)
        btn_row.addWidget(self._rename_btn)
        btn_row.addStretch(1)
        lv.addLayout(btn_row)

        splitter.addWidget(left)

        # ── Right pane: member runs of the selected runlist ─────────
        right = QtWidgets.QWidget()
        rv = QtWidgets.QVBoxLayout(right)
        rv.setContentsMargins(6, 0, 0, 0)
        rv.setSpacing(6)
        self._head = QtWidgets.QLabel("(pick a runlist on the left)")
        head_font = self._head.font()
        head_font.setBold(True)
        head_font.setPointSize(head_font.pointSize() + 3)
        self._head.setFont(head_font)
        rv.addWidget(self._head)
        self._subhead = QtWidgets.QLabel("")
        self._subhead.setObjectName("muted")
        rv.addWidget(self._subhead)

        self._runs = QtWidgets.QListWidget()
        self._runs.setStyleSheet(
            "QListWidget { border: none; }"
            " QListWidget::item { padding: 3px 8px;"
            "   font-family: 'Menlo','Consolas',monospace; }"
        )
        self._runs.setAlternatingRowColors(True)
        self._runs.setSelectionMode(
            QtWidgets.QAbstractItemView.ExtendedSelection
        )
        rv.addWidget(self._runs, 1)

        # ── Member-runs actions ─────────────────────────────────────
        # +/- pattern matching the Database tab.  Add pulls candidate
        # run ids from the matching database file (derived from the
        # current runlists file's name); remove drops the selected
        # rows on the right.
        #
        # Use full QPushButtons with explicit "Add runs" / "Remove"
        # text and the same +2 point bump as the Refresh button — the
        # earlier QToolButton with bare "＋ runs" glyph was easy to
        # miss in the row of icons.  Add-runs is the primary action
        # on this pane; it should look it.
        memb_row = QtWidgets.QHBoxLayout()
        self._add_runs_btn = QtWidgets.QPushButton(" ＋  Add runs ")
        f = self._add_runs_btn.font(); f.setPointSize(f.pointSize() + 2)
        self._add_runs_btn.setFont(f)
        self._add_runs_btn.setToolTip(
            "Add runs from the matching database to this runlist"
        )
        self._add_runs_btn.clicked.connect(self._on_add_runs)
        memb_row.addWidget(self._add_runs_btn)
        self._remove_runs_btn = QtWidgets.QPushButton(" −  Remove ")
        self._remove_runs_btn.setFont(f)
        self._remove_runs_btn.setToolTip(
            "Remove the selected run(s) from this runlist"
        )
        self._remove_runs_btn.clicked.connect(self._on_remove_runs)
        memb_row.addWidget(self._remove_runs_btn)
        memb_row.addStretch(1)
        rv.addLayout(memb_row)

        splitter.addWidget(right)
        splitter.setSizes([300, 800])
        outer.addWidget(splitter)

        self.reload()

    # ----- data ----------------------------------------------------------

    def reload(self) -> None:
        self._runlists = _read_runlists(self._rl_path)
        # Repopulate the named-list pane.
        current_name = None
        item = self._list.currentItem()
        if item is not None:
            current_name = item.text()
        self._list.blockSignals(True)
        self._list.clear()
        for name in sorted(self._runlists.keys()):
            it = QtWidgets.QListWidgetItem(name)
            it.setToolTip(f"{len(self._runlists[name])} run(s)")
            self._list.addItem(it)
        self._list.blockSignals(False)
        if current_name:
            for i in range(self._list.count()):
                if self._list.item(i).text() == current_name:
                    self._list.setCurrentRow(i)
                    return
        if self._list.count() > 0:
            self._list.setCurrentRow(0)
        else:
            self._show_empty()

    def _show_empty(self) -> None:
        self._head.setText("(no runlists yet)")
        self._subhead.setText(
            "Highlight runs in the Database tab and click \"→ Runlist\" "
            "to save a named selection here."
        )
        self._runs.clear()

    def _on_runlist_picked(self, current, _previous) -> None:
        if current is None:
            self._show_empty()
            return
        name = current.text()
        runs = self._runlists.get(name, [])
        self._head.setText(name)
        self._subhead.setText(f"{len(runs)} run(s)")
        self._runs.clear()
        for r in runs:
            self._runs.addItem(r)

    # ----- file picker --------------------------------------------------

    def _refresh_file_picker(self) -> None:
        self._file_combo.blockSignals(True)
        self._file_combo.clear()
        found = sorted(self._rl_dir.glob("*.runlists.toml"))
        if self._rl_path not in found:
            found.append(self._rl_path)
            found.sort()
        for p in found:
            self._file_combo.addItem(p.name, str(p))
        idx = self._file_combo.findData(str(self._rl_path))
        if idx >= 0:
            self._file_combo.setCurrentIndex(idx)
        self._file_combo.setEnabled(len(found) > 1)
        self._file_combo.blockSignals(False)

    def _on_file_picked(self, idx: int) -> None:
        if idx < 0:
            return
        new_path = Path(self._file_combo.itemData(idx))
        if new_path == self._rl_path:
            return
        self._rl_path = new_path
        self.reload()

    # ----- rename / delete ----------------------------------------------

    def _selected_name(self) -> Optional[str]:
        it = self._list.currentItem()
        return None if it is None else it.text()

    def _matching_database(self) -> Path:
        """Path of the database file paired with the current runlists.

        Convention: ``<year>.runlists.toml`` ↔ ``<year>.database.toml``,
        mirroring ``_derive_runlists_path`` on the Database tab.  Used
        to populate the "add runs" picker with the right run id pool.
        """
        return self._rl_dir / self._rl_path.name.replace(
            "runlists", "database",
        )

    def _on_new_runlist(self) -> None:
        name, ok = QtWidgets.QInputDialog.getText(
            self, "New runlist",
            "Name for the new runlist:",
        )
        if not ok or not name.strip():
            return
        path = self._rl_path
        new_name = name.strip()
        tag = f"new:{new_name}:{id(self)}"
        self._pending.add(tag)

        def do_create() -> None:
            if not _create_runlist(path, new_name):
                raise ValueError(f"a runlist named {new_name!r} already exists")
        self._db_worker.submit(tag, do_create)

    def _on_add_runs(self) -> None:
        name = self._selected_name()
        if not name:
            QtWidgets.QMessageBox.information(
                self, "Add runs",
                "Pick a runlist on the left first.",
            )
            return
        # Load the candidate pool from the matching database.  Using
        # tomllib directly (fast) — no need for full RunRecord parsing
        # here; just the [runs] keys.
        db_path = self._matching_database()
        if not db_path.is_file():
            QtWidgets.QMessageBox.warning(
                self, "Add runs",
                f"No matching database found at {db_path.name}",
            )
            return
        try:
            with db_path.open("rb") as fh:
                doc = _tomllib.load(fh)
        except (OSError, _tomllib.TOMLDecodeError) as exc:
            QtWidgets.QMessageBox.warning(
                self, "Add runs", f"Could not read database: {exc}",
            )
            return
        runs_table = doc.get("runs")
        if not isinstance(runs_table, dict):
            return
        #  Picker sorted newest-first so the operator finds the
        #  fresh run they just want to add at the top.  The
        #  serialised runlist (further down) stays chronological so
        #  consumers walking it in order still get expected ordering.
        all_ids = sorted(runs_table.keys(), reverse=True)
        existing = set(self._runlists.get(name, []))

        picked = _RunPickerDialog.pick(self, all_ids, existing, target=name)
        if picked is None or not picked:
            return
        # Merge + dedupe + sort chronologically (run ids = timestamps).
        new_list = sorted(set(self._runlists.get(name, [])) | set(picked))
        path = self._rl_path
        tag = f"addruns:{name}:{id(self)}"
        self._pending.add(tag)

        def do_set() -> None:
            _set_runlist_runs(path, name, new_list)
        self._db_worker.submit(tag, do_set)

    def _on_remove_runs(self) -> None:
        name = self._selected_name()
        if not name:
            return
        selected = [
            item.text() for item in self._runs.selectedItems()
        ]
        if not selected:
            QtWidgets.QMessageBox.information(
                self, "Remove runs",
                "Select one or more runs in the member list first.",
            )
            return
        confirm = QtWidgets.QMessageBox.question(
            self, "Remove runs",
            f"Remove {len(selected)} run(s) from runlist {name!r}?\n\n"
            "The runs themselves stay in the database.",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
        )
        if confirm != QtWidgets.QMessageBox.Yes:
            return
        kept = [r for r in self._runlists.get(name, []) if r not in selected]
        path = self._rl_path
        tag = f"rmruns:{name}:{id(self)}"
        self._pending.add(tag)

        def do_set() -> None:
            _set_runlist_runs(path, name, kept)
        self._db_worker.submit(tag, do_set)

    def _on_rename(self) -> None:
        old = self._selected_name()
        if not old:
            return
        new, ok = QtWidgets.QInputDialog.getText(
            self, "Rename runlist",
            f"New name for {old!r}:",
            QtWidgets.QLineEdit.Normal,
            old,
        )
        if not ok or not new.strip() or new.strip() == old:
            return
        path = self._rl_path
        new_name = new.strip()
        tag = f"rename:{old}->{new_name}:{id(self)}"
        self._pending.add(tag)

        def do_rename() -> None:
            _rename_runlist(path, old, new_name)
        self._db_worker.submit(tag, do_rename)

    def _on_delete(self) -> None:
        name = self._selected_name()
        if not name:
            return
        confirm = QtWidgets.QMessageBox.question(
            self, "Delete runlist",
            f"Delete the runlist {name!r}?\n\n"
            "Only the named selection is removed — the runs themselves "
            "stay in the database.",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
        )
        if confirm != QtWidgets.QMessageBox.Yes:
            return
        path = self._rl_path
        tag = f"del:{name}:{id(self)}"
        self._pending.add(tag)

        def do_del() -> None:
            _delete_runlist(path, name)
        self._db_worker.submit(tag, do_del)

    def _on_db_worker_done(self, tag: str, ok: bool, error: str) -> None:
        if tag not in self._pending:
            return
        self._pending.discard(tag)
        if not ok:
            QtWidgets.QMessageBox.warning(
                self, "Runlist write failed",
                error.splitlines()[0],
            )
            return
        self.reload()


class _RunPickerDialog(QtWidgets.QDialog):
    """Multi-select picker for adding runs to a runlist.

    Lists every run id in the matching database; runs already in
    the target runlist are tagged "(already in)" + disabled so the
    operator can't double-add them.  Filter input narrows the list
    by substring.
    """

    def __init__(
        self,
        parent: QtWidgets.QWidget | None,
        candidates: list[str],
        already_in: set[str],
        target: str,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle(f"Add runs to {target!r}")
        self.resize(420, 520)

        v = QtWidgets.QVBoxLayout(self)
        v.setContentsMargins(12, 12, 12, 12)
        v.setSpacing(6)

        head = QtWidgets.QLabel(
            f"Pick run(s) to add to <b>{target}</b>. "
            f"Multi-select with Cmd/Ctrl-click."
        )
        head.setTextFormat(QtCore.Qt.RichText)
        head.setWordWrap(True)
        v.addWidget(head)

        self._filter = QtWidgets.QLineEdit()
        self._filter.setPlaceholderText("filter run ids…")
        self._filter.textChanged.connect(self._apply_filter)
        v.addWidget(self._filter)

        self._list = QtWidgets.QListWidget()
        self._list.setSelectionMode(
            QtWidgets.QAbstractItemView.ExtendedSelection
        )
        self._list.setAlternatingRowColors(True)
        self._list.setStyleSheet(
            "QListWidget { border: none; }"
            " QListWidget::item { padding: 3px 8px;"
            "   font-family: 'Menlo','Consolas',monospace; }"
        )
        v.addWidget(self._list, 1)

        self._candidates = candidates
        self._already_in = already_in
        self._apply_filter("")

        buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        v.addWidget(buttons)

    def _apply_filter(self, text: str) -> None:
        needle = text.strip().lower()
        self._list.clear()
        for run_id in self._candidates:
            if needle and needle not in run_id.lower():
                continue
            label = run_id
            it = QtWidgets.QListWidgetItem(label)
            if run_id in self._already_in:
                it.setText(f"{run_id}    (already in)")
                it.setFlags(it.flags() & ~QtCore.Qt.ItemIsSelectable)
                it.setForeground(QtGui.QBrush(QtGui.QColor("#6B6968")))
            self._list.addItem(it)

    def picked(self) -> list[str]:
        out: list[str] = []
        for it in self._list.selectedItems():
            # Strip the "(already in)" suffix defensively (those are
            # marked non-selectable so this should be empty anyway).
            txt = it.text().split()[0]
            if txt not in self._already_in:
                out.append(txt)
        return out

    @classmethod
    def pick(
        cls,
        parent: QtWidgets.QWidget | None,
        candidates: list[str],
        already_in: set[str],
        target: str,
    ) -> Optional[list[str]]:
        """Run the dialog modally; return picked ids or None on cancel."""
        dlg = cls(parent, candidates, already_in, target)
        if dlg.exec() != QtWidgets.QDialog.Accepted:
            return None
        return dlg.picked()


__all__ = ["RunlistsView"]
