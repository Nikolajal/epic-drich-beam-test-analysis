"""Qt widget that renders a TOML document as an editable form.

Consumed by :class:`qa_quicklook.settings.SettingsView`.  Builds the
form recursively from :func:`qa_quicklook.toml_model.walk_leaves`:

  - one collapsible group box per top-level table,
  - nested tables → nested group boxes,
  - arrays of tables → one collapsible group per entry, labelled
    ``[index]  <first scalar value>`` when one is present (e.g. the
    trigger's ``name``) so the operator can find an entry by its
    label rather than by index,
  - scalar leaves → labelled widgets (QCheckBox / QSpinBox /
    QDoubleSpinBox / QLineEdit),
  - scalar arrays → CSV ``QLineEdit`` that parses on edit,
  - complex leaves → read-only label with a short repr.

The form does *not* hold any document state — it only emits
``value_changed(path, new_value)`` signals.  The Settings view owns
the tomlkit document and the write-back debouncer.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Iterable, Optional

import tomlkit
from PySide6 import QtCore, QtGui, QtWidgets

from .toml_model import Leaf, walk_leaves


class TomlForm(QtWidgets.QWidget):
    """Editable form built from a tomlkit document.

    Plain ``QWidget`` — whoever embeds the form decides on scrolling.
    The Settings tab uses one outer scroll area for many forms stacked
    together; if you only need to display a single form in isolation,
    wrap it in your own ``QScrollArea``.

    Signals:
      ``value_changed(tuple, object)``    — one leaf was edited; payload
                                            is ``(path, new_value)`` ready
                                            for ``toml_model.set_leaf``.
      ``parse_error(str)``                — the CSV array editor failed
                                            to parse a value; the widget
                                            reverts and the form surfaces
                                            the message.
    """

    value_changed = QtCore.Signal(tuple, object)
    parse_error = QtCore.Signal(str)
    # Structural mutation (key added or removed) made directly on the
    # tomlkit doc by a sub-widget — Settings re-saves the whole doc.
    doc_mutated = QtCore.Signal()

    def __init__(self, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._outer = QtWidgets.QVBoxLayout(self)
        self._outer.setContentsMargins(0, 0, 0, 0)
        self._outer.setSpacing(0)
        self._content: QtWidgets.QWidget | None = None
        self._editing: bool = False  # suppress signal emissions during programmatic loads
        # Per-load enum registry: ``{path_tuple: choices}``.  Used to
        # render fixed-choice string leaves (e.g. ``[ui].theme``) as a
        # combo box instead of a free-text line edit.
        self._enums: dict[tuple, tuple[str, ...]] = {}
        # Per-load table-layout registry: ``{path_tuple: TableLayout}``.
        # When a path matches, the form renders that Table as a real
        # multi-column QTableWidget instead of bubble cells — used for
        # key→array shapes like ``device_chip_to_pdu_matrix``.
        self._table_layouts: dict[tuple, "TableLayout"] = {}
        # Per-load section title overrides: ``{path_tuple: title}``.
        # Used to give proper-noun capitalisation to titles the
        # prettifier can't infer (e.g. "Streaming Hough", "Cherenkov").
        self._section_titles: dict[tuple, str] = {}
        # Per-load parameter descriptions: dashboard-curated text
        # that *wins* over the inline TOML comment.  The TOML comment
        # is developer documentation (terse, technical); the registry
        # is operator-facing wording.  Inline comment is the fallback
        # when the registry doesn't have an entry.
        self._param_descriptions: dict[tuple, str] = {}
        # Per-load unit overrides — for keys whose physical unit isn't
        # captured by the name (e.g. ``frame_size`` is in clock
        # cycles "cc").  Renders right of the input; falls back to
        # the suffix detector and then to "a.u." for numeric leaves
        # so the suffix slot is never empty.
        self._param_units: dict[tuple, str] = {}

    # ----- public API -----------------------------------------------------

    def load(
        self,
        doc: tomlkit.TOMLDocument,
        *,
        editable: bool = True,
        enums: dict[tuple, tuple[str, ...]] | None = None,
        table_layouts: dict[tuple, "TableLayout"] | None = None,
        section_titles: dict[tuple, str] | None = None,
        param_descriptions: dict[tuple, str] | None = None,
        param_units: dict[tuple, str] | None = None,
    ) -> None:
        """Render ``doc`` from scratch.

        Discards the previous form widget; signals from old widgets
        stop firing because they're detached and garbage-collected.
        Set ``editable=False`` to render every control disabled (used
        when a file fails the round-trip safety check).  ``enums``
        registers a fixed-choices dropdown for specific leaf paths;
        ``table_layouts`` registers multi-column table rendering for
        specific Table paths.
        """
        self._enums = dict(enums or {})
        self._table_layouts = dict(table_layouts or {})
        self._section_titles = dict(section_titles or {})
        self._param_descriptions = dict(param_descriptions or {})
        self._param_units = dict(param_units or {})
        self._editing = True
        try:
            # Tear down the previous content if present.
            if self._content is not None:
                self._outer.removeWidget(self._content)
                self._content.deleteLater()
                self._content = None

            content = QtWidgets.QWidget()
            layout = QtWidgets.QVBoxLayout(content)
            layout.setContentsMargins(8, 8, 8, 8)
            layout.setSpacing(8)
            layout.setAlignment(QtCore.Qt.AlignTop)
            self._render_node(doc, layout, ())
            if not editable:
                content.setEnabled(False)
            self._outer.addWidget(content)
            self._content = content
        finally:
            self._editing = False

    # ----- rendering ------------------------------------------------------

    def _render_node(self, node: Any, layout: QtWidgets.QBoxLayout, prefix: tuple) -> None:
        """Render one container (TOMLDocument / Table / AoT) into ``layout``."""
        from tomlkit.items import AoT, Table
        from tomlkit.toml_document import TOMLDocument

        if isinstance(node, (TOMLDocument, Table)):
            scalars: list[tuple[str, Any]] = []
            # Iterate over the underlying ``body`` list (not
            # ``.items()``) so we keep the tomlkit-wrapped item
            # objects — booleans in particular get unwrapped to plain
            # ``bool`` by ``.items()``, losing their trivia and so
            # losing the inline help comment we want to render.
            body = node.body if isinstance(node, TOMLDocument) else node.value.body
            for key, value in body:
                if key is None:
                    # Standalone comment / whitespace at this level —
                    # we collect leading comments via _leading_comment
                    # at render time, so skip here.
                    continue
                # Bare identifier (not the quoted source form).
                key_str = getattr(key, "key", str(key)).strip()
                sub_path = (*prefix, key_str)
                # Section title: override registry first (e.g. proper-
                # noun cases like "Streaming Hough"), then the auto-
                # prettifier as a fallback.
                title = self._section_titles.get(sub_path) \
                    or _format_group_title(key_str, value)
                if isinstance(value, (Table, AoT)):
                    table_spec = self._table_layouts.get(sub_path)
                    if isinstance(value, Table) and table_spec is not None:
                        if table_spec.render_mode == "master_detail":
                            section = _MasterDetailSection(
                                path_prefix=sub_path,
                                node=value,
                                layout_spec=table_spec,
                                title=title,
                            )
                            section.value_changed.connect(
                                lambda p, v: self._on_value_changed(p, v)
                            )
                            section.doc_mutated.connect(self.doc_mutated.emit)
                        else:
                            section = _TableSection(
                                path_prefix=sub_path,
                                node=value,
                                layout_spec=table_spec,
                                title=title,
                            )
                            section.value_changed.connect(
                                lambda p, v: self._on_value_changed(p, v)
                            )
                        layout.addWidget(section)
                    else:
                        # Sub-container — render as a collapsible group box.
                        group = _CollapsibleGroup(title)
                        self._render_node(value, group.body_layout(), sub_path)
                        layout.addWidget(group)
                else:
                    scalars.append((key_str, value))
            if scalars:
                # Bubble grid — 2 or 3 columns depending on how many
                # scalars live in this table.  Each cell is a small
                # widget showing label + control + inline comment, so
                # the operator can scan the whole section at a glance
                # instead of scrolling down a long single column.
                grid_holder = self._build_scalar_grid(scalars, prefix)
                # Insert *before* sub-container widgets if any were
                # already added, so leaves come first in the table —
                # less surprising than burying ``frame_size`` under
                # five nested groups.
                layout.insertWidget(0, grid_holder)
            return

        if isinstance(node, AoT):
            for i, entry in enumerate(node):
                label_value = _first_scalar_label(entry)
                title = f"[{i}]" + (f"  {label_value}" if label_value else "")
                group = _CollapsibleGroup(title)
                self._render_node(entry, group.body_layout(), (*prefix, i))
                layout.addWidget(group)
            return

        # Leaf at the top level of a render call — single-cell grid.
        grid_holder = self._build_scalar_grid(
            [(prefix[-1] if prefix else "?", node)], prefix[:-1] if prefix else ()
        )
        layout.addWidget(grid_holder)

    def _build_scalar_grid(
        self,
        scalars: list[tuple[str, Any]],
        prefix: tuple,
    ) -> QtWidgets.QWidget:
        """Lay scalar children out in a bubble grid.

        Column count is content-aware: dense for bool-only sections,
        roomy for arrays / long values, 2-3 cols for typical commented
        sections.  See :func:`_grid_columns`.

        Consecutive scalars sharing their first underscore-separated
        token (``r_min`` / ``r_max`` / ``r_step``, etc.) are
        auto-grouped into a sub-card so logically-related params live
        in the same visual unit.
        """
        holder = QtWidgets.QWidget()
        grid = QtWidgets.QGridLayout(holder)
        grid.setContentsMargins(4, 6, 4, 6)
        grid.setHorizontalSpacing(14)
        grid.setVerticalSpacing(12)

        units = _detect_groups(scalars)
        # For column-count purposes we represent each unit (group or
        # single) by a sample scalar — the heuristic in
        # ``_grid_columns`` only needs the kinds and comment-presence
        # across the units, not the full membership.
        sample_for_grid: list[tuple[str, Any]] = []
        for unit_entry in units:
            if unit_entry[0] == "group":
                _kind, _label, items = unit_entry
                sample_for_grid.append((items[0][0], items[0][1]))
            else:
                _kind, pair = unit_entry
                sample_for_grid.append(pair)
        ncols = _grid_columns(sample_for_grid)
        for i in range(ncols):
            grid.setColumnStretch(i, 1)

        #  Track the next free (row, col) honouring full-row spans —
        #  a table_array (e.g. ``devices = [{…}]`` in readout_config)
        #  carries its own grid (key/value pairs with right-edge
        #  resize), so squeezing it into a half-width column left a
        #  giant dead area to its left and a pinched table to its
        #  right.  Bumping it to a full row lets the editor fill the
        #  card horizontally, and the scalar before/after it falls
        #  onto its own row too — which reads as "one knob per row"
        #  rather than two cramped halves.
        row = 0
        col = 0
        for unit_entry in units:
            if unit_entry[0] == "group":
                _kind, prefix_tokens, group_items = unit_entry
                cell = self._build_param_group(prefix, prefix_tokens, group_items)
                full_row = False
            else:
                _kind, (key, value) = unit_entry
                path = (*prefix, key)
                cell = self._build_param_cell(path, key, value)
                full_row = _kind_of(value) == "table_array"

            if full_row:
                #  Push to a fresh row, span everything, then advance.
                if col != 0:
                    row += 1
                    col = 0
                grid.addWidget(cell, row, 0, 1, ncols)
                row += 1
                col = 0
            else:
                grid.addWidget(cell, row, col)
                col += 1
                if col >= ncols:
                    col = 0
                    row += 1
        return holder

    def _build_param_group(
        self,
        prefix: tuple,
        prefix_tokens: tuple,
        items: list[tuple[str, Any]],
    ) -> QtWidgets.QWidget:
        """Sub-card grouping related params, header = shared prefix.

        Member labels strip the shared prefix — inside a group titled
        "Afterpulse near", the members read just "LO" / "HI" instead
        of "Afterpulse near LO" / "Afterpulse near HI".
        """
        prefix_len = len(prefix_tokens)
        title_key = "_".join(prefix_tokens)
        members: list[tuple[str, str, Any, tuple]] = []
        for key, value in items:
            path = (*prefix, key)
            # Suffix = the tokens past the shared prefix.
            suffix_tokens = key.split("_")[prefix_len:]
            suffix_label = "_".join(suffix_tokens) if suffix_tokens else key
            members.append((key, suffix_label, value, path))
        return _ParamGroupCard(
            _prettify_key(title_key),
            members,
            on_change=lambda path, val: self._on_value_changed(path, val),
            enums_lookup=self._enums.get,
            units_lookup=lambda p, k: self._param_units.get(p) or _unit_for_key(k),
        )

    def _build_param_cell(self, path: tuple, key: str, value: Any) -> QtWidgets.QWidget:
        """Build one labelled-control-with-comment cell."""
        leaf = Leaf(path=path, value=_unwrap(value), kind=_kind_of(value))
        widget = _build_widget(
            leaf,
            on_change=lambda v: self._on_value_changed(path, v),
            enum_choices=self._enums.get(path),
        )
        # Description: curated registry wins; inline TOML comment is
        # the fallback when the registry doesn't have one.  Unit-only
        # comments (e.g. ``# mm``) are treated as empty since the
        # unit is rendered as a suffix label instead.
        comment = self._param_descriptions.get(path, "").strip()
        if not comment:
            inline = _inline_comment(value).strip()
            if inline.lower() not in _UNIT_ONLY_COMMENTS:
                comment = inline
        # Unit: registry override → key suffix → "a.u." for numeric
        # leaves (so the suffix slot stays consistent across the page).
        unit = self._param_units.get(path) or _unit_for_key(key)
        if not unit and leaf.kind in ("int", "float", "int_array", "float_array"):
            unit = "a.u."
        return _ParamCell(
            key=key,
            display_label=_prettify_key(key),
            unit=unit,
            leaf=leaf,
            control=widget,
            comment=comment,
        )

    def _on_value_changed(self, path: tuple, new_value: Any) -> None:
        if self._editing:
            return
        self.value_changed.emit(path, new_value)


# ---------------------------------------------------------------------------
# Fixed-height (two-line) description label used by every param cell.
# ---------------------------------------------------------------------------


class _TwoLineComment(QtWidgets.QLabel):
    """Description shown beneath a parameter's control.

    Two lines tall **when there is a comment** — short comments get
    padded with a blank second line so cells in the same row stay
    aligned; longer comments wrap to two lines and elide with ``…``
    (full text on tooltip).

    **When the TOML key has no inline comment** (which is the common
    case for tables like ``pdu_rotation = { 1 = true, 2 = true, … }``)
    the label is hidden entirely.  The cell then shrinks to just its
    label + control row — no wasted vertical space.
    """

    _LINES = 2

    def __init__(self, text: str, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("muted")
        self.setStyleSheet("font-size: 10px;")
        self.setWordWrap(True)
        self.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self._full_text = (text or "").strip()
        if not self._full_text:
            # No comment → don't take any space.  setVisible(False)
            # removes the widget from the parent layout's size hints.
            self.setVisible(False)
            self.setFixedHeight(0)
            return
        # Reserve two lines worth of height up-front so the surrounding
        # grid lays out without jitter.
        fm = QtGui.QFontMetrics(self.font())
        self.setFixedHeight(fm.lineSpacing() * self._LINES + 2)
        self.setToolTip(self._full_text)
        self.setText(self._full_text)

    def resizeEvent(self, ev: QtGui.QResizeEvent) -> None:  # noqa: N802 — Qt callback
        # Re-elide on every width change so the label fits exactly two
        # lines.  Skipped when there's no comment (label is hidden).
        super().resizeEvent(ev)
        if not self._full_text:
            return
        fm = QtGui.QFontMetrics(self.font())
        width = max(0, self.width() - 2)
        rendered = _wrap_to_lines(self._full_text, fm, width, self._LINES)
        QtWidgets.QLabel.setText(self, rendered)


def _wrap_to_lines(text: str, fm: QtGui.QFontMetrics, width: int, max_lines: int) -> str:
    """Wrap ``text`` to ``max_lines``; elide the final line on overflow.

    Greedy word-wrap — same algorithm Qt would use for ``setWordWrap``
    but we control the line count so the cell height stays fixed.
    """
    if width <= 0 or not text:
        return text
    words = text.split()
    lines: list[str] = []
    current = ""
    for w in words:
        candidate = (current + " " + w).strip()
        if fm.horizontalAdvance(candidate) <= width:
            current = candidate
            continue
        if current:
            lines.append(current)
        current = w
        if len(lines) >= max_lines:
            break
    if current and len(lines) < max_lines:
        lines.append(current)

    # If there's still more text past max_lines, elide the last line.
    consumed = " ".join(lines)
    if consumed != text:
        if lines:
            lines[-1] = fm.elidedText(
                lines[-1] + " " + text[len(consumed):].strip(),
                QtCore.Qt.ElideRight,
                width,
            )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Parameter cell — one scalar leaf rendered as a compact card.
# ---------------------------------------------------------------------------


class _ParamCell(QtWidgets.QFrame):
    """Label + control + inline-comment hint, one TOML scalar leaf.

    Lays out as::

        ┌───────────────────────────────────┐
        │ frame_size             [ 1024 ]   │
        │ clock cycles per frame (320 MHz…) │
        └───────────────────────────────────┘

    The comment text comes from the leaf's tomlkit inline ``# …``
    comment (whatever the operator already wrote next to the value in
    the file), so the help text is automatically the file's own
    documentation — no duplicated spec.
    """

    def __init__(
        self,
        key: str,
        display_label: str,
        leaf: Leaf,
        control: QtWidgets.QWidget,
        comment: str,
        unit: str = "",
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        # Subtle surface tone so each cell reads as its own chip even
        # without a heavy border.
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.NoFrame)

        outer = QtWidgets.QVBoxLayout(self)
        # Vertical padding stays modest; we tighten further below when
        # the comment is absent.  Side padding kept generous for
        # readability.
        outer.setContentsMargins(12, 8, 12, 8)
        # Tight inter-row spacing when the comment widget is hidden
        # (it would otherwise leave a phantom 6 px gap below the
        # label/control row).
        has_comment = bool((comment or "").strip())
        outer.setSpacing(6 if has_comment else 0)

        top = QtWidgets.QHBoxLayout()
        top.setContentsMargins(0, 0, 0, 0)
        top.setSpacing(10)

        label = QtWidgets.QLabel(display_label)
        # Human-readable label on the left.  The canonical TOML key,
        # its path, and the inferred kind live in the tooltip for
        # power users; the cell itself stays scannable.
        label.setStyleSheet("font-weight: 600;")
        label.setToolTip(
            f"<b>{key}</b><br/>"
            f"path: {'.'.join(_stringify_path(leaf.path))}<br/>"
            f"kind: {leaf.kind}"
        )
        top.addWidget(label, 0)
        top.addStretch(1)
        # Pin the control to a fixed-width slot so every cell's input
        # lands at the same x within its grid column.  Without this,
        # short values shrink the control and the right edge drifts.
        control.setSizePolicy(
            QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Fixed
        )
        control.setMinimumWidth(110)
        control.setMaximumWidth(110)
        top.addWidget(control, 0, QtCore.Qt.AlignVCenter)

        # Unit slot — *always* present, even when blank.  Reserving
        # the width here keeps cells with units (mm, ns, cc, a.u., …)
        # aligned with cells whose unit is genuinely missing (string
        # / bool leaves); otherwise the input shifts a few pixels
        # left in some rows and produces a zig-zag.
        unit_label = QtWidgets.QLabel(unit or "")
        unit_label.setObjectName("muted")
        unit_label.setStyleSheet("font-size: 11px; padding-left: 2px;")
        unit_label.setFixedWidth(36)
        unit_label.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
        top.addWidget(unit_label, 0, QtCore.Qt.AlignVCenter)

        outer.addLayout(top)

        # Two-line description below the row.  Hidden entirely when
        # the TOML key has no inline ``# …`` comment so cells in
        # comment-free sections (e.g. ``pdu_rotation``, the device→
        # PDU matrix) stay tight.
        comment_label = _TwoLineComment(comment)
        outer.addWidget(comment_label)


# ---------------------------------------------------------------------------
# Group card — wraps multiple related _ParamCells (e.g. R min/max/step)
# in a single bordered sub-card, so logically-coupled params live as
# a unit instead of being spread across separate cells.
# ---------------------------------------------------------------------------


class _ParamGroupCard(QtWidgets.QFrame):
    """Sub-card with a horizontal row of compact mini-cells.

    Header = the shared prefix (auto-prettified).  Each member shows
    only its suffix label (the part of the key past the shared
    prefix), its control, and its unit — no per-member description
    (would defeat the compaction).  Tooltips on the labels carry the
    full key and path for power users.
    """

    def __init__(
        self,
        title: str,
        members: list[tuple[str, str, Any, tuple]],
        on_change,
        enums_lookup,
        units_lookup,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(10, 8, 10, 10)
        outer.setSpacing(6)

        head = QtWidgets.QLabel(title)
        head_font = head.font()
        head_font.setBold(True)
        head_font.setPointSize(head_font.pointSize() + 1)
        head.setFont(head_font)
        head.setObjectName("sectionTitle")
        outer.addWidget(head)

        row = QtWidgets.QHBoxLayout()
        row.setSpacing(14)
        for key, suffix_label, value, path in members:
            leaf = Leaf(path=path, value=_unwrap(value), kind=_kind_of(value))
            widget = _build_widget(
                leaf,
                on_change=lambda v, p=path: on_change(p, v),
                enum_choices=enums_lookup(path),
            )
            unit = units_lookup(path, key)
            if not unit and leaf.kind in ("int", "float", "int_array", "float_array"):
                unit = "a.u."
            row.addLayout(_compact_member(suffix_label, key, leaf, widget, unit))
        row.addStretch(1)
        outer.addLayout(row)


def _compact_member(
    suffix_label: str,
    raw_key: str,
    leaf: Leaf,
    control: QtWidgets.QWidget,
    unit: str,
) -> QtWidgets.QBoxLayout:
    """Tiny (label · control · unit) triplet for a grouped member."""
    h = QtWidgets.QHBoxLayout()
    h.setContentsMargins(0, 0, 0, 0)
    h.setSpacing(6)
    label = QtWidgets.QLabel(_prettify_key(suffix_label))
    label.setStyleSheet("font-weight: 600;")
    label.setToolTip(
        f"<b>{raw_key}</b><br/>"
        f"path: {'.'.join(_stringify_path(leaf.path))}<br/>"
        f"kind: {leaf.kind}"
    )
    h.addWidget(label)
    control.setSizePolicy(QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Fixed)
    control.setFixedWidth(90)
    h.addWidget(control)
    if unit:
        unit_label = QtWidgets.QLabel(unit)
        unit_label.setObjectName("muted")
        unit_label.setStyleSheet("font-size: 11px;")
        unit_label.setFixedWidth(32)
        h.addWidget(unit_label)
    return h


# ---------------------------------------------------------------------------
# TableSection — sections whose semantic shape is a key → array
# relation, rendered as a proper multi-column table instead of a
# bunch of bubble cells.  The shape comes from a per-file registry
# (``TABLE_LAYOUTS`` in :mod:`qa_quicklook.settings`) so the form
# stays generic; specialised layouts are configuration, not code.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class TableLayout:
    """Describe how to render a key→array Table as a relation.

    ``key_columns`` is the labels for the columns derived from the
    Table's *keys*; ``key_splitter`` (e.g. ``"_"``) tells us how to
    split a compound key like ``"192_0"`` into ``("192", "0")``.
    When ``key_splitter`` is ``None`` the key is treated as a single
    column.

    ``value_columns`` labels the columns derived from the array
    *value* of each entry (one column per array element).

    ``render_mode`` chooses between:

      ``"auto"``    chip grid for ≤ ``CHIP_THRESHOLD`` entries, table
                    otherwise — the default.  Dense for small
                    sections (PDU XY position) and tabular for long
                    ones (device→chip→PDU matrix).
      ``"chips"``   force chip grid.
      ``"table"``   force QTableWidget mode.
    """

    key_columns: tuple[str, ...]
    value_columns: tuple[str, ...]
    key_splitter: str | None = None
    render_mode: str = "auto"


CHIP_THRESHOLD = 16


def _choose_table_mode(spec: TableLayout, n_entries: int) -> str:
    if spec.render_mode in ("chips", "table"):
        return spec.render_mode
    return "chips" if n_entries <= CHIP_THRESHOLD else "table"


class _TableSection(QtWidgets.QFrame):
    """Render a Table whose body is ``key = [v0, v1, ...]`` as a relation.

    Two modes:
      * **chips**  — one compact card per entry with the value
                     columns shown inline (label-value pairs).  Used
                     for short sections (≤ 16 entries by default);
                     fast to scan, dense across the page.
      * **table**  — proper QTableWidget with column headers per
                     value column.  Used for long sections where
                     line-by-line semantic columns matter.

    Edits in either mode flow through ``value_changed(path, value)``
    with an array-indexed path so the parent SettingsView can persist
    them via ``toml_model.set_leaf``.
    """

    value_changed = QtCore.Signal(tuple, object)

    def __init__(
        self,
        path_prefix: tuple,
        node: Any,
        layout_spec: TableLayout,
        title: str,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(10, 8, 10, 12)
        outer.setSpacing(8)

        # Title row mirroring _CollapsibleGroup so the visual rhythm
        # of the page is consistent.
        title_label = QtWidgets.QLabel(title)
        title_font = title_label.font()
        title_font.setPointSize(title_font.pointSize() + 2)
        title_font.setBold(True)
        title_label.setFont(title_font)
        outer.addWidget(title_label)

        # Build the data rows.
        rows = self._extract_rows(node, layout_spec)
        self._path_prefix = path_prefix
        self._layout_spec = layout_spec

        mode = _choose_table_mode(layout_spec, len(rows))
        if mode == "chips":
            outer.addWidget(self._build_chip_grid(rows, layout_spec))
        else:
            outer.addWidget(self._build_table(rows, layout_spec))

    # ----- chip grid mode -------------------------------------------------

    def _build_chip_grid(
        self,
        rows: list[tuple[str, list[str], list[Any]]],
        spec: TableLayout,
    ) -> QtWidgets.QWidget:
        """One compact cell per entry, inline label/value pairs.

        Columns adapt to the number of entries — same heuristic as
        the scalar bubble grid — so 8 entries lay out as 4 wide, 12
        as 4 wide, etc.
        """
        holder = QtWidgets.QWidget()
        grid = QtWidgets.QGridLayout(holder)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(8)
        grid.setVerticalSpacing(6)

        n = len(rows)
        # Slightly denser than the scalar bubble grid since chip cells
        # are intentionally narrow.
        ncols = 1 if n <= 1 else min(n, 4 if n <= 12 else 6)
        for c in range(ncols):
            grid.setColumnStretch(c, 1)

        for idx, (orig_key, key_parts, values) in enumerate(rows):
            row, col = divmod(idx, ncols)
            chip = self._build_chip(orig_key, key_parts, values, spec)
            grid.addWidget(chip, row, col)
        return holder

    def _build_chip(
        self,
        orig_key: str,
        key_parts: list[str],
        values: list[Any],
        spec: TableLayout,
    ) -> QtWidgets.QWidget:
        """One ``key → values`` chip card."""
        chip = QtWidgets.QFrame()
        chip.setObjectName("cardSurface")
        chip.setFrameShape(QtWidgets.QFrame.NoFrame)
        row = QtWidgets.QHBoxLayout(chip)
        row.setContentsMargins(8, 6, 8, 6)
        row.setSpacing(8)

        # Key label on the left.  For compound keys, join with `·` so
        # "192_0" reads as "192·0" inside the chip without taking two
        # extra cells.
        key_text = " · ".join(str(p) for p in key_parts) if len(key_parts) > 1 else str(key_parts[0])
        key_label = QtWidgets.QLabel(key_text)
        key_label.setStyleSheet("font-weight: 600;")
        key_label.setToolTip(orig_key)
        row.addWidget(key_label, 0)

        # Inline value editors.  Each value gets a tiny dim label
        # (the value-column name) + the typed input.
        for j, (col_name, v) in enumerate(zip(spec.value_columns, values)):
            tiny = QtWidgets.QLabel(col_name)
            tiny.setObjectName("muted")
            tiny.setStyleSheet("font-size: 10px;")
            row.addWidget(tiny, 0)
            editor = _build_value_editor(
                v,
                on_change=lambda new_v, key=orig_key, idx=j: self.value_changed.emit(
                    (*self._path_prefix, key, idx), new_v
                ),
            )
            row.addWidget(editor, 1)

        return chip

    # ----- table mode -----------------------------------------------------

    def _build_table(
        self,
        rows: list[tuple[str, list[str], list[Any]]],
        spec: TableLayout,
    ) -> QtWidgets.QWidget:
        all_columns = list(spec.key_columns) + list(spec.value_columns)
        n_key_cols = len(spec.key_columns)

        table = QtWidgets.QTableWidget(len(rows), len(all_columns), self)
        table.setHorizontalHeaderLabels(all_columns)
        table.verticalHeader().setVisible(False)
        hh = table.horizontalHeader()
        hh.setStretchLastSection(False)
        hh.setSectionResizeMode(QtWidgets.QHeaderView.ResizeToContents)
        table.setEditTriggers(
            QtWidgets.QAbstractItemView.DoubleClicked
            | QtWidgets.QAbstractItemView.EditKeyPressed
            | QtWidgets.QAbstractItemView.AnyKeyPressed
        )
        table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectItems)
        table.setAlternatingRowColors(True)
        row_h = table.fontMetrics().lineSpacing() + 6
        table.verticalHeader().setDefaultSectionSize(row_h)
        table.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        table.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        table.setSizePolicy(
            QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed
        )
        header_h = hh.sizeHint().height()
        table.setFixedHeight(header_h + row_h * max(1, len(rows)) + 4)

        self._cell_paths: dict[tuple[int, int], tuple[str, int]] = {}
        self._populating = True
        try:
            for row_idx, (orig_key, key_parts, values) in enumerate(rows):
                for col, part in enumerate(key_parts):
                    item = QtWidgets.QTableWidgetItem(str(part))
                    item.setFlags(item.flags() & ~QtCore.Qt.ItemIsEditable)
                    table.setItem(row_idx, col, item)
                for j, v in enumerate(values):
                    item = QtWidgets.QTableWidgetItem(_format_cell_value(v))
                    item.setData(QtCore.Qt.UserRole, v)
                    table.setItem(row_idx, n_key_cols + j, item)
                    self._cell_paths[(row_idx, n_key_cols + j)] = (orig_key, j)
        finally:
            self._populating = False

        self._table = table
        table.itemChanged.connect(self._on_item_changed)
        return table

    # ----- internals ----------------------------------------------------

    def _extract_rows(self, node: Any, spec: TableLayout) -> list[tuple[str, list[str], list[Any]]]:
        """Return ``[(original_key, key_parts, value_list), …]``."""
        from tomlkit.items import Array, Table
        body = node.value.body if isinstance(node, Table) else getattr(node, "body", [])
        out: list[tuple[str, list[str], list[Any]]] = []
        for key, value in body:
            if key is None:
                continue
            # tomlkit's ``str(key)`` returns the *source representation*
            # (quotes + trailing whitespace), so quoted keys like
            # ``"192_0"`` render as ``"192`` / ``0"`` after splitting.
            # ``key.key`` is the underlying bare identifier.
            orig_key = getattr(key, "key", str(key)).strip()
            if spec.key_splitter:
                parts = orig_key.split(spec.key_splitter)
                # Pad / truncate to match the declared key column count.
                parts = (parts + [""] * len(spec.key_columns))[: len(spec.key_columns)]
            else:
                parts = [orig_key]
            # Coerce values to a plain Python list.
            if isinstance(value, Array):
                vals = [v for v in value]
            elif isinstance(value, list):
                vals = list(value)
            else:
                vals = [value]
            # Pad to declared value-column width.
            vals = (vals + [""] * len(spec.value_columns))[: len(spec.value_columns)]
            out.append((orig_key, parts, vals))
        return out

    def _on_item_changed(self, item: QtWidgets.QTableWidgetItem) -> None:
        if self._populating:
            return
        coord = (item.row(), item.column())
        if coord not in self._cell_paths:
            return
        orig_key, value_index = self._cell_paths[coord]
        text = item.text()
        # Parse using the original value's type as a hint so an
        # integer stays an integer when the user types ``42``.
        original = item.data(QtCore.Qt.UserRole)
        try:
            new_val = _coerce_cell_value(text, original)
        except ValueError:
            # Revert and bail; the user sees their typo restored.
            self._populating = True
            try:
                item.setText(_format_cell_value(original))
            finally:
                self._populating = False
            return
        path = (*self._path_prefix, orig_key, value_index)
        self.value_changed.emit(path, new_val)


def _build_value_editor(value: Any, on_change) -> QtWidgets.QWidget:
    """Tiny widget for an inline (chip-cell) value.

    Picks the right kind based on the value's runtime type, applies
    a compact maximum width so chips stay narrow, and routes edits
    through ``on_change``.
    """
    if isinstance(value, bool):
        widget = _bool_widget(value, on_change)
    elif isinstance(value, int):
        widget = _int_widget(value, on_change)
        widget.setMaximumWidth(80)
        widget.setAlignment(QtCore.Qt.AlignRight)
    elif isinstance(value, float):
        widget = _float_widget(value, on_change)
        widget.setMaximumWidth(80)
        widget.setAlignment(QtCore.Qt.AlignRight)
    else:
        widget = _str_widget(str(value), on_change)
        widget.setMaximumWidth(120)
    return widget


# ---------------------------------------------------------------------------
# Master-detail editor for grouped key→array tables.
#
# Used for sections whose semantic shape is ``(group_key, sub_key) →
# value_array`` — e.g. the device→chip→[PDU, Quadrant] matrix.  The
# section's natural width is two panes:
#
#   - left  → unique group keys (devices), with ``+/-`` to add/remove.
#   - right → the selected group's detail rows (chips), with
#             ``+/-`` to add/remove and per-row editable values.
#
# Structural changes (key add/remove) mutate the tomlkit doc directly
# and signal ``doc_mutated`` upward; SettingsView re-saves the file
# through the normal debounced write-back path.
# ---------------------------------------------------------------------------


class _MasterDetailSection(QtWidgets.QFrame):
    """Two-pane editor for ``(prefix, rest)`` key tables.

    Left:  list of unique master keys (e.g. devices) + ``+/-``.
    Right: detail table for the selected master (e.g. that device's
           chips) with ``+/-`` to add/remove rows.  Editable values.
    """

    value_changed = QtCore.Signal(tuple, object)
    doc_mutated = QtCore.Signal()

    def __init__(
        self,
        path_prefix: tuple,
        node: Any,
        layout_spec: TableLayout,
        title: str,
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("cardSurface")
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        self._node = node
        self._path_prefix = path_prefix
        self._spec = layout_spec
        self._populating = False

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(10, 8, 10, 12)
        outer.setSpacing(8)

        title_label = QtWidgets.QLabel(title)
        title_font = title_label.font()
        title_font.setPointSize(title_font.pointSize() + 2)
        title_font.setBold(True)
        title_label.setFont(title_font)
        outer.addWidget(title_label)

        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        splitter.setChildrenCollapsible(False)
        splitter.addWidget(self._build_master_pane())
        splitter.addWidget(self._build_detail_pane())
        splitter.setSizes([220, 680])
        outer.addWidget(splitter)

        self._refresh_master()

    # ----- pane construction ---------------------------------------------

    # ----- pane construction ---------------------------------------------

    def _build_master_pane(self) -> QtWidgets.QWidget:
        master_label = self._spec.key_columns[0] if self._spec.key_columns else "Group"
        wrap = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(wrap)
        layout.setContentsMargins(0, 0, 6, 0)
        layout.setSpacing(4)

        cap = QtWidgets.QLabel(master_label + "s")
        cap.setObjectName("muted")
        cap.setStyleSheet("font-size: 11px;")
        layout.addWidget(cap)

        self._master_list = QtWidgets.QListWidget()
        self._master_list.setSelectionMode(QtWidgets.QAbstractItemView.SingleSelection)
        self._master_list.currentItemChanged.connect(self._on_master_changed)
        layout.addWidget(self._master_list, 1)

        btn_row = QtWidgets.QHBoxLayout()
        add_btn = QtWidgets.QPushButton("+")
        add_btn.setToolTip(f"Add {master_label.lower()}")
        add_btn.clicked.connect(self._on_add_master)
        rm_btn = QtWidgets.QPushButton("−")
        rm_btn.setToolTip(f"Remove selected {master_label.lower()} and all its rows")
        rm_btn.clicked.connect(self._on_remove_master)
        btn_row.addWidget(add_btn)
        btn_row.addWidget(rm_btn)
        btn_row.addStretch(1)
        layout.addLayout(btn_row)
        return wrap

    def _build_detail_pane(self) -> QtWidgets.QWidget:
        wrap = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(wrap)
        layout.setContentsMargins(6, 0, 0, 0)
        layout.setSpacing(4)

        self._detail_caption = QtWidgets.QLabel("Pick an entry on the left.")
        self._detail_caption.setObjectName("muted")
        layout.addWidget(self._detail_caption)

        sub_key_cols = list(self._spec.key_columns[1:])
        value_cols = list(self._spec.value_columns)
        cols = sub_key_cols + value_cols
        self._n_sub_key_cols = len(sub_key_cols)

        self._detail_table = QtWidgets.QTableWidget(0, len(cols))
        self._detail_table.setHorizontalHeaderLabels(cols)
        self._detail_table.verticalHeader().setVisible(False)
        hh = self._detail_table.horizontalHeader()
        hh.setStretchLastSection(False)
        hh.setSectionResizeMode(QtWidgets.QHeaderView.ResizeToContents)
        self._detail_table.setEditTriggers(
            QtWidgets.QAbstractItemView.DoubleClicked
            | QtWidgets.QAbstractItemView.EditKeyPressed
            | QtWidgets.QAbstractItemView.AnyKeyPressed
        )
        self._detail_table.setAlternatingRowColors(True)
        row_h = self._detail_table.fontMetrics().lineSpacing() + 6
        self._detail_table.verticalHeader().setDefaultSectionSize(row_h)
        self._detail_table.itemChanged.connect(self._on_detail_item_changed)
        layout.addWidget(self._detail_table, 1)

        btn_row = QtWidgets.QHBoxLayout()
        self._add_chip_btn = QtWidgets.QPushButton("+")
        self._add_chip_btn.setToolTip("Add a row to the selected entry")
        self._add_chip_btn.clicked.connect(self._on_add_detail)
        self._add_chip_btn.setEnabled(False)
        self._rm_chip_btn = QtWidgets.QPushButton("−")
        self._rm_chip_btn.setToolTip("Remove the selected row")
        self._rm_chip_btn.clicked.connect(self._on_remove_detail)
        self._rm_chip_btn.setEnabled(False)
        btn_row.addWidget(self._add_chip_btn)
        btn_row.addWidget(self._rm_chip_btn)
        btn_row.addStretch(1)
        layout.addLayout(btn_row)
        return wrap

    # ----- node ↔ UI -----------------------------------------------------

    def _refresh_master(self) -> None:
        prev = self._current_master()
        self._master_list.blockSignals(True)
        self._master_list.clear()
        seen: set[str] = set()
        for _orig, master, _sub, _vals in self._entries():
            if master in seen:
                continue
            seen.add(master)
            count = sum(1 for e in self._entries() if e[1] == master)
            item = QtWidgets.QListWidgetItem(f"{master}   ({count})")
            item.setData(QtCore.Qt.UserRole, master)
            self._master_list.addItem(item)
        self._master_list.blockSignals(False)
        if prev:
            for i in range(self._master_list.count()):
                if self._master_list.item(i).data(QtCore.Qt.UserRole) == prev:
                    self._master_list.setCurrentRow(i)
                    self._refresh_detail()
                    return
        self._refresh_detail()

    def _refresh_detail(self) -> None:
        master = self._current_master()
        enable = master is not None
        self._add_chip_btn.setEnabled(enable)
        self._rm_chip_btn.setEnabled(enable)

        rows = [e for e in self._entries() if e[1] == master] if master else []
        master_lbl = self._spec.key_columns[0] if self._spec.key_columns else "Group"
        sub_lbl = self._spec.key_columns[1] if len(self._spec.key_columns) > 1 else "Row"
        self._detail_caption.setText(
            f"{master_lbl} {master}  —  {len(rows)} {sub_lbl.lower()}{'s' if len(rows) != 1 else ''}"
            if master else
            "Pick an entry on the left."
        )

        self._populating = True
        try:
            self._detail_table.setRowCount(len(rows))
            self._row_keys: list[str] = []
            for r, (orig_key, _master, sub, vals) in enumerate(rows):
                self._row_keys.append(orig_key)
                if self._n_sub_key_cols > 0:
                    item = QtWidgets.QTableWidgetItem(sub)
                    item.setFlags(item.flags() & ~QtCore.Qt.ItemIsEditable)
                    self._detail_table.setItem(r, 0, item)
                for j, v in enumerate(vals):
                    item = QtWidgets.QTableWidgetItem(_format_cell_value(v))
                    item.setData(QtCore.Qt.UserRole, v)
                    self._detail_table.setItem(r, self._n_sub_key_cols + j, item)
        finally:
            self._populating = False

        row_h = self._detail_table.verticalHeader().defaultSectionSize()
        header_h = self._detail_table.horizontalHeader().sizeHint().height()
        self._detail_table.setFixedHeight(header_h + row_h * max(1, len(rows)) + 4)

    def _current_master(self) -> Optional[str]:
        item = self._master_list.currentItem()
        if item is None:
            return None
        return str(item.data(QtCore.Qt.UserRole))

    # ----- signals -------------------------------------------------------

    def _on_master_changed(self, _current, _previous) -> None:
        self._refresh_detail()

    def _on_detail_item_changed(self, item: QtWidgets.QTableWidgetItem) -> None:
        if self._populating:
            return
        row = item.row()
        col = item.column()
        if col < self._n_sub_key_cols or row >= len(self._row_keys):
            return
        value_index = col - self._n_sub_key_cols
        orig_key = self._row_keys[row]
        original = item.data(QtCore.Qt.UserRole)
        try:
            new_val = _coerce_cell_value(item.text(), original)
        except ValueError:
            self._populating = True
            try:
                item.setText(_format_cell_value(original))
            finally:
                self._populating = False
            return
        self.value_changed.emit(
            (*self._path_prefix, orig_key, value_index), new_val,
        )

    def _entries(self) -> list[tuple[str, str, str, list[Any]]]:
        """Return ``[(orig_key, master_part, sub_part, value_list), …]``."""
        from tomlkit.items import Array, Table
        body = self._node.value.body if isinstance(self._node, Table) else getattr(self._node, "body", [])
        out: list[tuple[str, str, str, list[Any]]] = []
        splitter = self._spec.key_splitter or "_"
        for key, value in body:
            if key is None:
                continue
            orig_key = getattr(key, "key", str(key)).strip()
            parts = orig_key.split(splitter)
            master = parts[0] if parts else orig_key
            sub = splitter.join(parts[1:]) if len(parts) > 1 else ""
            if isinstance(value, Array):
                vals = [v for v in value]
            elif isinstance(value, list):
                vals = list(value)
            else:
                vals = [value]
            vals = (vals + [0] * len(self._spec.value_columns))[: len(self._spec.value_columns)]
            out.append((orig_key, master, sub, vals))
        return out

    # (legacy per-card item handler removed when we reverted to splitter mode)

    def _on_add_master(self) -> None:
        master_lbl = self._spec.key_columns[0] if self._spec.key_columns else "Group"
        text, ok = QtWidgets.QInputDialog.getText(
            self, f"Add {master_lbl}", f"{master_lbl} identifier:"
        )
        if not ok or not text.strip():
            return
        new_master = text.strip()
        for _o, m, _s, _v in self._entries():
            if m == new_master:
                QtWidgets.QMessageBox.warning(
                    self, "Duplicate", f"{master_lbl} {new_master} already exists.",
                )
                return
        splitter = self._spec.key_splitter or "_"
        new_key = f"{new_master}{splitter}0" if self._spec.key_columns and len(self._spec.key_columns) > 1 else new_master
        self._node[new_key] = [0] * len(self._spec.value_columns)
        self.doc_mutated.emit()
        self._refresh_master()
        # Auto-select the new entry on the left so the operator sees it.
        for i in range(self._master_list.count()):
            if self._master_list.item(i).data(QtCore.Qt.UserRole) == new_master:
                self._master_list.setCurrentRow(i)
                break

    def _on_remove_master(self) -> None:
        master = self._current_master()
        if not master:
            return
        master_lbl = self._spec.key_columns[0] if self._spec.key_columns else "Group"
        confirm = QtWidgets.QMessageBox.question(
            self, f"Remove {master_lbl}",
            f"Remove {master_lbl} {master} and all its rows?",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
        )
        if confirm != QtWidgets.QMessageBox.Yes:
            return
        for orig_key, m, _s, _v in self._entries():
            if m == master:
                del self._node[orig_key]
        self.doc_mutated.emit()
        self._refresh_master()

    def _on_add_detail(self) -> None:
        master = self._current_master()
        if not master:
            return
        splitter = self._spec.key_splitter or "_"
        used: set[int] = set()
        for _o, m, sub, _v in self._entries():
            if m != master:
                continue
            try:
                used.add(int(sub))
            except ValueError:
                pass
        next_idx = 0
        while next_idx in used:
            next_idx += 1
        new_key = f"{master}{splitter}{next_idx}"
        self._node[new_key] = [0] * len(self._spec.value_columns)
        self.doc_mutated.emit()
        self._refresh_detail()

    def _on_remove_detail(self) -> None:
        row = self._detail_table.currentRow()
        if row < 0 or row >= len(self._row_keys):
            return
        orig_key = self._row_keys[row]
        del self._node[orig_key]
        self.doc_mutated.emit()
        self._refresh_master()


def _format_cell_value(v: Any) -> str:
    """How a typed value renders in a QTableWidgetItem."""
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def _coerce_cell_value(text: str, original: Any) -> Any:
    """Parse ``text`` back to the same primitive type as ``original``."""
    if isinstance(original, bool):
        s = text.strip().lower()
        if s in {"true", "1", "yes", "on"}:
            return True
        if s in {"false", "0", "no", "off"}:
            return False
        raise ValueError(f"not a bool: {text!r}")
    if isinstance(original, int):
        return int(text.strip())
    if isinstance(original, float):
        return float(text.strip())
    return text


# ---------------------------------------------------------------------------
# Group widget — header that toggles a body widget visible/hidden.  We
# prefer this over QGroupBox(setCheckable=True) because the latter
# disables children rather than hiding them, which still consumes
# layout space.
# ---------------------------------------------------------------------------


class _CollapsibleGroup(QtWidgets.QFrame):
    def __init__(self, title: str, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        # Border + background come from the global ``#cardSurface`` QSS.
        self.setObjectName("cardSurface")
        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(10, 8, 10, 12)
        outer.setSpacing(8)

        self._toggle = QtWidgets.QToolButton()
        self._toggle.setText("▾  " + title)
        self._toggle.setCheckable(True)
        self._toggle.setChecked(True)
        # Bump the title font 2 pt above the default so section names
        # read as a heading instead of getting lost in their content.
        font = self._toggle.font()
        font.setPointSize(font.pointSize() + 2)
        font.setBold(True)
        self._toggle.setFont(font)
        self._toggle.setStyleSheet(
            "QToolButton { text-align: left; padding: 4px 0; }"
        )
        self._toggle.toggled.connect(self._on_toggled)
        outer.addWidget(self._toggle)

        self._body = QtWidgets.QWidget()
        self._body_layout = QtWidgets.QVBoxLayout(self._body)
        self._body_layout.setContentsMargins(8, 4, 0, 0)
        self._body_layout.setSpacing(8)
        outer.addWidget(self._body)

    def body_layout(self) -> QtWidgets.QBoxLayout:
        return self._body_layout

    def _on_toggled(self, checked: bool) -> None:
        self._body.setVisible(checked)
        self._toggle.setText(("▾  " if checked else "▸  ") + self._toggle.text().lstrip("▾▸ "))


# ---------------------------------------------------------------------------
# Widget building.  One function per kind keeps the type-specific
# conversions self-contained; ``on_change`` is a single-arg callback
# that receives the parsed new value.
# ---------------------------------------------------------------------------


def _build_widget(leaf: Leaf, on_change, enum_choices: tuple[str, ...] | None = None) -> QtWidgets.QWidget:
    kind = leaf.kind
    value = leaf.value
    # Enum override: if the leaf has a fixed set of valid values
    # (registered per-path by the caller), render a dropdown.  Works
    # for any underlying kind, though in practice it's almost always
    # strings (theme = "light"/"dark"/"system", etc.).
    if enum_choices:
        return _enum_widget(value, enum_choices, on_change)
    if kind == "bool":
        return _bool_widget(bool(value), on_change)
    if kind == "int":
        return _int_widget(int(value), on_change)
    if kind == "float":
        return _float_widget(float(value), on_change)
    if kind == "str":
        return _str_widget(str(value), on_change)
    if kind in ("int_array", "float_array", "str_array"):
        elem_kind = kind.split("_", 1)[0]
        return _array_widget(value, elem_kind, on_change)
    if kind == "table_array":
        # Inline-table arrays (e.g. ``devices = [{ id = 192, chips =
        # "*" }, …]``) — render as a real editable mini-table.
        return _inline_table_array_widget(value, leaf.path, on_change)
    # complex — render the repr as a tooltip + a disabled hint label.
    return _complex_widget(value)


def _inline_table_array_widget(value: list, base_path: tuple, on_change) -> QtWidgets.QWidget:
    """Editable mini-table for an Array of InlineTables.

    Columns = union of every entry's keys (preserves first-occurrence
    order).  Each cell is editable; on change we emit a *whole-list*
    replacement through ``on_change`` because the underlying tomlkit
    structure is one ``Array`` value at ``base_path``, not per-cell
    leaves.  This is simpler than threading per-cell paths and lets
    ``toml_model.set_leaf`` keep its scalar contract.
    """
    # Build column list preserving first-occurrence order.
    columns: list[str] = []
    seen: set[str] = set()
    for row in value:
        for k in row:
            if k not in seen:
                seen.add(k)
                columns.append(k)

    holder = QtWidgets.QFrame()
    holder.setObjectName("cardSurface")
    holder.setFrameShape(QtWidgets.QFrame.StyledPanel)
    outer = QtWidgets.QVBoxLayout(holder)
    outer.setContentsMargins(8, 6, 8, 8)
    outer.setSpacing(6)

    table = QtWidgets.QTableWidget(len(value), len(columns))
    table.setHorizontalHeaderLabels(columns)
    table.verticalHeader().setVisible(False)
    hh = table.horizontalHeader()
    #  Stretch so the table fills the (now full-row) parent cell.
    #  The earlier ResizeToContents+full-row combination left the
    #  table pinched against the left edge with all the horizontal
    #  space dead — last section stretches so trailing nulls don't
    #  leave a final dead column either.
    hh.setStretchLastSection(True)
    hh.setSectionResizeMode(QtWidgets.QHeaderView.Stretch)
    table.setEditTriggers(
        QtWidgets.QAbstractItemView.DoubleClicked
        | QtWidgets.QAbstractItemView.EditKeyPressed
        | QtWidgets.QAbstractItemView.AnyKeyPressed
    )
    table.setAlternatingRowColors(True)
    row_h = table.fontMetrics().lineSpacing() + 6
    table.verticalHeader().setDefaultSectionSize(row_h)
    table.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
    table.setVerticalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
    table.setFixedHeight(hh.sizeHint().height() + row_h * max(1, len(value)) + 4)

    # Holds the mutable list-of-dicts; we rebuild on every edit.
    state = [dict(r) for r in value]
    populating = {"on": True}

    def repopulate() -> None:
        populating["on"] = True
        try:
            table.setRowCount(len(state))
            for r, row_dict in enumerate(state):
                for c, col_name in enumerate(columns):
                    raw = row_dict.get(col_name, "")
                    item = QtWidgets.QTableWidgetItem(_format_cell_value(raw))
                    item.setData(QtCore.Qt.UserRole, raw)
                    table.setItem(r, c, item)
        finally:
            populating["on"] = False

    def on_item_changed(item: QtWidgets.QTableWidgetItem) -> None:
        if populating["on"]:
            return
        col_name = columns[item.column()]
        original = item.data(QtCore.Qt.UserRole)
        try:
            new_val = _coerce_cell_value(item.text(), original)
        except ValueError:
            populating["on"] = True
            try:
                item.setText(_format_cell_value(original))
            finally:
                populating["on"] = False
            return
        state[item.row()][col_name] = new_val
        on_change(state.copy())

    table.itemChanged.connect(on_item_changed)
    repopulate()
    outer.addWidget(table)

    # +/- row.  Add duplicates the last row (preserves columns); a
    # missing-row guard renders Add useful even on an empty array.
    btn_row = QtWidgets.QHBoxLayout()
    add_btn = QtWidgets.QToolButton()
    add_btn.setText("+ row")

    def add_row() -> None:
        if state:
            template = dict(state[-1])
        else:
            template = {c: "" for c in columns}
        state.append(template)
        on_change(state.copy())
        table.setRowCount(len(state))
        # Lazy: re-render the just-added row.
        for c, col_name in enumerate(columns):
            item = QtWidgets.QTableWidgetItem(_format_cell_value(template.get(col_name, "")))
            item.setData(QtCore.Qt.UserRole, template.get(col_name, ""))
            table.setItem(len(state) - 1, c, item)
        # Resize height to fit the new row.
        rh = table.verticalHeader().defaultSectionSize()
        hh_h = table.horizontalHeader().sizeHint().height()
        table.setFixedHeight(hh_h + rh * len(state) + 4)

    add_btn.clicked.connect(add_row)
    rm_btn = QtWidgets.QToolButton()
    rm_btn.setText("− row")

    def remove_row() -> None:
        row = table.currentRow()
        if row < 0 or row >= len(state):
            return
        del state[row]
        on_change(state.copy())
        repopulate()
        rh = table.verticalHeader().defaultSectionSize()
        hh_h = table.horizontalHeader().sizeHint().height()
        table.setFixedHeight(hh_h + rh * max(1, len(state)) + 4)

    rm_btn.clicked.connect(remove_row)
    btn_row.addWidget(add_btn)
    btn_row.addWidget(rm_btn)
    btn_row.addStretch(1)
    outer.addLayout(btn_row)

    return holder


def _bool_widget(value: bool, on_change) -> QtWidgets.QCheckBox:
    cb = QtWidgets.QCheckBox()
    cb.setChecked(value)
    cb.toggled.connect(lambda checked: on_change(bool(checked)))
    return cb


def _int_widget(value: int, on_change) -> QtWidgets.QLineEdit:
    """Plain line edit + integer validator.

    Scientific configs span a huge value range (flag toggles, frame
    counts in the millions, FIFO indices, …) so a sized spin box
    never has the right increment anyway.  A clean line edit reads
    better, has no visual chrome, and round-trips cleanly to TOML.
    """
    line = QtWidgets.QLineEdit(str(int(value)))
    line.setValidator(QtGui.QIntValidator(-(2**31), 2**31 - 1, line))
    last_good = {"v": int(value)}

    def commit() -> None:
        text = line.text().strip()
        try:
            new = int(text)
        except ValueError:
            line.setText(str(last_good["v"]))
            return
        last_good["v"] = new
        on_change(new)

    line.editingFinished.connect(commit)
    return line


def _float_widget(value: float, on_change) -> QtWidgets.QLineEdit:
    """Plain line edit + double validator with `%g` formatting.

    No spin box (the up/down arrows aren't useful for physics values
    that span orders of magnitude), no forced decimal padding (the
    old ``setDecimals(6)`` turned ``35.0`` into ``35,000000`` in
    European locales), and `,` is accepted on input as a decimal
    separator so a comma-keypad user isn't stuck.
    """
    line = QtWidgets.QLineEdit(_format_float_for_display(value))
    validator = QtGui.QDoubleValidator(-1e15, 1e15, 12, line)
    validator.setNotation(QtGui.QDoubleValidator.StandardNotation)
    line.setValidator(validator)
    last_good = {"v": float(value)}

    def commit() -> None:
        text = line.text().replace(",", ".").strip()
        try:
            new = float(text)
        except ValueError:
            line.setText(_format_float_for_display(last_good["v"]))
            return
        last_good["v"] = new
        line.setText(_format_float_for_display(new))
        on_change(new)

    line.editingFinished.connect(commit)
    return line


def _format_float_for_display(v: float) -> str:
    """Format a float without unnecessary trailing zeros.

    ``35.0``      → ``"35.0"``  (keep one decimal so the eye still reads it as a float)
    ``1.5``       → ``"1.5"``
    ``0.0035``    → ``"0.0035"``
    ``0.0``       → ``"0.0"``
    ``1.5e-7``    → ``"1.5e-07"``
    """
    if v == 0.0:
        return "0.0"
    if v == int(v) and abs(v) < 1e15:
        return f"{int(v)}.0"
    return f"{v:g}"


def _str_widget(value: str, on_change) -> QtWidgets.QLineEdit:
    line = QtWidgets.QLineEdit(value)
    line.editingFinished.connect(lambda: on_change(line.text()))
    return line


def _enum_widget(value: Any, choices: tuple[str, ...], on_change) -> QtWidgets.QComboBox:
    """Dropdown for a leaf with a fixed set of valid values.

    If ``value`` isn't among ``choices`` we add it as an extra disabled
    item so the user sees the current (unexpected) value rather than
    silently snapping to the first choice.
    """
    combo = QtWidgets.QComboBox()
    for c in choices:
        combo.addItem(str(c))
    current = str(value)
    if current not in choices:
        combo.addItem(f"({current})", userData=current)
        combo.model().item(combo.count() - 1).setEnabled(False)
    combo.setCurrentText(current)
    combo.currentTextChanged.connect(
        lambda t: on_change(t if not t.startswith("(") else t.strip("()"))
    )
    return combo


def _array_widget(values: list, elem_kind: str, on_change) -> QtWidgets.QWidget:
    """One-line CSV editor for scalar arrays.

    Renders ``[1, 2, 3]`` as ``"1, 2, 3"``.  On commit we parse each
    comma-separated token according to ``elem_kind``; a parse failure
    reverts the widget to the last good value and emits a tooltip
    (no exception bubble out).
    """
    holder = QtWidgets.QWidget()
    layout = QtWidgets.QHBoxLayout(holder)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(4)

    line = QtWidgets.QLineEdit(_format_csv(values))
    layout.addWidget(line, 1)

    hint = QtWidgets.QLabel(f"[{elem_kind} list, comma-separated]")
    hint.setObjectName("muted")
    hint.setStyleSheet("font-size: 10px;")
    layout.addWidget(hint)

    def commit() -> None:
        try:
            parsed = _parse_csv(line.text(), elem_kind)
        except ValueError as exc:
            # Surface the parse error with a tinted background pulled
            # from the active theme's danger colour so it reads in dark
            # mode too.
            from . import theme as _theme
            pal = _theme.palette()
            line.setText(_format_csv(values))
            line.setToolTip(f"parse failed: {exc}")
            line.setStyleSheet(
                f"QLineEdit {{ background: {_theme._mix(pal.danger, pal.bg, 0.75)}; }}"
            )
            return
        line.setStyleSheet("")
        line.setToolTip("")
        on_change(parsed)

    line.editingFinished.connect(commit)
    return holder


def _complex_widget(value: Any) -> QtWidgets.QWidget:
    holder = QtWidgets.QFrame()
    # Reuse the muted-card surface from the theme; no hex here.
    holder.setObjectName("cardSurface")
    h = QtWidgets.QHBoxLayout(holder)
    h.setContentsMargins(4, 2, 4, 2)
    label = QtWidgets.QLabel("(complex — edit in TOML directly)")
    label.setObjectName("muted")
    label.setStyleSheet("font-style: italic;")
    label.setToolTip(repr(value)[:400])
    h.addWidget(label)
    h.addStretch(1)
    return holder


# ---------------------------------------------------------------------------
# Misc.
# ---------------------------------------------------------------------------


def _row_label(key: str, leaf: Leaf) -> QtWidgets.QLabel:
    # Kept for backward compatibility with any caller still using the
    # single-row form.  Colour comes from the global QLabel rule; we
    # just attach the path / kind tooltip.
    label = QtWidgets.QLabel(key)
    label.setToolTip(f"path: {'.'.join(_stringify_path(leaf.path))}\nkind: {leaf.kind}")
    return label


# ---------------------------------------------------------------------------
# Human-readable rendering of snake_case TOML keys.
# ---------------------------------------------------------------------------


# Trailing tokens that turn into a parenthesised unit at the end of
# the label.  Keys are lowercased for matching; values are the canonical
# display form (so ``_mev`` → "(MeV)").
_UNIT_SUFFIXES: dict[str, str] = {
    "mm": "mm", "cm": "cm", "m": "m",
    "ns": "ns", "us": "µs", "ms": "ms", "s": "s",
    "cc": "cc",
    "ev": "eV", "kev": "keV", "mev": "MeV", "gev": "GeV", "tev": "TeV",
    "deg": "deg", "rad": "rad",
    "hz": "Hz", "khz": "kHz", "mhz": "MHz",
    "v": "V", "mv": "mV", "kv": "kV",
}

# Tokens that should render as all-caps (domain acronyms).
_ALL_CAPS: set[str] = {
    "qa", "tdc", "dcr", "ct", "rdo", "pdu", "sipm", "mcp", "fpga",
    "alcor", "epic", "drich", "daq", "fifo", "lo", "hi", "id", "xy",
}

# Token aliases — replaced verbatim regardless of position.  Lets
# scientific symbols come through (no LaTeX, so we substitute the
# canonical Unicode glyph where it makes sense).
_TOKEN_ALIASES: dict[str, str] = {
    "sigma": "σ",
    "mu": "μ",
    "phi": "φ",
    "theta": "θ",
    "delta": "Δ",
    "lambda": "λ",
    "alpha": "α",
    "beta": "β",
    "gamma": "γ",
    "tau": "τ",
    "chi": "χ",
    "omega": "ω",
    "pi": "π",
    "eta": "η",
    "rho": "ρ",
    "epsilon": "ε",
    "psi": "ψ",
    "nu": "ν",
}


def _prettify_key(key: str) -> str:
    """Turn ``snake_case`` into a human-readable label.

    Rules:

      - ``frame_size`` → ``"Frame size"``
      - ``n_phi_bins_coverage`` → ``"N phi bins coverage"``
      - ``r_min_coverage_mm`` → ``"R min coverage"``   (unit dropped — shown to the right of the input)
      - ``ct_phys_signal_lo`` → ``"CT phys signal LO"``  (CT, LO in ``_ALL_CAPS``)
      - single-letter tokens stay capitalised (``"R"`` not ``"r"``)

    Returns ``key`` unchanged for already-mixed-case keys (we don't
    want to butcher e.g. ``deviceID``).
    """
    if any(c.isupper() for c in key):
        return key
    # Compound numeric identifiers (``"195_0"`` device/chip pair) — the
    # underscores are punctuation, not word separators.  Leave the key
    # untouched so the cell shows e.g. ``195_0`` not ``"195 0"``.
    parts = key.split("_")
    if any(p.isdigit() for p in parts):
        return key
    # Strip a trailing unit suffix — the cell renderer shows it as a
    # small label to the right of the input instead, which reads
    # cleaner than baking ``"(mm)"`` into the title.
    if len(parts) > 1 and parts[-1] in _UNIT_SUFFIXES:
        parts = parts[:-1]
    out: list[str] = []
    for i, p in enumerate(parts):
        if p in _TOKEN_ALIASES:
            out.append(_TOKEN_ALIASES[p])
            continue
        if p in _ALL_CAPS:
            out.append(p.upper())
            continue
        if len(p) == 1 and p.isalpha():
            out.append(p.upper())
            continue
        out.append(p.capitalize() if i == 0 else p)
    return " ".join(out)


def _unit_for_key(key: str) -> str:
    """Return the canonical unit symbol for the key, or ``""``.

    ``r_min_coverage_mm`` → ``"mm"``, ``time_window_ns`` → ``"ns"``,
    ``frame_size`` → ``""``.
    """
    if any(c.isupper() for c in key):
        return ""
    parts = key.split("_")
    if len(parts) > 1 and parts[-1] in _UNIT_SUFFIXES and not any(p.isdigit() for p in parts):
        return _UNIT_SUFFIXES[parts[-1]]
    return ""


# Comments that are *just* a unit token (e.g. a TOML line like
# ``r_min_coverage_mm = 25.0   # mm``) become redundant once the
# unit is shown as the right-side suffix.  Suppress them so the
# description area collapses instead of restating the obvious.
_UNIT_ONLY_COMMENTS: set[str] = {u.lower() for u in _UNIT_SUFFIXES.values()}


def _detect_groups(scalars: list[tuple[str, Any]]) -> list[tuple]:
    """Group consecutive scalars sharing a multi-token prefix.

    Returns a list of entries in original order, each shaped as:

      ``("single", (key, value))``                — one standalone param
      ``("group", prefix_tokens, [(key, value), …])``  — 2+ related params,
                                                         ``prefix_tokens`` is
                                                         a tuple of the common
                                                         underscore-separated
                                                         tokens

    Rule: group only when **at least 2 underscore-tokens** are shared,
    so trivial single-letter prefixes (``r_min`` / ``r_max`` / ``r_step``
    share just ``"r"``) don't create useless "R" headers.  Real groups
    look like:

      - ``afterpulse_near_lo`` / ``afterpulse_near_hi`` → prefix
        ``("afterpulse", "near")``
      - ``fit_circle_init_x/y/r`` → prefix ``("fit", "circle", "init")``
      - ``ct_phys_signal_lo/hi`` → prefix ``("ct", "phys", "signal")``
    """
    out: list[tuple] = []
    i = 0
    n = len(scalars)
    while i < n:
        # Probe the longest *2+ token* common prefix starting at i.
        toks_i = scalars[i][0].split("_")
        if len(toks_i) >= 3:
            # Need at least 3 tokens total so the prefix can be 2+
            # tokens and there's still a suffix to discriminate members.
            j = i + 1
            common = toks_i[:-1]  # try the longest prefix first
            while common:
                # Find the run of consecutive entries that match this
                # candidate prefix and have at least one suffix token.
                k = i
                while k < n:
                    parts = scalars[k][0].split("_")
                    if len(parts) > len(common) and parts[:len(common)] == common:
                        k += 1
                    else:
                        break
                if k - i >= 2 and len(common) >= 2:
                    out.append(("group", tuple(common), scalars[i:k]))
                    i = k
                    common = None
                    break
                # Shrink the candidate prefix and try again.
                common = common[:-1]
            if common is None:
                continue
        # Fallback: standalone cell.
        out.append(("single", scalars[i]))
        i += 1
    return out


def _grid_columns(scalars: list[tuple[str, Any]]) -> int:
    """Pick a column count based on the cells' content, not just the count.

    Rules of thumb, in priority order:

      1. **Has any inline comment** → use the readable 2- or 3-column
         default (so the comment row has room to render).
      2. **All bool, no comments** (``pdu_rotation``, ``readout``
         channel masks…) → pack tightly, up to 8 across so a row of
         eight checkboxes lays out in one line.
      3. **Contains arrays** (``pdu_xy_position``, device-matrix,
         scalar lists) → cap at 4 columns; CSV strings need width.
      4. **Mixed plain scalars, no comments** → 4 columns, less for
         very small sections.
    """
    n = len(scalars)
    if n == 0:
        return 1

    has_comment = any(_inline_comment(v).strip() for _, v in scalars)
    if has_comment:
        # Tiny sections — never wrap to a half-empty second row.
        if n <= 3:
            return n
        return 3 if n >= 9 else 2

    kinds = {_kind_of(v) for _, v in scalars}
    if kinds <= {"bool"}:
        # All checkboxes — fits comfortably in one line up to 8 wide.
        return min(n, 8)
    if kinds & {"int_array", "float_array", "str_array"}:
        return min(n, 4)
    # Plain scalars (int / float / str) with no commentary.
    return min(n, 4)


def _inline_comment(item: Any) -> str:
    """Pull the trailing ``# …`` comment off a tomlkit item.

    Returns ``""`` for items without a comment (plain Python values
    that don't carry trivia, or items the operator didn't annotate).
    Leading ``#`` and whitespace are stripped so the caller renders
    just the text.
    """
    trivia = getattr(item, "trivia", None)
    if trivia is None:
        return ""
    raw = (trivia.comment or "").strip()
    if raw.startswith("#"):
        raw = raw[1:].strip()
    return raw


def _stringify_path(path: tuple) -> Iterable[str]:
    for seg in path:
        yield f"[{seg}]" if isinstance(seg, int) else str(seg)


def _format_group_title(key: Any, container: Any) -> str:
    """Title shown on a collapsible group's header.

    Keys are routed through ``_prettify_key`` so section names read
    like prose ("Device chip to PDU matrix") instead of the raw TOML
    identifier.  For arrays of tables we annotate ``[N]`` so the
    operator sees the count without expanding.
    """
    from tomlkit.items import AoT
    pretty = _prettify_key(str(key))
    if isinstance(container, AoT):
        return f"{pretty}  [{len(container)} entries]"
    return pretty


def _first_scalar_label(table: Any) -> Optional[str]:
    """Return the first scalar value of ``table`` for use as an AoT row label.

    e.g. ``[[trigger]]`` entries get titled with their ``name`` field.
    Returns ``None`` if no scalar key is available.
    """
    from tomlkit.items import Bool, Float, Integer, String
    for _, v in table.items():
        if isinstance(v, (String, str)):
            return str(v)
        if isinstance(v, (Bool, Integer, Float, bool, int, float)):
            return str(v)
    return None


def _format_csv(values: list) -> str:
    return ", ".join(str(v) for v in values)


def _parse_csv(text: str, elem_kind: str) -> list:
    tokens = [t.strip() for t in text.split(",") if t.strip() != ""]
    if elem_kind == "int":
        return [int(t) for t in tokens]
    if elem_kind == "float":
        return [float(t) for t in tokens]
    return tokens  # str


def _unwrap(value: Any) -> Any:
    """Best-effort tomlkit → plain-python unwrap for the leaf payload."""
    from tomlkit.items import Bool, Float, Integer, String, Array, InlineTable, Table
    if isinstance(value, Bool):
        return bool(value)
    if isinstance(value, Integer):
        return int(value)
    if isinstance(value, Float):
        return float(value)
    if isinstance(value, String):
        return str(value)
    if isinstance(value, Array):
        return [_unwrap(x) for x in value]
    if isinstance(value, (InlineTable, Table)):
        return {str(k): _unwrap(v) for k, v in value.items()}
    return value


def _kind_of(value: Any) -> str:
    """Mirror of toml_model._leaf_kind for a single value (so we don't
    import a private function across modules)."""
    from .toml_model import _leaf_kind   # tightly coupled here; one place to update
    return _leaf_kind(value)


__all__ = ["TomlForm"]
