"""Run-overview card widget.

Renders the headline summary for the selected run: ring σ, recodata
entry count, trigger count, plus a small status line (beam, V_bias,
quality).  Reads ``Data/<run>/recodata.root`` through
:func:`qa_quicklook.summary.read_summary`; when the file isn't there
yet shows a "run recodata first" placeholder instead of fields.

This is the *post-run* view.  The live-metrics table below it shows
what's coming out of the *currently-running* writer; when nothing is
running the card is the operator's "is this run good" glance.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from PySide6 import QtCore, QtGui, QtWidgets

from .catalog import Run
from .summary import Summary, read_summary


class RunOverviewCard(QtWidgets.QFrame):
    """One-card summary; refreshed on selection change or after a run."""

    def __init__(self, data_root: Path, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._data_root = data_root
        self._run: Optional[Run] = None
        self._summary: Optional[Summary] = None

        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        self.setStyleSheet(
            "RunOverviewCard { background: #f6f7f9; border: 1px solid #d9dce1;"
            " border-radius: 6px; }"
        )

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(10, 8, 10, 10)
        outer.setSpacing(6)

        # Title row: run id + a 'reload' chip.
        title_row = QtWidgets.QHBoxLayout()
        self._title = QtWidgets.QLabel("(no run selected)")
        self._title.setStyleSheet("font-weight: 600; font-size: 13px;")
        title_row.addWidget(self._title)
        title_row.addStretch(1)
        self._refresh_btn = QtWidgets.QToolButton()
        self._refresh_btn.setText("⟳")
        self._refresh_btn.setToolTip("Re-read recodata.root for this run")
        self._refresh_btn.clicked.connect(self.refresh)
        title_row.addWidget(self._refresh_btn)
        outer.addLayout(title_row)

        # Status line: beam · V_bias · quality.
        self._status = QtWidgets.QLabel()
        self._status.setStyleSheet("color: #555; font-size: 11px;")
        outer.addWidget(self._status)

        # Hero numbers row: three big values.
        hero_row = QtWidgets.QHBoxLayout()
        hero_row.setSpacing(16)
        self._sigma_cell = _HeroCell("σ (first ring)", "mm")
        self._entries_cell = _HeroCell("entries", "")
        self._triggers_cell = _HeroCell("triggers", "")
        hero_row.addWidget(self._sigma_cell)
        hero_row.addWidget(self._entries_cell)
        hero_row.addWidget(self._triggers_cell)
        hero_row.addStretch(1)
        outer.addLayout(hero_row)

        # Placeholder / error label, hidden when we have content.
        self._placeholder = QtWidgets.QLabel()
        self._placeholder.setStyleSheet("color: #888; font-style: italic; padding: 6px 0;")
        self._placeholder.setWordWrap(True)
        outer.addWidget(self._placeholder)

        self._apply_summary()

    # ----- public API -----------------------------------------------------

    def set_run(self, run: Optional[Run]) -> None:
        self._run = run
        self.refresh()

    def refresh(self) -> None:
        """Re-read recodata.root for the current run, redraw."""
        if self._run is None:
            self._summary = None
        else:
            self._summary = read_summary(self._data_root / self._run.run_id, self._run.run_id)
        self._apply_summary()

    # ----- internals ------------------------------------------------------

    def _apply_summary(self) -> None:
        run = self._run
        s = self._summary

        if run is None:
            self._title.setText("(no run selected)")
            self._status.clear()
            self._sigma_cell.clear()
            self._entries_cell.clear()
            self._triggers_cell.clear()
            self._placeholder.setText("Pick a run on the left to see its overview.")
            self._placeholder.show()
            self._refresh_btn.setEnabled(False)
            return

        self._refresh_btn.setEnabled(True)
        self._title.setText(run.run_id)

        bits: list[str] = []
        if run.beam_energy is not None:
            bits.append(f"beam={run.beam_energy}")
        if run.v_bias is not None:
            bits.append(f"V_bias={run.v_bias}")
        if run.temperature is not None:
            bits.append(f"T={run.temperature}")
        if run.quality:
            bits.append(f"quality={run.quality}")
        self._status.setText("  ·  ".join(bits) if bits else "(no metadata)")

        if s is None or not s.has_recodata:
            self._sigma_cell.clear()
            self._entries_cell.clear()
            self._triggers_cell.clear()
            self._placeholder.setText("recodata.root not produced yet — click Recodata or Full reco.")
            self._placeholder.show()
            return

        if s.error:
            self._sigma_cell.clear()
            self._entries_cell.clear()
            self._triggers_cell.clear()
            self._placeholder.setText(f"read error: {s.error}")
            self._placeholder.show()
            return

        self._placeholder.hide()
        if s.sigma_mm is None:
            self._sigma_cell.set_missing("summary histo absent")
        else:
            self._sigma_cell.set_value(f"{s.sigma_mm:.2f}")
            if s.sigma_by_bin:
                tooltip = "\n".join(f"{k:>22}  {v:6.3f} mm" for k, v in s.sigma_by_bin.items())
                self._sigma_cell.setToolTip(tooltip)
        if s.n_entries is None:
            self._entries_cell.set_missing("recodata tree absent")
        else:
            self._entries_cell.set_value(_format_count(s.n_entries))
        if s.n_triggers is None:
            self._triggers_cell.set_missing("trigger histo absent")
        else:
            self._triggers_cell.set_value(_format_count(s.n_triggers))


class _HeroCell(QtWidgets.QFrame):
    """One labelled big number inside the overview card."""

    def __init__(self, label: str, unit: str, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._unit = unit

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(4, 2, 4, 2)
        layout.setSpacing(0)

        self._value = QtWidgets.QLabel("–")
        big = self._value.font()
        big.setPointSize(big.pointSize() + 6)
        big.setBold(True)
        self._value.setFont(big)

        self._sub = QtWidgets.QLabel(label + (f"  [{unit}]" if unit else ""))
        self._sub.setStyleSheet("color: #666; font-size: 10px;")

        layout.addWidget(self._value)
        layout.addWidget(self._sub)

    def set_value(self, text: str) -> None:
        self._value.setText(text)
        self._value.setStyleSheet("color: #222;")
        self.setToolTip("")

    def set_missing(self, tooltip: str) -> None:
        self._value.setText("–")
        self._value.setStyleSheet("color: #aaa;")
        self.setToolTip(tooltip)

    def clear(self) -> None:
        self._value.setText("–")
        self._value.setStyleSheet("color: #aaa;")
        self.setToolTip("")


def _format_count(n: int) -> str:
    """Compact integer rendering (1234567 → '1.23 M')."""
    if n < 1_000:
        return str(n)
    if n < 1_000_000:
        return f"{n / 1_000:.1f} k"
    if n < 1_000_000_000:
        return f"{n / 1_000_000:.2f} M"
    return f"{n / 1_000_000_000:.2f} G"


__all__ = ["RunOverviewCard"]
