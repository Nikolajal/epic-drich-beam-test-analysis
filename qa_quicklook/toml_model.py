"""Pure helpers over a ``tomlkit`` document.

Everything in here is headless and Qt-free.  The Settings form widget
calls these to enumerate editable leaves, push edits back into the
document, and verify that the document round-trips losslessly through
``tomlkit`` (a precondition for write-back: if parse → dump already
mutates the file, our save would silently rewrite the user's
formatting).

Path conventions
----------------
A "path" is a tuple of segments addressing one node in the document.
Each segment is either:

  - ``str``   — a table key (``"framer"``, ``"qa"``).
  - ``int``   — an index into an array of tables (``[[trigger]][0]``).

So the third trigger's ``delay`` key in ``trigger_conf.toml`` lives at
``("trigger", 2, "delay")``.

Leaf kinds
----------
``walk_leaves`` reports one of:

  ``"bool"``   ``"int"``   ``"float"``   ``"str"``
  ``"int_array"``       homogeneous list of int
  ``"float_array"``     homogeneous list of float
  ``"str_array"``       homogeneous list of str
  ``"table_array"``     list of inline tables (array of dicts)
  ``"complex"``         everything else — dict-shaped tables, lists of
                        tables, mixed arrays, etc.  These are reported
                        as leaves (so the UI can show them) but
                        ``set_leaf`` refuses to write to a complex
                        path.  The form widget renders them read-only.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Iterator, Sequence

import tomlkit
from tomlkit.items import AoT, Array, Bool, Float, InlineTable, Integer, String, Table
from tomlkit.toml_document import TOMLDocument


# Sentinel passed into ``set_leaf`` to *delete* a key rather than
# overwrite it.  Useful when a checkbox is unchecked and the schema
# wants the key absent rather than ``false``.  Not exercised by the
# current Settings form (every field round-trips a value, including
# the "(unset)" combobox sentinel which writes ``""``); kept here
# because ``set_leaf`` honours it and a future widget that wants
# key-absent semantics can opt in by passing ``DELETE`` directly.
DELETE = object()


PathSegment = str | int
Path = tuple[PathSegment, ...]


@dataclass(frozen=True)
class Leaf:
    """One renderable node in the document tree."""

    path: Path
    value: Any
    kind: str              # one of the kind strings documented above


# ---------------------------------------------------------------------------
# Walking
# ---------------------------------------------------------------------------


def walk_leaves(doc: TOMLDocument | Table | AoT) -> Iterator[Leaf]:
    """Yield every leaf in document order.

    Tables and arrays-of-tables are descended into; the *contents* are
    yielded, not the container itself.  Scalar arrays (lists of
    int/float/str) are yielded as a single leaf each — the form
    renders them as a CSV line edit, not as N separate widgets.

    Complex values (dicts of arrays, inline tables that aren't simple,
    mixed-type arrays) are yielded with ``kind="complex"`` so the
    caller can render them read-only instead of guessing.
    """
    yield from _walk(doc, ())


def _walk(node: Any, prefix: Path) -> Iterator[Leaf]:
    if isinstance(node, (TOMLDocument, Table)):
        for key, value in node.items():
            yield from _walk(value, (*prefix, str(key)))
        return
    if isinstance(node, AoT):
        for i, entry in enumerate(node):
            # Each entry is a Table; descend and prefix its path with
            # the integer index so set_leaf can address it.
            yield from _walk(entry, (*prefix, i))
        return
    yield Leaf(path=prefix, value=_python_value(node), kind=_leaf_kind(node))


# ---------------------------------------------------------------------------
# Edits
# ---------------------------------------------------------------------------


def set_leaf(doc: TOMLDocument, path: Path, new_value: Any) -> None:
    """Update the leaf at ``path`` in ``doc`` with ``new_value``.

    Mutates the document in place — caller is responsible for dumping
    it back to disk.  Raises ``KeyError`` if the path is missing,
    ``TypeError`` if the path traverses into something that isn't a
    table or array-of-tables.

    Complex leaves (``kind="complex"``) reject writes — the v1 form
    doesn't expose an editor for them, and going through this helper
    would silently strip the existing tomlkit formatting.  Use
    ``replace_complex`` instead (not implemented yet) when we add a
    raw-TOML side editor.
    """
    if not path:
        raise ValueError("empty path")
    parent = _descend(doc, path[:-1])
    last = path[-1]
    existing = _index(parent, last)
    existing_kind = _leaf_kind(existing)
    if existing_kind == "complex":
        raise TypeError(
            f"refusing to write complex node at {path!r} via set_leaf; "
            "the tomlkit formatting would be lost"
        )
    if existing_kind == "table_array":
        # Wholesale replace the array of inline tables with the new
        # list-of-dicts.  tomlkit re-renders the array compactly
        # (loses the operator's original line breaks, accepted).
        import tomlkit as _tk
        new_array = _tk.array()
        for row in (new_value or []):
            tbl = _tk.inline_table()
            for k, v in row.items():
                tbl[k] = v
            new_array.append(tbl)
        _assign(parent, last, new_array)
        return
    if new_value is DELETE:
        del parent[last]
        return
    _assign(parent, last, new_value)


def replace_document_text(text: str, path: Path, new_value: Any) -> str:
    """Round-trip helper: parse → set_leaf → dump.

    Used by the file-write path so callers don't have to import
    tomlkit themselves.  Returns the new file body; callers write it
    atomically.
    """
    doc = tomlkit.parse(text)
    set_leaf(doc, path, new_value)
    return tomlkit.dumps(doc)


def split_at_double_hash(text: str) -> tuple[str, str]:
    """Return ``(pre, post)`` split at the first ``##``-marker line.

    The project's parser convention (documented in ``trigger_conf.toml``
    and respected by ``RunInfo::read_database()``-style C++ readers)
    is: a line whose first non-whitespace characters are ``##`` halts
    parsing — everything below is treated as documentation, disabled
    examples, or change-log notes and is *not* part of the active
    configuration.

    The dashboard mirrors this in two ways:

      - on **load**, the rendered form sees only ``pre`` (so disabled
        examples don't show up as editable controls);
      - on **save**, the dashboard re-attaches ``post`` verbatim so
        the operator's documentation/examples survive a round trip.

    ``pre`` ends with a newline if the input did; ``post`` starts
    with the marker line.  When no ``##`` is present, ``pre`` is the
    whole input and ``post`` is empty.
    """
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if line.lstrip().startswith("##"):
            return "".join(lines[:i]), "".join(lines[i:])
    return text, ""


def apply_double_hash_cutoff(text: str) -> str:
    """Backwards-compat wrapper around :func:`split_at_double_hash`."""
    return split_at_double_hash(text)[0]


def roundtrip_safe(text: str) -> bool:
    """True if ``tomlkit.dumps(tomlkit.parse(text)) == text``.

    The Settings form runs this once when a file is loaded.  A
    mismatch means tomlkit reformats the file on every save — the
    user's hand-formatting would drift on the first edit.  When this
    returns False the UI surfaces a banner and disables write-back
    for that file; the user can still see values but edits go to
    nowhere.
    """
    try:
        return tomlkit.dumps(tomlkit.parse(text)) == text
    except Exception:  # noqa: BLE001 — parse errors mean "not safe"
        return False


# ---------------------------------------------------------------------------
# Internals
# ---------------------------------------------------------------------------


def _descend(doc: TOMLDocument | Table | AoT, path: Path) -> Any:
    """Walk into ``doc`` following ``path``; return the parent container."""
    cur: Any = doc
    for seg in path:
        cur = _index(cur, seg)
    return cur


def _index(container: Any, key: PathSegment) -> Any:
    if isinstance(key, int):
        # Two flavours of integer-indexable containers:
        #   - ``AoT`` for ``[[trigger]]``-style arrays of tables;
        #   - ``Array`` for plain scalar / inline-table arrays
        #     (``pdu_xy_position = [-82.0, 30.0]`` etc).
        # We accept either so set_leaf can write to a single element
        # of e.g. ``("device_chip_to_pdu_matrix", "192_0", 0)``.
        if isinstance(container, (AoT, Array)):
            return container[key]
        # Some tomlkit versions return plain ``list`` after unwrap.
        if isinstance(container, list):
            return container[key]
        raise TypeError(
            f"int index {key!r} requires an Array / AoT, got {type(container).__name__}"
        )
    # str key — works on Table / TOMLDocument / InlineTable.
    return container[key]


def _assign(container: Any, key: PathSegment, value: Any) -> None:
    if isinstance(key, int):
        container[key] = value
    else:
        container[key] = value


def _python_value(node: Any) -> Any:
    """Strip tomlkit's wrapper types so callers compare against plain
    Python primitives.  We unwrap arrays element-wise so the value the
    caller sees is a plain ``list[int|float|str]`` for the array case.
    """
    if isinstance(node, Bool):
        return bool(node)
    if isinstance(node, Integer):
        return int(node)
    if isinstance(node, Float):
        return float(node)
    if isinstance(node, String):
        return str(node)
    if isinstance(node, Array):
        return [_python_value(x) for x in node]
    if isinstance(node, (InlineTable, Table)):
        return {str(k): _python_value(v) for k, v in node.items()}
    # Fallback — already a python primitive (bool/int/float/str/list/dict).
    if isinstance(node, list):
        return [_python_value(x) for x in node]
    if isinstance(node, dict):
        return {str(k): _python_value(v) for k, v in node.items()}
    return node


def _leaf_kind(node: Any) -> str:
    """Classify a value for the form widget.

    The order of the ``isinstance`` checks matters: tomlkit's
    ``Bool``/``Integer``/``Float``/``String`` subclass the Python
    primitives in some versions, so we test the tomlkit types first
    where it matters, and ``bool`` before ``int`` because ``True`` is
    also an ``int``.
    """
    # Tomlkit wrappers first (they round-trip preserving comments).
    if isinstance(node, Bool):
        return "bool"
    if isinstance(node, Integer):
        return "int"
    if isinstance(node, Float):
        return "float"
    if isinstance(node, String):
        return "str"
    if isinstance(node, Array):
        return _classify_array([_python_value(x) for x in node])
    if isinstance(node, (InlineTable, Table, AoT, TOMLDocument)):
        return "complex"

    # Plain-python fallbacks.
    if isinstance(node, bool):
        return "bool"
    if isinstance(node, int):
        return "int"
    if isinstance(node, float):
        return "float"
    if isinstance(node, str):
        return "str"
    if isinstance(node, list):
        return _classify_array(node)
    return "complex"


def _classify_array(values: Sequence[Any]) -> str:
    if not values:
        # Empty — treat as a string array so the form gives a CSV
        # editor; the user can fill it with anything and tomlkit
        # will widen as needed.
        return "str_array"
    types = {type(v) for v in values}
    # bool is a subclass of int — strip it so a list of bools doesn't
    # get classified as int_array.
    if types == {bool}:
        return "complex"   # not common in the configs; render read-only
    if types <= {int}:
        return "int_array"
    if types <= {float, int}:
        return "float_array"
    if types == {str}:
        return "str_array"
    # Array of inline tables (dicts) → "table_array".  The form
    # widget renders it as an editable mini-table; ``set_leaf``
    # allows whole-list replacement.
    if all(isinstance(v, dict) for v in values):
        return "table_array"
    return "complex"


__all__ = [
    "DELETE",
    "Leaf",
    "Path",
    "PathSegment",
    "apply_double_hash_cutoff",
    "replace_document_text",
    "roundtrip_safe",
    "set_leaf",
    "split_at_double_hash",
    "walk_leaves",
]
