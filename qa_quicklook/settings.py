"""Settings tab — top-level UI for editing ``conf/*.toml`` from the panel.

Layout
------

One long scrolling page.  At the very top, the **setting-set picker**
(active set, switch dropdown, "save as set…").  Below it, every
configuration file is stacked vertically — each as its own section
with a filename header, a per-file status line, an inline conflict
banner, and the file's bubble-card form.  No left-side file picker;
the whole conf surface is browsable by scrolling.

Edit flow (per file, independent)
---------------------------------

1. At construction, every ``conf/*.toml`` (plus the optional
   dashboard-config extras) is parsed once and rendered.
2. Editing a widget emits ``value_changed(path, value)`` from that
   file's ``TomlForm``; we apply the change to the file's in-memory
   ``TOMLDocument`` and arm a 500 ms debounce timer.  Subsequent edits
   on the same file reset that file's timer.
3. When the timer fires we ``tomlkit.dumps`` the doc and write
   atomically (``write_text`` to a sibling ``.tmp`` then
   ``os.replace``).  Master-symlink files are promoted into the
   working overlay first so the pristine defaults stay untouched.
4. ``QFileSystemWatcher`` + a 1 s mtime poll re-read whichever files
   change underneath us.  A change while we have unsaved edits raises
   that file's per-section "Reload from disk / Keep my edits" banner;
   a change with no pending edits silently reloads in place.
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import tomlkit
from PySide6 import QtCore, QtGui, QtWidgets

from . import conf_layout, readout_validate, theme
from .conf_layout import MasterKind, active_set_name, list_sets, promote_to_working, scan, switch_set
from .toml_form import TableLayout, TomlForm
from .toml_model import apply_double_hash_cutoff, roundtrip_safe, set_leaf, split_at_double_hash


SAVE_DEBOUNCE_MS = 500
POLL_FALLBACK_MS = 1000


# ---------------------------------------------------------------------------
# File presentation: human-readable titles + order in the scroll page.
#
# Order is "frequently tuned at top → rarely changed at bottom".  Files
# whose ``is_app_settings`` flag is True are pinned to the bottom of
# the page regardless of the order they're discovered in.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class _FilePresentation:
    basename: str
    title: str
    is_app_settings: bool = False


FILE_PRESENTATION: tuple[_FilePresentation, ...] = (
    _FilePresentation("streaming.toml",        "Streaming Trigger & Hough"),
    _FilePresentation("recodata.toml",         "Reconstruction"),
    _FilePresentation("framer_conf.toml",      "Streaming Framer"),
    _FilePresentation("readout_config.toml",   "Readout"),
    _FilePresentation("trigger_conf.toml",     "Triggers"),
    _FilePresentation("mapping_conf.toml",     "Mapping"),
    #  Lives at conf/calib/calibration_conf.toml (no defaults/sets/working
    #  layout) — surfaced as an ``extra_files`` injection in app.py.
    _FilePresentation("calibration_conf.toml", "Calibration"),
    _FilePresentation("qa_quicklook.toml",     "App Settings", is_app_settings=True),
)


def _presentation_index() -> dict[str, _FilePresentation]:
    return {p.basename: p for p in FILE_PRESENTATION}


# Per-file dropdown registries: paths whose value should be a closed
# choice list rather than a free-text edit.  The dashboard widgets
# consult this when rendering ``str`` leaves.
ENUM_FIELDS: dict[str, dict[tuple, tuple[str, ...]]] = {
    "qa_quicklook.toml": {
        ("ui", "theme"):       ("system", "light", "dark"),
        # plots_theme is intentionally separate from ``theme`` so the
        # operator can run the dashboard in a dark UI but force the
        # QA plots to render light (or vice versa) when paper /
        # projector / monitor-glare conditions differ.
        ("ui", "plots_theme"): ("follow", "light", "dark"),
    },
}


# Per-file multi-column-table layouts: paths whose Table value is a
# key→array relation we want to render as a proper relation widget
# (chip grid or QTableWidget) instead of bubble cells.
TABLE_LAYOUTS: dict[str, dict[tuple, TableLayout]] = {
    "mapping_conf.toml": {
        ("pdu_xy_position",): TableLayout(
            key_columns=("PDU",),
            value_columns=("X", "Y"),     # short labels — chips show "X" / "Y" inline
            render_mode="chips",          # 8 entries → dense chip row
        ),
        ("device_chip_to_pdu_matrix",): TableLayout(
            key_columns=("Device", "Chip"),
            key_splitter="_",
            value_columns=("PDU", "Quadrant"),
            render_mode="master_detail",  # device on the left, chips on the right
        ),
    },
}


# Section-title overrides — when the auto-prettifier doesn't capture
# proper-noun capitalisation ("Hough" the surname, "Cherenkov", …).
# Anything *not* in this map is auto-prettified.  Keys are tuples
# matching the leaf path (use ``()`` for top-level sections).
SECTION_TITLES: dict[str, dict[tuple, str]] = {
    "streaming.toml": {
        ("streaming_trigger",): "Streaming Trigger",
        ("streaming_hough",):   "Streaming Hough",
    },
    "recodata.toml": {
        ("recodata",): "Recodata",
    },
}


# Per-leaf description registry — curated operator-facing text.
# **Wins** over the inline TOML comment (which is developer doc).
# When no entry is present and no inline comment exists, the cell
# simply has no description.
PARAM_DESCRIPTIONS: dict[str, dict[tuple, str]] = {
    "recodata.toml": {
        ("recodata", "n_phi_bins_coverage"):         "Number of azimuthal slices of the coverage map (1° per bin at 360).",
        ("recodata", "n_r_bins_coverage"):           "Number of radial bins in the coverage map.",
        ("recodata", "r_min_coverage_mm"):           "Lower radius of the coverage map.",
        ("recodata", "r_max_coverage_mm"):           "Upper radius of the coverage map.",
        ("recodata", "channel_half_width_mm"):       "Pixel half-side (SiPM pitch / 2). Drives coverage rasterisation.",
        ("recodata", "nominal_centre_x_mm"):         "Nominal beam-axis projection on the detector plane (X).",
        ("recodata", "nominal_centre_y_mm"):         "Nominal beam-axis projection on the detector plane (Y).",
        ("recodata", "delta_r_for_coverage_mm"):     "Ring bandwidth — a channel counts as on-ring when |r_ch − R| < this.",
        ("recodata", "min_hits_per_ring"):           "Minimum hits required for a ring to enter per-ring statistics.",
        ("recodata", "min_channel_r_for_coverage_mm"): "Channels closer to the centre than this are excluded from the coverage map.",
    },
    "framer_conf.toml": {
        ("framer", "frame_size"):                    "Clock cycles per frame (320 MHz clock → 3.125 ns/cc).",
        ("framer", "first_frames_trigger"):          "Frames at start of spill reserved for the noise sample.",
        ("framer", "afterpulse_deadtime"):           "Mask window after a hit to suppress the same-channel afterpulse.",
        ("framer", "trigger_secondary_window"):      "Window after a primary trigger in which a secondary still counts.",
        ("qa", "afterpulse_near_lo"):                "Near-band lower edge for the afterpulse QA window.",
        ("qa", "afterpulse_near_hi"):                "Near-band upper edge (mirror of the deadtime).",
        ("qa", "afterpulse_sideband_offset"):        "Far-band start — sideband well past the afterpulse tail.",
        ("qa", "ct_scan_dt_min"):                    "Lower Δt for the cross-talk scan loop.",
        ("qa", "ct_scan_dt_max"):                    "Upper Δt for the cross-talk scan loop.",
        ("qa", "ct_phys_signal_lo"):                 "Physical cross-talk window lower edge.",
        ("qa", "ct_phys_signal_hi"):                 "Physical cross-talk window upper edge.",
        ("qa", "ct_elec_signal_lo"):                 "Electrical cross-talk window lower edge.",
        ("qa", "ct_elec_signal_hi"):                 "Electrical cross-talk window upper edge.",
    },
    "streaming.toml": {
        ("streaming_trigger", "time_window_ns"):     "Sliding-window width used by the streaming score.",
        ("streaming_trigger", "n_sigma_threshold"):  "Fire threshold for the streaming score, in σ above noise mean.",
        ("streaming_trigger", "min_noise_hits"):     "Min noise-sample hits required for a channel to enter the weight bundle.",
        ("streaming_hough", "r_min"):                "Lower bound of the Hough radius scan.",
        ("streaming_hough", "r_max"):                "Upper bound of the Hough radius scan.",
        ("streaming_hough", "r_step"):               "Hough radius granularity (halved for sub-cell aggregation).",
        ("streaming_hough", "cell_size"):            "XY accumulator cell size (halved like r_step).",
        ("streaming_hough", "centre_padding_mm"):    "Padding around the centre search window; −1.0 falls back to r_max.",
        ("streaming_hough", "threshold_fraction"):   "Relative floor: minimum vote fraction of the currently-leading peak.",
        ("streaming_hough", "min_hits_slack"):       "Ring-acceptance slack on the absolute vote count.",
        ("streaming_hough", "hough_threshold_fraction"): "Hough entry gate, as a fraction of active channels.",
        ("streaming_hough", "collection_radius"):    "Ring band width for hit assignment.",
        ("streaming_hough", "centre_xy_half_range_mm"): "Half-range of the centre search box.",
        ("streaming_hough", "aggregation_window_cells"): "Sub-cell aggregation window (1 = single-cell, 2 = aggregated).",
        ("streaming_hough", "fit_circle_init_x"):    "Initial centre X for fit_circle (legacy; refinement runs in recodata).",
        ("streaming_hough", "fit_circle_init_y"):    "Initial centre Y for fit_circle (legacy; refinement runs in recodata).",
        ("streaming_hough", "fit_circle_init_r"):    "Initial radius for fit_circle (legacy; refinement runs in recodata).",
    },
}


# Curated unit suffixes — for keys whose physical unit isn't captured
# by the name (e.g. ``frame_size`` is in clock cycles "cc").  When
# absent here AND not detectable from a ``_mm`` / ``_ns`` / … suffix,
# numeric leaves render with "a.u." (arbitrary units) so the suffix
# slot stays consistent across the page.
PARAM_UNITS: dict[str, dict[tuple, str]] = {
    "framer_conf.toml": {
        ("framer", "frame_size"):                    "cc",
        ("framer", "first_frames_trigger"):          "frames",
        ("framer", "afterpulse_deadtime"):           "cc",
        ("framer", "trigger_secondary_window"):      "cc",
        ("qa", "afterpulse_near_lo"):                "cc",
        ("qa", "afterpulse_near_hi"):                "cc",
        ("qa", "afterpulse_sideband_offset"):        "cc",
        ("qa", "ct_scan_dt_min"):                    "cc",
        ("qa", "ct_scan_dt_max"):                    "cc",
        ("qa", "ct_phys_signal_lo"):                 "cc",
        ("qa", "ct_phys_signal_hi"):                 "cc",
        ("qa", "ct_elec_signal_lo"):                 "cc",
        ("qa", "ct_elec_signal_hi"):                 "cc",
    },
    "streaming.toml": {
        ("streaming_trigger", "n_sigma_threshold"):  "σ",
        ("streaming_trigger", "min_noise_hits"):     "hits",
        ("streaming_hough", "r_min"):                "mm",
        ("streaming_hough", "r_max"):                "mm",
        ("streaming_hough", "r_step"):               "mm",
        ("streaming_hough", "cell_size"):            "mm",
        ("streaming_hough", "collection_radius"):    "mm",
        ("streaming_hough", "threshold_fraction"):   "",   # fraction → a.u. is misleading; blank suffix
        ("streaming_hough", "hough_threshold_fraction"): "",
        ("streaming_hough", "min_hits_slack"):       "hits",
        ("streaming_hough", "aggregation_window_cells"): "cells",
        ("streaming_hough", "fit_circle_init_x"):    "mm",
        ("streaming_hough", "fit_circle_init_y"):    "mm",
        ("streaming_hough", "fit_circle_init_r"):    "mm",
    },
    "recodata.toml": {
        ("recodata", "n_phi_bins_coverage"):         "bins",
        ("recodata", "n_r_bins_coverage"):           "bins",
        ("recodata", "min_hits_per_ring"):           "hits",
    },
}


@dataclass
class _FileState:
    """Per-file in-memory state."""

    path: Path
    doc: tomlkit.TOMLDocument
    text_on_disk: str
    last_mtime: float
    roundtrip_ok: bool
    save_timer: QtCore.QTimer = field(default=None)  # type: ignore[assignment]
    dirty: bool = False


# ---------------------------------------------------------------------------
# Per-file section — one filename header + form + inline conflict banner.
# ---------------------------------------------------------------------------


class _FileSection(QtWidgets.QWidget):
    """One file's render in the scrolling page.

    Owns the per-file UI affordances (header, status line, inline
    conflict banner, the TomlForm itself).  No file I/O — the parent
    ``SettingsView`` orchestrates loading + saving and pushes new
    state in via :meth:`set_state` / :meth:`show_conflict`.
    """

    value_changed = QtCore.Signal(Path, tuple, object)   # file, leaf path, new value
    doc_mutated = QtCore.Signal(Path)                     # structural change (add/remove)
    reload_requested = QtCore.Signal(Path)
    keep_requested = QtCore.Signal(Path)

    def __init__(self, path: Path, label: str, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._path = path

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(0, 28, 0, 4)
        outer.setSpacing(8)

        # Header — clickable toggle + status text (right).
        # Default state: collapsed.  Operator picks which files to
        # expand; a long Settings tab stays scannable.
        header_row = QtWidgets.QHBoxLayout()
        header_row.setContentsMargins(4, 0, 4, 0)
        self._label_text = label
        self._toggle = QtWidgets.QToolButton()
        # Sections expanded by default; still collapsible for the
        # operator to focus on one file at a time.
        self._toggle.setText(f"▾  {label}")
        self._toggle.setCheckable(True)
        self._toggle.setChecked(True)
        self._toggle.setStyleSheet(
            "QToolButton { border: none; text-align: left; padding: 4px 2px; }"
        )
        title_font = self._toggle.font()
        title_font.setPointSize(title_font.pointSize() + 3)
        title_font.setBold(True)
        self._toggle.setFont(title_font)
        self._toggle.toggled.connect(self._on_toggle)
        header_row.addWidget(self._toggle)
        header_row.addStretch(1)
        self._status = QtWidgets.QLabel()
        self._status.setObjectName("muted")
        header_row.addWidget(self._status)
        outer.addLayout(header_row)

        # Per-file conflict banner — hidden until the file changes on
        # disk while we have unsaved edits.
        self._banner = _Banner()
        self._banner.reload_clicked.connect(lambda: self.reload_requested.emit(self._path))
        self._banner.keep_clicked.connect(lambda: self.keep_requested.emit(self._path))
        self._banner.setVisible(False)
        outer.addWidget(self._banner)

        #  Per-file warning surface — separate from the reload/keep
        #  conflict banner so the two semantics don't fight for the
        #  same widget.  Used today for readout_config.toml's
        #  device-chip overlap check; future schema validations can
        #  hook the same pipe via ``set_warnings([...])``.
        self._warn_label = QtWidgets.QLabel()
        self._warn_label.setWordWrap(True)
        #  PlainText (not RichText) — warning lines are composed from
        #  TOML field names (device ids, chip lists, role labels) which
        #  the operator can in principle edit by hand.  Rendering them
        #  as HTML would leave a small injection surface; plain text +
        #  Unicode bullets gives the same visual without the risk.
        self._warn_label.setTextFormat(QtCore.Qt.PlainText)
        self._warn_label.setStyleSheet(
            "QLabel { background-color: rgba(192, 57, 43, 0.18);"
            "  color: #E74C3C; border: 1px solid #C0392B;"
            "  border-radius: 6px; padding: 8px 10px; }"
        )
        self._warn_label.setVisible(False)
        outer.addWidget(self._warn_label)

        # The form lives inside a body widget that toggles with the
        # header.  Loading still happens regardless of expanded state
        # so the status line stays accurate while collapsed.
        self._body = QtWidgets.QWidget()
        body_layout = QtWidgets.QVBoxLayout(self._body)
        body_layout.setContentsMargins(0, 0, 0, 0)
        self._form = TomlForm()
        self._form.value_changed.connect(
            lambda leaf_path, value: self.value_changed.emit(self._path, leaf_path, value)
        )
        self._form.doc_mutated.connect(lambda: self.doc_mutated.emit(self._path))
        body_layout.addWidget(self._form)
        self._body.setVisible(True)
        outer.addWidget(self._body)

    def _on_toggle(self, checked: bool) -> None:
        self._body.setVisible(checked)
        arrow = "▾" if checked else "▸"
        self._toggle.setText(f"{arrow}  {self._label_text}")

    # ----- public API -----------------------------------------------------

    def path(self) -> Path:
        return self._path

    def set_warnings(self, warnings: list[str]) -> None:
        """Show / hide the per-file validation warning banner.

        ``warnings`` is a list of human-readable lines (one per
        problem).  Empty list hides the banner.  Lives next to the
        on-disk conflict banner but uses its own colours + widget so
        the two never fight over visibility.
        """
        if not warnings:
            self._warn_label.setVisible(False)
            self._warn_label.clear()
            return
        #  Plain-text layout — see _warn_label's setTextFormat note.
        #  Newline-joined "⚠ <line>" bullets read identically without
        #  the HTML injection vector.
        bullets = "\n".join(f"⚠ {w}" for w in warnings)
        self._warn_label.setText("Validation warning\n" + bullets)
        self._warn_label.setVisible(True)

    def render(
        self,
        doc: tomlkit.TOMLDocument,
        *,
        editable: bool,
        enums: dict[tuple, tuple[str, ...]] | None = None,
        table_layouts: dict[tuple, "TableLayout"] | None = None,
        section_titles: dict[tuple, str] | None = None,
        param_descriptions: dict[tuple, str] | None = None,
        param_units: dict[tuple, str] | None = None,
    ) -> None:
        self._form.load(
            doc,
            editable=editable,
            enums=enums,
            table_layouts=table_layouts,
            section_titles=section_titles,
            param_descriptions=param_descriptions,
            param_units=param_units,
        )

    def set_status(self, text: str) -> None:
        self._status.setText(text)

    def show_conflict(self) -> None:
        self._banner.show_conflict(self._path.name)
        self._banner.setVisible(True)

    def hide_conflict(self) -> None:
        self._banner.setVisible(False)

    def show_parse_error(self, message: str) -> None:
        self._banner.show_error(self._path.name, message)
        self._banner.setVisible(True)


# ---------------------------------------------------------------------------
# Main view
# ---------------------------------------------------------------------------


class SettingsView(QtWidgets.QWidget):
    """Settings tab; pass the absolute ``conf/`` directory in."""

    def __init__(
        self,
        conf_dir: Path,
        parent: QtWidgets.QWidget | None = None,
        *,
        extra_files: list[Path] | None = None,
    ) -> None:
        super().__init__(parent)
        self._conf_dir = conf_dir.resolve()
        # Files outside ``conf_dir`` to also expose — used for the
        # dashboard's own ``qa_quicklook.toml`` (theme, advanced QA
        # toggle, rsync source, …).  They render above the analysis
        # configs.
        self._extra_files: list[Path] = [p.resolve() for p in (extra_files or [])]
        self._files: dict[Path, _FileState] = {}
        self._sections: dict[Path, _FileSection] = {}

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(8, 8, 8, 8)
        outer.setSpacing(8)

        # Top: setting-set picker (applies to the conf/ tree only).
        self._set_picker = _SettingSetPicker(self._conf_dir)
        self._set_picker.set_chosen.connect(self._on_set_chosen)
        self._set_picker.save_as_set_requested.connect(self._on_save_as_set_requested)
        outer.addWidget(self._set_picker)

        # The big scrolling page where every file's bubbles live.
        self._scroll = QtWidgets.QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        outer.addWidget(self._scroll, 1)

        self._page = QtWidgets.QWidget()
        self._page_layout = QtWidgets.QVBoxLayout(self._page)
        self._page_layout.setContentsMargins(8, 8, 8, 16)
        self._page_layout.setSpacing(0)   # _FileSection owns its own top margin
        self._page_layout.setAlignment(QtCore.Qt.AlignTop)
        self._scroll.setWidget(self._page)

        # Disk watcher: directory (for arrivals/removals) + each known file.
        self._watcher = QtCore.QFileSystemWatcher(self)
        self._watcher.fileChanged.connect(self._on_file_changed_on_disk)
        self._watcher.directoryChanged.connect(self._on_dir_changed)

        # Cheap mtime poll for filesystems where the watcher misses events.
        self._poll = QtCore.QTimer(self)
        self._poll.setInterval(POLL_FALLBACK_MS)
        self._poll.timeout.connect(self._poll_all)
        self._poll.start()

        self._rebuild_sections()

    # ----- discovery + section construction ------------------------------

    def _discover_files(self) -> list[tuple[Path, str]]:
        """Return ``[(path, header_label)]`` in display order.

        The curated ``FILE_PRESENTATION`` table is the source of truth
        for ordering and labels.  Any TOML found under ``conf/`` or
        ``extra_files`` that isn't listed there is appended in
        alphabetical order *before* the app-settings group, so an
        unfamiliar file shows up automatically without breaking the
        bottom-anchor for App Settings.
        """
        catalog: dict[str, Path] = {}
        if self._conf_dir.is_dir():
            for p in sorted(self._conf_dir.glob("*.toml")):
                catalog[p.name] = p
        for p in self._extra_files:
            if p.is_file():
                catalog[p.name] = p

        index = _presentation_index()
        ordered: list[tuple[Path, str]] = []
        app_settings: list[tuple[Path, str]] = []
        seen: set[str] = set()
        for pres in FILE_PRESENTATION:
            path = catalog.get(pres.basename)
            if path is None:
                continue
            entry = (path, pres.title)
            (app_settings if pres.is_app_settings else ordered).append(entry)
            seen.add(pres.basename)

        # Files we don't have a curated entry for — slot above App
        # Settings, alphabetically.
        leftover = sorted(name for name in catalog if name not in seen)
        for name in leftover:
            ordered.append((catalog[name], name))

        return ordered + app_settings

    def _rebuild_sections(self) -> None:
        """(Re-)create the page from scratch.

        Cheap to call: parses every file, builds one ``_FileSection``
        per file and stacks them in the scrolling page.  Called at
        construction, after ``switch_set``, and when files appear /
        disappear in ``conf/``.
        """
        # Clear existing sections.
        for sec in self._sections.values():
            self._page_layout.removeWidget(sec)
            sec.deleteLater()
        self._sections.clear()
        self._files.clear()

        files = self._discover_files()

        # Refresh the watcher with the current files (and the conf dir).
        if self._conf_dir.is_dir() and self._conf_dir.as_posix() not in self._watcher.directories():
            self._watcher.addPath(self._conf_dir.as_posix())
        wanted = {p.as_posix() for p, _ in files}
        current = set(self._watcher.files())
        to_add = wanted - current
        to_drop = current - wanted
        if to_add:
            self._watcher.addPaths(sorted(to_add))
        if to_drop:
            self._watcher.removePaths(sorted(to_drop))

        # Build sections.
        for path, label in files:
            section = _FileSection(path, label)
            section.value_changed.connect(self._on_value_changed)
            section.doc_mutated.connect(self._on_doc_mutated)
            section.reload_requested.connect(self._on_section_reload)
            section.keep_requested.connect(self._on_section_keep)
            self._page_layout.addWidget(section)
            self._sections[path] = section
            self._load_into_section(path, section)

        self._page_layout.addStretch(1)

    def _load_into_section(self, path: Path, section: _FileSection) -> None:
        """Parse ``path``, store state, render into ``section``.

        The displayed document is the file truncated at the first
        ``##`` marker line — same convention the C++ parser uses, so
        the dashboard form matches what the analysis actually reads.
        Writes still go through the full original text path; only the
        *rendered* form is post-cutoff.
        """
        try:
            text = path.read_text()
            mtime = path.stat().st_mtime
            display_text = apply_double_hash_cutoff(text)
            doc = tomlkit.parse(display_text)
        except Exception as exc:  # noqa: BLE001
            self._files.pop(path, None)
            section.render(tomlkit.parse(""), editable=False, enums={})
            section.show_parse_error(str(exc))
            section.set_status("parse error")
            return

        # Only the pre-cutoff portion is editable; round-trip safety is
        # checked on that portion alone (anything past ``##`` is text
        # we splice back verbatim and never feed through tomlkit).
        rtok = roundtrip_safe(display_text)

        existing = self._files.get(path)
        if existing is None:
            timer = QtCore.QTimer(self)
            timer.setSingleShot(True)
            timer.setInterval(SAVE_DEBOUNCE_MS)
            timer.timeout.connect(lambda p=path: self._save_to_disk(p))
            state = _FileState(
                path=path,
                doc=doc,
                text_on_disk=text,
                last_mtime=mtime,
                roundtrip_ok=rtok,
                save_timer=timer,
            )
        else:
            state = existing
            state.doc = doc
            state.text_on_disk = text
            state.last_mtime = mtime
            state.roundtrip_ok = rtok
            state.dirty = False
        self._files[path] = state

        section.hide_conflict()
        enums = ENUM_FIELDS.get(path.name, {})
        table_layouts = TABLE_LAYOUTS.get(path.name, {})
        section_titles = SECTION_TITLES.get(path.name, {})
        param_descriptions = PARAM_DESCRIPTIONS.get(path.name, {})
        param_units = PARAM_UNITS.get(path.name, {})
        section.render(
            state.doc,
            editable=state.roundtrip_ok,
            enums=enums,
            table_layouts=table_layouts,
            section_titles=section_titles,
            param_descriptions=param_descriptions,
            param_units=param_units,
        )
        self._refresh_status(state)
        self._refresh_validation(state)

    def _refresh_status(self, state: _FileState) -> None:
        section = self._sections.get(state.path)
        if section is None:
            return
        bits: list[str] = []
        mtime_str = time.strftime("%H:%M:%S", time.localtime(state.last_mtime))
        bits.append(f"mtime {mtime_str}")
        if not state.roundtrip_ok:
            bits.append("⚠ not round-trip safe — editing disabled")
        elif state.dirty:
            bits.append("• unsaved")
        section.set_status("   ·   ".join(bits))

    # ----- edits + write-back --------------------------------------------

    def _on_doc_mutated(self, path: Path) -> None:
        """A sub-widget mutated the tomlkit doc directly (add/remove key).

        Mark dirty and start the debounce so the change is persisted
        via the normal write-back path.
        """
        state = self._files.get(path)
        if state is None or not state.roundtrip_ok:
            return
        state.dirty = True
        self._refresh_status(state)
        state.save_timer.start()

    def _on_value_changed(self, path: Path, leaf_path: tuple, new_value) -> None:
        state = self._files.get(path)
        if state is None or not state.roundtrip_ok:
            return
        try:
            set_leaf(state.doc, leaf_path, new_value)
        except Exception as exc:  # noqa: BLE001
            section = self._sections.get(path)
            if section is not None:
                section.set_status(f"could not apply edit: {exc}")
            return
        state.dirty = True
        self._refresh_status(state)
        self._refresh_validation(state)
        state.save_timer.start()

    def _refresh_validation(self, state: _FileState) -> None:
        """Run per-file schema checks and push the result onto the section.

        Today the only check is readout_config.toml's device/chip
        overlap — handled by :func:`readout_validate.find_overlaps`.
        Empty result hides the banner.  Other files: no check, banner
        stays hidden.  Adding a new schema validation is a two-step
        plug-in: write the ``find_*`` helper next to
        ``readout_validate.py``, then dispatch by ``state.path.name``
        here.
        """
        section = self._sections.get(state.path)
        if section is None:
            return
        warnings: list[str] = []
        if state.path.name == "readout_config.toml":
            try:
                warnings = readout_validate.find_overlaps(state.doc)
            except Exception:  # noqa: BLE001
                #  Validation must never block editing — a bug in the
                #  checker shouldn't lock the operator out of the form.
                warnings = []
        section.set_warnings(warnings)

    def _save_to_disk(self, path: Path) -> None:
        state = self._files.get(path)
        if state is None or not state.dirty:
            return
        # Re-attach the post-`##` documentation/examples (if any) so
        # the operator's footer survives a round-trip.  We only edit
        # the pre-cutoff portion of the file in the form.
        _pre_on_disk, post_on_disk = split_at_double_hash(state.text_on_disk)
        new_text = tomlkit.dumps(state.doc) + post_on_disk
        if new_text == state.text_on_disk:
            state.dirty = False
            self._refresh_status(state)
            return

        # Promote master symlinks to working/ before writing so
        # defaults / committed sets aren't accidentally overwritten.
        write_target = self._resolve_write_target(path)

        tmp = write_target.with_suffix(write_target.suffix + ".tmp")
        try:
            tmp.write_text(new_text)
            os.replace(tmp, write_target)
        except OSError as exc:
            section = self._sections.get(path)
            if section is not None:
                section.set_status(f"save failed: {exc}")
            return
        state.text_on_disk = new_text
        try:
            state.last_mtime = path.stat().st_mtime
        except OSError:
            state.last_mtime = time.time()
        state.dirty = False
        self._refresh_status(state)
        # Promotion might have changed the active-set classification.
        self._set_picker.refresh()

    def _resolve_write_target(self, master: Path) -> Path:
        """If ``master`` is a managed conf/ symlink, promote to working/."""
        try:
            inside_conf = master.resolve().is_relative_to(self._conf_dir)
        except (OSError, ValueError):
            inside_conf = False
        if not inside_conf or master.parent != self._conf_dir or not master.is_symlink():
            return master
        return promote_to_working(master)

    # ----- external-change detection -------------------------------------

    def _on_file_changed_on_disk(self, qpath: str) -> None:
        # QFileSystemWatcher drops the path on editor-renames; re-add.
        if qpath not in self._watcher.files():
            self._watcher.addPath(qpath)
        self._handle_external_change(Path(qpath))

    def _on_dir_changed(self, _qpath: str) -> None:
        # Files appeared / disappeared — rebuild the page.
        self._rebuild_sections()

    def _poll_all(self) -> None:
        """Lightweight mtime probe of every known file.

        The single-page layout means every file is "currently shown",
        so we poll the lot.  Cost is N stat calls per second; with
        ~10 files that's nothing.
        """
        for state in list(self._files.values()):
            try:
                mtime = state.path.stat().st_mtime
            except OSError:
                continue
            if mtime > state.last_mtime + 1e-6:
                self._handle_external_change(state.path)

    def _handle_external_change(self, path: Path) -> None:
        state = self._files.get(path)
        section = self._sections.get(path)
        if state is None or section is None:
            # New arrival or stranger — rebuild gets it.
            return
        try:
            mtime = path.stat().st_mtime
            new_text = path.read_text()
        except OSError:
            return
        if mtime <= state.last_mtime + 1e-6 and new_text == state.text_on_disk:
            return
        if state.dirty:
            section.show_conflict()
        else:
            self._load_into_section(path, section)

    # ----- per-section banner handlers -----------------------------------

    def _on_section_reload(self, path: Path) -> None:
        section = self._sections.get(path)
        if section is not None:
            self._load_into_section(path, section)

    def _on_section_keep(self, path: Path) -> None:
        section = self._sections.get(path)
        state = self._files.get(path)
        if section is None or state is None:
            return
        section.hide_conflict()
        # Re-anchor against the on-disk file so the next save overwrites
        # and the watcher doesn't immediately flag again.
        try:
            state.last_mtime = path.stat().st_mtime
            state.text_on_disk = path.read_text()
        except OSError:
            pass
        state.save_timer.start()

    # ----- setting-set handlers ------------------------------------------

    def _on_set_chosen(self, set_name: str) -> None:
        try:
            switch_set(self._conf_dir, set_name)
        except ValueError as exc:
            # Surface on the picker; nothing else to do.
            self._set_picker.show_error(str(exc))
            return
        self._rebuild_sections()
        self._set_picker.refresh()

    def _on_save_as_set_requested(self) -> None:
        # Stubbed for now; the next iteration packages the working
        # overlay into a new ``conf/sets/<name>/``.
        QtWidgets.QMessageBox.information(
            self,
            "Save as setting set",
            "Coming in the next iteration: this will package the current "
            "working/ overlay into a new conf/sets/<name>/ directory "
            "(only the files that actually differ from default) and "
            "repoint masters at the new set.",
        )


# ---------------------------------------------------------------------------
# Reload-conflict / parse-error banner — one per file section.
# ---------------------------------------------------------------------------


class _Banner(QtWidgets.QFrame):
    reload_clicked = QtCore.Signal()
    keep_clicked = QtCore.Signal()

    def __init__(self, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("warnBanner")
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)
        self._label = QtWidgets.QLabel()
        self._label.setWordWrap(True)
        layout.addWidget(self._label, 1)
        self._reload_btn = QtWidgets.QPushButton("Reload from disk")
        self._reload_btn.clicked.connect(self.reload_clicked.emit)
        layout.addWidget(self._reload_btn)
        self._keep_btn = QtWidgets.QPushButton("Keep my edits")
        self._keep_btn.clicked.connect(self.keep_clicked.emit)
        layout.addWidget(self._keep_btn)

    def show_conflict(self, filename: str) -> None:
        self.setObjectName("warnBanner")
        self.style().unpolish(self)
        self.style().polish(self)
        self._label.setText(
            f"⚠ <b>{filename}</b> changed on disk while you have unsaved edits."
        )
        self._reload_btn.setVisible(True)
        self._keep_btn.setVisible(True)

    def show_error(self, filename: str, message: str) -> None:
        self.setObjectName("errorBanner")
        self.style().unpolish(self)
        self.style().polish(self)
        self._label.setText(f"⚠ <b>{filename}</b>: {message}")
        self._reload_btn.setVisible(True)
        self._keep_btn.setVisible(False)


# ---------------------------------------------------------------------------
# Setting-set picker — top-of-page banner.
# ---------------------------------------------------------------------------


class _SettingSetPicker(QtWidgets.QFrame):
    """Active-set indicator + switch dropdown + "save as set" button.

    Reads ``conf_dir`` directly: it owns no state, the filesystem is
    the source of truth.  Emits:

      ``set_chosen(str)``           — operator picked a different set
                                      from the dropdown.
      ``save_as_set_requested()``   — operator clicked the "save as set"
                                      button.
    """

    set_chosen = QtCore.Signal(str)
    save_as_set_requested = QtCore.Signal()

    def __init__(self, conf_dir: Path, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._conf_dir = conf_dir
        self._suppress_signal = False

        self.setObjectName("infoBanner")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(8)

        layout.addWidget(QtWidgets.QLabel("<b>Active set:</b>"))

        self._set_combo = QtWidgets.QComboBox()
        self._set_combo.setMinimumWidth(140)
        self._set_combo.currentIndexChanged.connect(self._on_combo_changed)
        layout.addWidget(self._set_combo)

        self._status_dot = QtWidgets.QLabel()
        self._status_dot.setObjectName("muted")
        layout.addWidget(self._status_dot, 1)

        self._save_as_btn = QtWidgets.QPushButton("Save as set…")
        self._save_as_btn.clicked.connect(self.save_as_set_requested.emit)
        layout.addWidget(self._save_as_btn)

        self.refresh()

    def refresh(self) -> None:
        entries = scan(self._conf_dir)
        active = active_set_name(entries) if entries else "empty"
        sets = list_sets(self._conf_dir)

        choices = ["default", *sets]
        extras = []
        if active not in choices:
            extras.append(active)

        self._suppress_signal = True
        self._set_combo.clear()
        for c in choices:
            self._set_combo.addItem(c, userData=c)
        for e in extras:
            self._set_combo.addItem(f"({e})", userData=e)
            idx = self._set_combo.count() - 1
            self._set_combo.model().item(idx).setEnabled(False)
        for i in range(self._set_combo.count()):
            if self._set_combo.itemData(i) == active:
                self._set_combo.setCurrentIndex(i)
                break
        self._suppress_signal = False

        # Status detail.  Colours come from the active theme palette
        # rather than hard-coded hex so the label reads in both modes.
        pal = theme.palette()
        missing = [e.name for e in entries if e.kind == MasterKind.MISSING]
        if missing:
            self._status_dot.setText(f"⚠ dangling: {', '.join(missing)}")
            self._status_dot.setStyleSheet(f"color: {pal.danger};")
        elif active == "working":
            workers = [e.name for e in entries if e.kind == MasterKind.WORKING]
            self._status_dot.setText(f"working overlay on: {', '.join(workers)}")
            self._status_dot.setStyleSheet(f"color: {pal.warning};")
        elif active == "mixed":
            self._status_dot.setText("masters span multiple sets")
            self._status_dot.setStyleSheet(f"color: {pal.warning};")
        else:
            self._status_dot.setText("")
            self._status_dot.setStyleSheet("")

    def show_error(self, message: str) -> None:
        self._status_dot.setText(message)
        self._status_dot.setStyleSheet(f"color: {theme.palette().danger};")

    def _on_combo_changed(self, _idx: int) -> None:
        if self._suppress_signal:
            return
        name = self._set_combo.currentData()
        if name:
            self.set_chosen.emit(str(name))


__all__ = ["SettingsView"]
