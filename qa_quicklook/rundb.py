"""Run-database parser with forward-inheritance semantics.

Mirrors the C++ ``RunInfo::read_database()`` in
``include/utility/config_reader.h``: each entry in
``run-lists/2025.database.toml`` reads its own keys, missing keys
inherit from the previous entry.  Source order matters and is
chronological by construction (the run id is the timestamp).

Pure stdlib (well, ``tomlkit`` for the comment-preserving parse) —
no Qt.  The Runlist tab consumes ``RunRecord`` instances.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import tomlkit

# Read path: stdlib tomllib is ~600× faster than tomlkit on the
# ~260-entry production database (3 ms vs 1700 ms).  We use it for
# every read-only operation; tomlkit stays on the write paths where
# we need the round-trip comment-preserving parse.  Python < 3.11
# falls back to the ``tomli`` shim (a dev dependency).
if sys.version_info >= (3, 11):
    import tomllib as _tomllib
else:  # pragma: no cover
    import tomli as _tomllib  # type: ignore


@dataclass
class RunRecord:
    """One run's merged view.

    ``own_fields`` is the literal keys explicitly set on this run's
    ``[runs."<id>"]`` table.  ``inherited_fields`` are the keys
    that came from upstream entries via forward-inheritance.  The
    merged result is ``{**inherited_fields, **own_fields}``.
    """

    run_id: str
    own_fields: dict[str, Any] = field(default_factory=dict)
    inherited_fields: dict[str, Any] = field(default_factory=dict)

    @property
    def merged(self) -> dict[str, Any]:
        return {**self.inherited_fields, **self.own_fields}

    def get(self, key: str, default: Any = None) -> Any:
        return self.merged.get(key, default)


def load_database(path: Path) -> list[RunRecord]:
    """Parse ``path`` and return one ``RunRecord`` per run in source order.

    Forward-inheritance: the *N*-th record's ``inherited_fields`` is
    the merged view of records ``0..N-1`` (effectively the running
    merge of all previous entries' own fields).

    Uses stdlib ``tomllib`` for parsing — orders of magnitude faster
    than ``tomlkit`` on production-sized databases (260 entries:
    ~3 ms vs ~1700 ms).  Source order is preserved because Python
    dicts (3.7+) maintain insertion order, which ``tomllib`` honours.
    """
    if not path.is_file():
        return []
    try:
        with path.open("rb") as fh:
            doc = _tomllib.load(fh)
    except (OSError, _tomllib.TOMLDecodeError):
        return []
    runs = doc.get("runs")
    if not isinstance(runs, dict):
        return []

    records: list[RunRecord] = []
    running_merged: dict[str, Any] = {}
    for run_id, fields in runs.items():
        if not isinstance(fields, dict):
            continue
        # Defensive copy — tomllib already gives plain dicts, but we
        # want each record to own its own dict so later mutations
        # (auto-pin, etc.) on the doc don't leak in.
        own = dict(fields)
        records.append(RunRecord(
            run_id=str(run_id),
            own_fields=own,
            inherited_fields=dict(running_merged),
        ))
        running_merged.update(own)
    return records


def detect_new_runs(database_path: Path, data_dir: Path) -> list[str]:
    """Return run ids present under ``data_dir`` but not in the database.

    Sorted alphabetically, which is chronological since run ids are
    timestamps ``YYYYMMDD-HHMMSS``.

    Reads the existing run ids via tomllib directly (skipping the
    full ``load_database`` record construction) — only the top-level
    keys of ``[runs]`` matter for the diff.
    """
    if not data_dir.is_dir():
        return []
    on_disk = {p.name for p in data_dir.iterdir() if p.is_dir()}
    if not on_disk:
        return []
    existing: set[str] = set()
    if database_path.is_file():
        try:
            with database_path.open("rb") as fh:
                doc = _tomllib.load(fh)
            runs = doc.get("runs")
            if isinstance(runs, dict):
                existing = set(runs.keys())
        except (OSError, _tomllib.TOMLDecodeError):
            pass
    return sorted(on_disk - existing)


def append_runs(database_path: Path, new_run_ids: list[str]) -> int:
    """Insert empty ``[runs."<id>"]`` blocks at their chronological slot.

    Run ids are timestamps (``YYYYMMDD-HHMMSS``), so lexical sort is
    chronological sort.  ``load_database`` returns runs in file
    order, and forward-inheritance walks them in that same order to
    build the merged view — so a historical run wedged at the tail
    of the file would inherit from runs that are actually *after* it,
    which is wrong.  This function preserves the chronological
    invariant:

      - Fast path: every new id is later than every existing id.
        Append in order (cheap, preserves trivia between blocks).
      - Slow path: at least one new id slots between or before
        existing entries.  Rebuild the ``[runs]`` table by re-
        inserting every entry in sorted order — comments inside
        each ``[runs."X"]`` sub-table survive (sub-tables are
        re-attached by reference), but trivia between blocks (rare
        in this database) is reflowed.

    Returns the number of entries actually written (deduped against
    the file's current state).

    Atomic write: ``write_text`` to a sibling ``.tmp`` then
    ``os.replace`` so a half-written database is never observable.
    """
    if not new_run_ids:
        return 0
    text = database_path.read_text() if database_path.is_file() else ""
    doc = tomlkit.parse(text) if text else tomlkit.document()
    runs = doc.setdefault("runs", tomlkit.table())

    existing_keys = list(runs.keys())
    existing_set = set(existing_keys)
    to_add = sorted({rid for rid in new_run_ids if rid not in existing_set})
    if not to_add:
        return 0

    #  Fast path — every new id is strictly later than every
    #  existing id, so simple ``runs[k] = table()`` appends are
    #  already chronological.  Empty existing list also counts:
    #  one-shot bootstrap.
    fast_path = (
        not existing_keys
        or to_add[0] > max(existing_keys)
    )

    if fast_path:
        for run_id in to_add:
            runs[run_id] = tomlkit.table()
    else:
        #  Slow path: at least one new id belongs in the middle (or
        #  before) the existing entries.  Snapshot existing sub-
        #  tables by reference, wipe ``[runs]``, then re-insert
        #  every key in sorted order.  Sub-table contents and
        #  comments inside them survive intact.
        snapshot = {k: runs[k] for k in existing_keys}
        for k in existing_keys:
            del runs[k]
        merged_order = sorted(existing_set | set(to_add))
        for k in merged_order:
            runs[k] = snapshot[k] if k in snapshot else tomlkit.table()

    added = len(to_add)
    new_text = tomlkit.dumps(doc)
    tmp = database_path.with_suffix(database_path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, database_path)
    return added


def _audit_path(database_path: Path) -> Path:
    """Sibling audit log next to ``database_path``.

    Drops the ``.toml`` extension and appends ``.audit.toml``, so
    ``run-lists/2025.database.toml`` →
    ``run-lists/2025.database.audit.toml``.  Mirrors the C++
    ``util::audit::sibling_audit_path`` convention in
    ``include/utility/audit.h`` so all provenance logs in the repo
    follow one naming rule.
    """
    #  ``2025.database.toml`` has Path.suffix == ``.toml`` and a
    #  ``.database`` stem-extension we MUST preserve, so naive
    #  ``with_suffix(".audit.toml")`` would silently drop it.  Build
    #  the sibling by string surgery on the leaf name instead.
    name = database_path.name
    if name.endswith(".toml"):
        name = name[: -len(".toml")]
    return database_path.with_name(name + ".audit.toml")


def _toml_value(v: Any) -> str:
    """Render ``v`` as a TOML literal for the audit log.

    Only the small handful of leaf types our database carries:
    ``str``, ``bool``, ``int``, ``float``, ``None`` (emitted as
    ``""`` since TOML has no null).  Everything else falls through
    to a quoted ``repr`` — never the right answer but at least
    auditable when a new exotic type appears.
    """
    if v is None:
        return '""'
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return '"' + repr(v).replace('"', '\\"') + '"'


def _append_audit_entry(
    database_path: Path,
    *,
    source: str,
    run: str,
    field: str,
    old_value: Any,
    new_value: Any,
) -> None:
    """Append one ``[[entry]]`` block to the database's audit log.

    Open-append + line-oriented output — multiple processes appending
    in parallel won't tear an entry because each call's write is
    small (~150 B) and POSIX ``O_APPEND`` guarantees atomicity at the
    fs level for writes under PIPE_BUF.  No load-merge-rewrite cycle
    is needed since the log is append-only by design.
    """
    from datetime import datetime
    audit = _audit_path(database_path)
    block = (
        "[[entry]]\n"
        f'at        = "{datetime.now().isoformat(timespec="seconds")}"\n'
        f'source    = "{source}"\n'
        f'run       = "{run}"\n'
        f'field     = "{field}"\n'
        f'old_value = {_toml_value(old_value)}\n'
        f'new_value = {_toml_value(new_value)}\n\n'
    )
    try:
        with audit.open("a", encoding="utf-8") as fh:
            fh.write(block)
    except OSError as e:
        # Audit failures must never block the primary edit — the
        # caller has already written the new value to disk.  Log to
        # stderr so the operator notices, but don't raise.
        print(f"[rundb] WARNING: audit write to {audit} failed: {e}",
              file=sys.stderr)


def update_run_field(
    database_path: Path,
    run_id: str,
    field: str,
    new_value: Any,
    *,
    auto_pin: bool = True,
    source: str = "dashboard",
) -> None:
    """Set ``field`` on ``run_id``, optionally auto-pinning downstream.

    Forward-inheritance rule: changing a field on run ``N`` silently
    changes every subsequent run whose own keys don't override that
    field.  ``auto_pin=True`` preserves downstream values by writing
    the OLD value onto the next inheriting run (the very next one
    that didn't have ``field`` explicitly set), so only run ``N``
    changes.

    No-op when the new value equals the merged-view value.

    ``source`` tags the audit-log entry written next to the database
    (``<db>.audit.toml``) — defaults to ``"dashboard"`` because every
    in-process call lands here from the GUI today.  Pass ``""`` to
    suppress the log (e.g. internal migration code that shouldn't
    show up as a user edit).
    """
    if not database_path.is_file():
        return
    doc = tomlkit.parse(database_path.read_text())
    runs = doc.get("runs")
    if runs is None or run_id not in runs:
        return

    # Compute the records BEFORE applying the edit so we can spot the
    # next inheriting run and capture its currently-merged value.
    records = load_database(database_path)
    record_index = {r.run_id: i for i, r in enumerate(records)}
    if run_id not in record_index:
        return
    idx = record_index[run_id]
    old_merged_value = records[idx].get(field)

    if (
        auto_pin
        and field not in records[idx].own_fields
        and old_merged_value is not None
    ):
        # Find the next run that doesn't explicitly set ``field`` and
        # pin the OLD merged value there so its merged view is
        # preserved.  We only need to pin on the *first* such run —
        # subsequent ones inherit from it.  No-op when the previous
        # merged value was absent: there is nothing to preserve.
        for j in range(idx + 1, len(records)):
            if field in records[j].own_fields:
                break
            # Pin here.
            pin_id = records[j].run_id
            runs[pin_id][field] = old_merged_value
            break

    # Apply the actual edit on the target run.
    runs[run_id][field] = new_value

    new_text = tomlkit.dumps(doc)
    tmp = database_path.with_suffix(database_path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, database_path)

    # Provenance log — written *after* the primary file lands so a
    # failed primary write doesn't produce orphan audit entries.
    if source:
        _append_audit_entry(
            database_path,
            source=source,
            run=run_id,
            field=field,
            old_value=old_merged_value,
            new_value=new_value,
        )


def delete_runs(
    database_path: Path,
    run_ids: list[str],
    *,
    preserve_inheritance: bool = True,
) -> int:
    """Remove ``[runs."<id>"]`` blocks, optionally preserving downstream views.

    Forward-inheritance gotcha: deleting run *N* would silently
    change every subsequent run whose merged view used to pull from
    ``N``'s own fields.  ``preserve_inheritance=True`` (the default)
    migrates each of *N*'s own fields onto the next run that doesn't
    already set them, so downstream merged views stay byte-identical
    to before the delete.  Symmetric counterpart of ``update_run_field``'s
    auto-pin.

    Returns the number of runs actually removed (deduped against
    what's in the file).
    """
    if not database_path.is_file() or not run_ids:
        return 0
    doc = tomlkit.parse(database_path.read_text())
    runs = doc.get("runs")
    if runs is None:
        return 0

    targets = [rid for rid in run_ids if rid in runs]
    if not targets:
        return 0

    if preserve_inheritance:
        # Walk the records (in source order) so we know which fields
        # each victim run was the OWN-er of, and which downstream
        # run will be left holding the bag (= first surviving run
        # past the victim that doesn't set the field).
        records = load_database(database_path)
        record_index = {r.run_id: i for i, r in enumerate(records)}
        target_set = set(targets)
        for victim in targets:
            idx = record_index.get(victim)
            if idx is None:
                continue
            for field, value in records[idx].own_fields.items():
                # Find the first surviving downstream run that doesn't
                # already override this field — pin the value there
                # so the merged view is unchanged for everyone past
                # the victim.
                for j in range(idx + 1, len(records)):
                    nxt = records[j]
                    if nxt.run_id in target_set:
                        continue
                    if field in nxt.own_fields:
                        # Downstream already sets it → nothing to do.
                        break
                    runs[nxt.run_id][field] = value
                    break

    removed = 0
    for rid in targets:
        del runs[rid]
        removed += 1
    if removed == 0:
        return 0

    new_text = tomlkit.dumps(doc)
    tmp = database_path.with_suffix(database_path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, database_path)
    return removed


def read_schema_extras(database_path: Path) -> list[str]:
    """Return user-declared extra field names from ``[schema] extra_fields``.

    These are fields the operator added via the dashboard's
    "+ Field" button so the runcard skeleton includes them on every
    run as a ``· unset`` slot — even though no run has actually set
    a value yet.  No value is ever stored under any run for an extra
    field that's only declared in the schema; that's the whole point
    of the "propagates backward as unset" UX.
    """
    if not database_path.is_file():
        return []
    try:
        with database_path.open("rb") as fh:
            doc = _tomllib.load(fh)
    except (OSError, _tomllib.TOMLDecodeError):
        return []
    schema = doc.get("schema")
    if not isinstance(schema, dict):
        return []
    extras = schema.get("extra_fields") or []
    return [str(x) for x in extras]


def add_schema_extra(database_path: Path, field_name: str) -> bool:
    """Append ``field_name`` to ``[schema] extra_fields`` if not present.

    Returns ``True`` if the field was actually added, ``False`` when
    it was already in the schema (either as an extra or as a real
    field on some run).  Raises ``ValueError`` for empty / clearly
    illegal names.
    """
    field_name = field_name.strip()
    if not field_name:
        raise ValueError("field name cannot be empty")
    # Keep the namespace clean — TOML allows almost anything as a key
    # but downstream registries (units, enums) are happier with
    # snake_case-ish identifiers.
    if not all(c.isalnum() or c in "_-" for c in field_name):
        raise ValueError(
            f"field name {field_name!r} must be alphanumeric / underscore / dash"
        )

    text = database_path.read_text() if database_path.is_file() else ""
    doc = tomlkit.parse(text) if text else tomlkit.document()
    schema = doc.get("schema")
    if schema is None:
        schema = tomlkit.table()
        doc["schema"] = schema
    extras = schema.get("extra_fields")
    if extras is None:
        extras = tomlkit.array()
        schema["extra_fields"] = extras
    current = [str(x) for x in extras]
    if field_name in current:
        return False
    extras.append(field_name)

    new_text = tomlkit.dumps(doc)
    tmp = database_path.with_suffix(database_path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, database_path)
    return True


@dataclass
class ResultEntry:
    """One ``(value, error)`` reading on a single ``(run, sensor, quantity)`` key.

    Mirrors the C++ ``ResultEntry`` in ``include/analysis_results.h``.
    ``error == 0`` is the documented sentinel for dimensionless
    diagnostics where an uncertainty isn't meaningful.
    """

    value: float
    error: float = 0.0


def results_load(path: Path) -> dict:
    """Read the dashboard-side TOML mirror of ``AnalysisResults``.

    Returns a nested dict::

        { "<run>": { "<sensor>": { "<quantity>": ResultEntry } } }

    Designed for cheap slicing — picking a (sensor, quantity) trend
    across a runlist is two nested ``.get`` calls + one filter.
    Empty dict on missing / unparseable file so callers can treat
    "no results yet" the same as "nothing to plot".

    Schema produced by ``AnalysisResults::update`` (C++) is::

        [results."<run>"."<sensor>"]
        "<quantity>" = { value = X, error = Y }   # error optional
    """
    if not path.is_file():
        return {}
    try:
        with path.open("rb") as fh:
            doc = _tomllib.load(fh)
    except (OSError, _tomllib.TOMLDecodeError):
        return {}
    results = doc.get("results")
    if not isinstance(results, dict):
        return {}
    out: dict[str, dict[str, dict[str, ResultEntry]]] = {}
    for run, sensors in results.items():
        if not isinstance(sensors, dict):
            continue
        run_out: dict[str, dict[str, ResultEntry]] = {}
        for sensor, quantities in sensors.items():
            if not isinstance(quantities, dict):
                continue
            qmap: dict[str, ResultEntry] = {}
            for q, leaf in quantities.items():
                if not isinstance(leaf, dict):
                    continue
                try:
                    qmap[str(q)] = ResultEntry(
                        value=float(leaf.get("value", 0.0)),
                        error=float(leaf.get("error", 0.0)),
                    )
                except (TypeError, ValueError):
                    continue
            if qmap:
                run_out[str(sensor)] = qmap
        if run_out:
            out[str(run)] = run_out
    return out


def save_selection_as_runlist(
    runlists_path: Path,
    name: str,
    run_ids: list[str],
) -> None:
    """Append a named runlist to ``run-lists/2025.runlists.toml``.

    Adds ``[runlists.<name>]`` with ``runs = [...]`` preserving
    comments via tomlkit.  If ``name`` already exists, raises
    ``ValueError`` — the caller should rename or confirm overwrite.
    """
    text = runlists_path.read_text() if runlists_path.is_file() else ""
    doc = tomlkit.parse(text) if text else tomlkit.document()
    runlists = doc.setdefault("runlists", tomlkit.table())
    if name in runlists:
        raise ValueError(f"runlist {name!r} already exists in {runlists_path.name}")
    entry = tomlkit.table()
    arr = tomlkit.array()
    for r in run_ids:
        arr.append(r)
    entry["runs"] = arr
    runlists[name] = entry
    new_text = tomlkit.dumps(doc)
    tmp = runlists_path.with_suffix(runlists_path.suffix + ".tmp")
    tmp.write_text(new_text)
    import os
    os.replace(tmp, runlists_path)


__all__ = [
    "ResultEntry",
    "RunRecord",
    "add_schema_extra",
    "append_runs",
    "delete_runs",
    "detect_new_runs",
    "load_database",
    "read_schema_extras",
    "results_load",
    "save_selection_as_runlist",
    "update_run_field",
]
