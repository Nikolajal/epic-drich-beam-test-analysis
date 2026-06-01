"""Data-inspect preview pane.

Shown in the Run Manager when a run is selected.  Surfaces:

  - which ``.root`` files exist under ``Data/<run>/`` with their
    size + last-modified time;
  - whether ``fine_calib.{txt,toml}`` is present;
  - per-file "Show params" affordance that opens the embedded
    ``Config/`` metadata (the writers stash their input TOML into
    the ROOT file's ``Config`` directory for traceability) in a
    small popup, read via uproot.

Reading the Config tree requires uproot but is best-effort — if
the file has no ``Config/`` directory we just say so.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Optional

from PySide6 import QtCore, QtGui, QtWidgets


# Files the dashboard recognises and labels.  Anything else found in
# the run dir is listed too, just without a friendly label.
_KNOWN_FILES: dict[str, str] = {
    "lightdata.root":         "Lightdata (framer + trigger + Hough)",
    "recodata.root":          "Reconstruction (ring refinement, σ panels)",
    "recotrackdata.root":     "Track-matched data",
    "pulser_calib_qa.root":   "Pulser-calibration QA",
    "fine_calib.toml":        "Fine calibration",
}


class DataInspectPane(QtWidgets.QFrame):
    """Files-and-params summary for the selected run."""

    def __init__(self, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        self._run_dir: Optional[Path] = None

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(12, 10, 12, 12)
        outer.setSpacing(8)

        head = QtWidgets.QLabel("Run preview")
        head_font = head.font()
        head_font.setBold(True)
        head_font.setPointSize(head_font.pointSize() + 2)
        head.setFont(head_font)
        outer.addWidget(head)

        self._caption = QtWidgets.QLabel("(no run selected)")
        self._caption.setObjectName("muted")
        outer.addWidget(self._caption)

        self._files_holder = QtWidgets.QWidget()
        self._files_layout = QtWidgets.QVBoxLayout(self._files_holder)
        self._files_layout.setContentsMargins(0, 4, 0, 0)
        self._files_layout.setSpacing(4)
        outer.addWidget(self._files_holder)

    # ----- public API -----------------------------------------------------

    def set_run(self, run_dir: Optional[Path]) -> None:
        self._run_dir = run_dir
        self._refresh()

    # ----- internals ------------------------------------------------------

    def _refresh(self) -> None:
        # Clear existing rows.
        while self._files_layout.count():
            item = self._files_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()

        if self._run_dir is None or not self._run_dir.is_dir():
            self._caption.setText("(no run selected)")
            return

        # Canonical files only — keep the preview focused.
        entries: list[tuple[str, Optional[Path]]] = []
        for name in _KNOWN_FILES:
            p = self._run_dir / name
            entries.append((name, p if p.is_file() else None))

        present = sum(1 for _, p in entries if p is not None)
        self._caption.setText(
            f"{self._run_dir.name}  ·  {present} / {len(entries)} known artefact"
            f"{'s' if len(entries) != 1 else ''}"
        )

        for name, path in entries:
            self._files_layout.addWidget(self._build_row(name, path))

    def _build_row(self, name: str, path: Optional[Path]) -> QtWidgets.QWidget:
        row = QtWidgets.QFrame()
        h = QtWidgets.QHBoxLayout(row)
        h.setContentsMargins(2, 2, 2, 2)
        h.setSpacing(8)

        dot = QtWidgets.QLabel("●")
        dot.setStyleSheet(
            "color: #0BDA51; font-size: 14px;" if path is not None
            else "color: #6B6968; font-size: 14px;"
        )
        h.addWidget(dot)

        label = QtWidgets.QLabel(name)
        label.setStyleSheet("font-family: 'Menlo','Consolas',monospace;")
        h.addWidget(label)

        friendly = _KNOWN_FILES.get(name)
        if friendly:
            desc = QtWidgets.QLabel(friendly)
            desc.setObjectName("muted")
            desc.setStyleSheet("font-size: 11px;")
            h.addWidget(desc)
        h.addStretch(1)

        if path is not None:
            stat = path.stat()
            meta = QtWidgets.QLabel(
                f"{_human_size(stat.st_size)}  ·  {time.strftime('%Y-%m-%d %H:%M', time.localtime(stat.st_mtime))}"
            )
            meta.setObjectName("muted")
            meta.setStyleSheet("font-size: 11px;")
            h.addWidget(meta)

            if name.endswith(".root"):
                btn = QtWidgets.QPushButton("Show params")
                btn.setToolTip(
                    "Read the Config/ directory embedded in this ROOT file by the writer."
                )
                btn.clicked.connect(lambda _checked=False, p=path: self._show_params(p))
                h.addWidget(btn)
        else:
            absent = QtWidgets.QLabel("missing")
            absent.setObjectName("muted")
            absent.setStyleSheet("font-size: 11px; font-style: italic;")
            h.addWidget(absent)
        return row

    def _show_params(self, root_path: Path) -> None:
        """Pop up the embedded Config/ tree from ``root_path``."""
        try:
            import uproot  # type: ignore
        except ImportError:
            QtWidgets.QMessageBox.warning(
                self, "uproot missing",
                "Reading embedded params needs the `uproot` Python package "
                "(already in qa_quicklook/requirements.txt — try "
                "`./scripts/qa_quicklook --reinstall`).",
            )
            return

        try:
            f = uproot.open(root_path)
            config_dir = f.get("Config") or f.get("config")
        except Exception as exc:  # noqa: BLE001
            QtWidgets.QMessageBox.warning(
                self, "Could not read file", f"{root_path.name}: {exc}",
            )
            return

        if config_dir is None:
            QtWidgets.QMessageBox.information(
                self, "No embedded params",
                f"{root_path.name} doesn't carry a Config/ tree — older writer "
                "or this stage doesn't write one.",
            )
            return

        # Parse every Config/ entry into (clean_key, value, kind).
        # ``kind`` is one of ``scalar`` (TParameter) / ``conf_pointer``
        # (TNamed with a path-like name) / ``toml_payload`` (TNamed
        # with a *_toml name).
        scalar_rows: list[tuple[str, object]] = []
        pointer_rows: list[tuple[str, str]] = []
        toml_rows: list[tuple[str, str]] = []
        try:
            for key in config_dir.keys():
                clean = key.split(";", 1)[0]
                node = config_dir[key]
                value = _extract_config_value(node)
                if clean.endswith("_toml"):
                    toml_rows.append((clean, str(value)))
                elif clean.endswith("_conf_file"):
                    pointer_rows.append((clean, str(value)))
                else:
                    scalar_rows.append((clean, value))
        except Exception as exc:  # noqa: BLE001
            scalar_rows = [(f"(error walking Config/)", str(exc))]

        body_lines: list[str] = [f"# Embedded params from {root_path.name}", ""]
        if scalar_rows:
            body_lines.append("[parameters]")
            width = max(len(k) for k, _ in scalar_rows)
            for k, v in scalar_rows:
                body_lines.append(f"  {k.ljust(width)} = {v}")
            body_lines.append("")
        if pointer_rows:
            body_lines.append("[source configs]")
            width = max(len(k) for k, _ in pointer_rows)
            for k, v in pointer_rows:
                body_lines.append(f"  {k.ljust(width)} = {v}")
            body_lines.append("")
        if toml_rows:
            body_lines.append("[toml payloads]")
            for k, v in toml_rows:
                body_lines.append(f"  --- {k} ---")
                for ln in v.splitlines():
                    body_lines.append(f"  {ln}")
                body_lines.append("")

        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle(f"Params — {root_path.name}")
        dialog.resize(720, 520)
        v = QtWidgets.QVBoxLayout(dialog)
        text = QtWidgets.QPlainTextEdit()
        text.setReadOnly(True)
        font = QtGui.QFont("Menlo")
        font.setStyleHint(QtGui.QFont.Monospace)
        font.setPointSize(11)
        text.setFont(font)
        text.setPlainText("\n".join(body_lines))
        v.addWidget(text)
        btn_row = QtWidgets.QHBoxLayout()
        btn_row.addStretch(1)
        close = QtWidgets.QPushButton("Close")
        close.clicked.connect(dialog.accept)
        btn_row.addWidget(close)
        v.addLayout(btn_row)
        dialog.exec()


def _human_size(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024
    return f"{n:.1f} TB"


def _extract_config_value(node) -> object:
    """Pull the actual value out of a ROOT Config/ object.

    Handles the two shapes the writers embed:

      - ``TParameter<int|double|...>`` → ``.member('fVal')`` gives
        the numeric value;
      - ``TNamed`` → ``.member('fTitle')`` gives the string payload
        (this is what's used for ``*_conf_file`` pointers and the
        ``*_toml`` raw bodies).

    Falls back to a one-line ``repr`` if neither shape matches.
    """
    for attr in ("fVal",):
        try:
            v = node.member(attr)
            if v is not None:
                return v
        except Exception:
            pass
    for attr in ("fTitle", "title"):
        try:
            v = node.member(attr) if hasattr(node, "member") else getattr(node, attr, None)
            if callable(v):
                v = v()
            if v:
                return v
        except Exception:
            continue
    return repr(node)[:120]


__all__ = ["DataInspectPane"]
