#!/usr/bin/env python3
"""Convert a campaign's Run Book .xlsx into ``run-lists/<year>.database.toml``.

The Run Book is the operator-maintained source of truth for what was
taken when.  Each year's book lives as a separate xlsx with a slightly
different column layout (the schema drifted year-over-year — 2023
predates the "Particle" + "GEM" + "Triggers" columns, 2024 added
gas-vs-aerogel mirror split but no Particle yet, 2025 has the full
modern schema).

This script preserves the **forward-inheritance** invariant the
dashboard's ``rundb`` reader expects: each TOML entry carries ONLY
the keys that changed from the previous merged view.  We walk the
rows in source order, maintaining a running "merged" state, and emit
a delta for every row that carries a real run id.

Per-year schemas are declared in ``SCHEMAS`` below — extend that
table when a new column shows up in a future Run Book.

Usage::

    python3 scripts/import_runbook.py \\
        --year 2024 \\
        --xlsx "/path/to/[2024][dRICH][testbeam] Run Book.xlsx" \\
        --out run-lists/2024.database.toml

    # Optional: extract every "LIST *" sheet into <year>.runlists.toml
    python3 scripts/import_runbook.py ... --runlists run-lists/2024.runlists.toml

Scope of NOT-imported fields (intentional):
  - ``radiators`` (structured array) — the xlsx description column
    (e.g. "aerogel 1.02 2x 2 cm tile") is captured as ``radiator_desc``
    for visibility, but the structured form (tag, refindex, depth,
    side) needs operator input that isn't in the book.  Fill via the
    dashboard runcard.
  - ``Archived`` flag — not tracked in the database schema.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any, Optional

import pandas as pd


RUN_ID_RE = re.compile(r"^\s*(\d{8}-\d{6})\s*$")


#  Per-year column maps.  Keys are the canonical database field names;
#  values are the 0-indexed column position in that year's main sheet.
#  Missing keys = field doesn't exist in that year's Run Book.
SCHEMAS: dict[str, dict] = {
    "2023": {
        "sheet": "Sheet1",
        "header_row": 1,
        "data_start": 3,
        "cols": {
            "radiator_desc":   2,
            "mirror_mm":       3,
            "beam_status":     4,
            "polarity":        5,
            "collimators":     6,
            "beam_energy":     7,
            "trigger":         8,
            "rdo_firmware":    9,
            "timing_firmware": 10,
            "temperature":     11,
            "v_bias":          12,
            "calibration":     13,
            "dcr_scan":        14,
            "run_id":          15,
            "n_spills":        16,
            "quality":         17,
            "notes":           19,
        },
    },
    "2024": {
        "sheet": "RunBook",
        "header_row": 1,
        "data_start": 3,
        "cols": {
            "radiator_gas":         2,
            "mirror_mm_gas":        3,
            "radiator_desc":        4,
            "mirror_mm":            5,
            "beam_status":          6,
            "polarity":             7,
            "collimators":          8,
            "beam_energy":          9,
            "trigger":              10,
            "rdo_firmware":         11,
            "timing_firmware":      12,
            "temperature":          13,
            "v_bias":               14,
            "calibration":          15,
            "dcr_scan":             16,
            "run_id":               17,
            "n_spills":             18,
            "gem":                  19,
            "available_triggers":   20,
            "quality":              21,
            "notes":                23,
        },
    },
}


#  Fields whose values get type-coerced before going into the TOML.
#  Anything not listed here goes through as a stripped string.
_INT_FIELDS = {"mirror_mm", "mirror_mm_gas", "n_spills", "collimators"}
_FLOAT_FIELDS = {"temperature", "beam_energy", "v_bias"}


def _coerce(field: str, raw: Any) -> Any:
    """Type-coerce a cell value per the field's documented type.

    Falls back to the raw string when coercion fails — the database
    file's TOML is permissive about type and the operator can see
    the bad cell on the next dashboard load.
    """
    if raw is None or (isinstance(raw, float) and pd.isna(raw)):
        return None
    s = str(raw).strip()
    if not s:
        return None
    if field in _INT_FIELDS:
        #  Strip trailing "mm" / units that crept into some 2023 rows.
        s2 = re.sub(r"[^\d\-\.]", "", s)
        try:
            return int(float(s2))
        except ValueError:
            return s
    if field in _FLOAT_FIELDS:
        #  Strip "V" / "C" suffixes ("52 V", "52V", "53.5 V").
        s2 = re.sub(r"[^\d\-\.]", "", s)
        try:
            return float(s2)
        except ValueError:
            return s
    return s


def _toml_value(v: Any) -> str:
    """Render a Python value as a TOML literal.

    Only the leaf types our database carries — kept narrow so we
    don't smuggle in YAML-ish exotic types by accident.
    """
    if v is None:
        return '""'
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        #  Avoid `1.0` collapsing to `1` on round-trip — keep the
        #  decimal point so a downstream consumer can't accidentally
        #  treat it as an int.
        if v.is_integer():
            return f"{v:.1f}"
        return repr(v)
    if isinstance(v, str):
        #  TOML basic strings forbid raw newlines / carriage returns /
        #  tabs — operator-entered multi-line cells need escaping.
        #  The order matters: backslash first so subsequent escapes
        #  don't get doubly-quoted.
        s = (v.replace("\\", "\\\\")
              .replace('"',  '\\"')
              .replace("\n", "\\n")
              .replace("\r", "\\r")
              .replace("\t", "\\t"))
        return '"' + s + '"'
    return '"' + str(v).replace('"', '\\"').replace("\n", "\\n") + '"'


def convert_runbook(
    xlsx_path: Path,
    year: str,
) -> list[tuple[str, dict[str, Any]]]:
    """Walk the runbook + return ``[(run_id, delta_dict), …]`` in chronological order.

    Strategy in two phases:

      1. **Snapshot pass** — walk every row in source order, forward-
         filling a ``merged`` state from non-empty cells.  Each time
         a row carries a run id, record ``last_state[run_id] = dict(merged)``.
         Duplicate run ids (the same Physics-cell appearing on two
         rows because the operator re-stated a calibration or made a
         correction) get the LATER row's full state — last-write-wins,
         matching the operator's revised intent.
      2. **Delta pass** — process ``last_state`` in run-id order
         (chronological since ids are zero-padded ``YYYYMMDD-HHMMSS``
         timestamps) and emit each entry's diff from the previous
         entry's full state.  The output is the same delta encoding
         the dashboard's ``rundb`` reader produces.

    Two-phase is required because a single-pass delta computation
    would emit two ``[runs."<id>"]`` blocks for any duplicate, which
    breaks ``tomllib`` (declared-twice error).
    """
    spec = SCHEMAS.get(year)
    if spec is None:
        raise ValueError(
            f"no schema for year {year!r} — extend SCHEMAS in this script"
        )

    df = pd.read_excel(xlsx_path, sheet_name=spec["sheet"], header=None)
    cols = spec["cols"]

    merged: dict[str, Any] = {}          # current forward-filled state
    last_state: dict[str, dict[str, Any]] = {}   # run_id → state snapshot

    for r_idx in range(spec["data_start"], len(df)):
        row = df.iloc[r_idx]

        #  Update the forward-fill state from every non-empty cell.
        for field, c_idx in cols.items():
            if field == "run_id":
                continue
            if c_idx >= len(row):
                continue
            cell = row.iat[c_idx] if c_idx < len(row) else None
            coerced = _coerce(field, cell)
            if coerced is not None:
                merged[field] = coerced

        run_cell = row.iat[cols["run_id"]] if cols["run_id"] < len(row) else None
        if not isinstance(run_cell, str):
            run_cell = "" if pd.isna(run_cell) else str(run_cell)
        m = RUN_ID_RE.match(run_cell)
        if not m:
            continue
        run_id = m.group(1)
        #  Last-write-wins on duplicates — operator's later row is the
        #  intended state.  We snapshot ``merged`` here, not just the
        #  cells from this row, so forward-inheritance still works.
        last_state[run_id] = dict(merged)

    #  Delta pass — sort by run id (chronological) so each entry's
    #  delta is against the previous entry's state, exactly matching
    #  the rundb reader's forward-inheritance walk.
    out: list[tuple[str, dict[str, Any]]] = []
    emitted: dict[str, Any] = {}
    for run_id in sorted(last_state.keys()):
        state = last_state[run_id]
        delta = {k: v for k, v in state.items() if emitted.get(k) != v}
        out.append((run_id, delta))
        emitted = state
    return out


def write_database_toml(
    entries: list[tuple[str, dict[str, Any]]],
    out_path: Path,
    year: str,
    xlsx_source: Path,
) -> None:
    """Render ``entries`` to a TOML file in the ``rundb`` schema."""
    lines: list[str] = []
    lines.append(f"# {year} full database of runs info")
    lines.append("# The first run defines the standard configuration,")
    lines.append("# all subsequent runs only specify fields that changed w.r.t the previous run.")
    lines.append("#")
    lines.append(f"# Source: imported from {xlsx_source.name}")
    lines.append("# Fields: mirror_mm, beam_status, particle, polarity, beam_energy,")
    lines.append("#         rdo_firmware, timing_firmware, temperature, v_bias,")
    lines.append("#         calibration, dcr_scan, n_spills, quality, notes")
    lines.append("")
    for run_id, delta in entries:
        lines.append(f'[runs."{run_id}"]')
        for k, v in delta.items():
            lines.append(f"{k} = {_toml_value(v)}")
        lines.append("")
    out_path.write_text("\n".join(lines))


# ---------------------------------------------------------------------------
# Runlist extraction — LIST * sheets in 2024
# ---------------------------------------------------------------------------


def _slugify(name: str) -> str:
    """Turn a sheet name like ``"LIST aerogel mirror scan"`` into a TOML key."""
    s = name.strip()
    if s.upper().startswith("LIST"):
        s = s[4:].strip()
    s = re.sub(r"[^\w\-]+", "_", s.lower())
    s = re.sub(r"_+", "_", s).strip("_")
    return s or "unnamed"


def extract_runlists(xlsx_path: Path) -> dict[str, list[str]]:
    """Pull every ``LIST *`` sheet's runs into ``{slug: [run_ids]}``.

    Honours the layout discovered on inspection: row 4 is the column
    header, rows 5+ are data with the run id in column 0.  Skips
    rows whose first cell doesn't parse as a run id.
    """
    out: dict[str, list[str]] = {}
    book = pd.read_excel(xlsx_path, sheet_name=None, header=None)
    for sheet, df in book.items():
        if not sheet.upper().startswith("LIST"):
            continue
        runs: list[str] = []
        for r in range(5, len(df)):
            cell = df.iat[r, 0]
            if not isinstance(cell, str):
                cell = "" if pd.isna(cell) else str(cell)
            m = RUN_ID_RE.match(cell)
            if m:
                runs.append(m.group(1))
        if runs:
            out[_slugify(sheet)] = runs
    return out


def write_runlists_toml(
    runlists: dict[str, list[str]],
    out_path: Path,
    year: str,
) -> None:
    lines: list[str] = []
    lines.append(f"# {year} runlists — extracted from the Run Book's LIST sheets.")
    lines.append("# Each named runlist groups runs that share an analysis intent.")
    lines.append("")
    for name in sorted(runlists.keys()):
        ids = runlists[name]
        lines.append(f"[runlists.{name}]")
        lines.append('campaign = ""   # fill in: short campaign tag')
        lines.append("runs = [")
        for rid in ids:
            lines.append(f'    "{rid}",')
        lines.append("]")
        lines.append("")
    out_path.write_text("\n".join(lines))


# ---------------------------------------------------------------------------
# Scaffold an empty year
# ---------------------------------------------------------------------------


def scaffold_empty_year(out_dir: Path, year: str) -> list[Path]:
    """Write empty ``<year>.database.toml`` + ``<year>.runlists.toml``.

    Returns the list of files created so the caller can log them.
    The audit file is created lazily by the dashboard on the first
    edit; we don't pre-seed it here.
    """
    created: list[Path] = []
    db = out_dir / f"{year}.database.toml"
    if not db.exists():
        db.write_text(
            f"# {year} full database of runs info\n"
            "# The first run defines the standard configuration,\n"
            "# all subsequent runs only specify fields that changed w.r.t the previous run.\n"
            "#\n"
            f"# Source: scaffolded empty by scripts/import_runbook.py\n"
            "# Add runs via the dashboard's Run Manager (Detect runs button) or\n"
            "# by importing this campaign's Run Book once it's available.\n"
        )
        created.append(db)
    rl = out_dir / f"{year}.runlists.toml"
    if not rl.exists():
        rl.write_text(
            f"# {year} runlists — empty.\n"
            "# Add named runlists via the dashboard's Runlists tab as analysis intents form.\n"
        )
        created.append(rl)
    return created


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="import_runbook",
        description="Convert a Run Book .xlsx into <year>.database.toml + .runlists.toml.",
    )
    parser.add_argument("--year", required=True,
                        help="Campaign year, e.g. 2024.  Used for the file headers + schema lookup.")
    parser.add_argument("--xlsx", type=Path, default=None,
                        help="Path to the Run Book .xlsx.  Omit to scaffold an empty year.")
    parser.add_argument("--out", type=Path, required=True,
                        help="Output database TOML path.")
    parser.add_argument("--runlists", type=Path, default=None,
                        help="Optional output path for the runlists TOML "
                             "(extracted from LIST * sheets).")
    parser.add_argument("--scaffold-empty", action="store_true",
                        help="Skip --xlsx and create empty database + runlists files.")
    args = parser.parse_args(argv)

    args.out.parent.mkdir(parents=True, exist_ok=True)

    if args.scaffold_empty:
        created = scaffold_empty_year(args.out.parent, args.year)
        for c in created:
            print(f"created {c}")
        if not created:
            print(f"all files already exist for {args.year}; nothing to do")
        return 0

    if args.xlsx is None:
        print("--xlsx is required unless --scaffold-empty is set", file=sys.stderr)
        return 2

    entries = convert_runbook(args.xlsx, args.year)
    write_database_toml(entries, args.out, args.year, args.xlsx)
    print(f"wrote {args.out} — {len(entries)} runs")

    if args.runlists is not None:
        runlists = extract_runlists(args.xlsx)
        write_runlists_toml(runlists, args.runlists, args.year)
        total_runs = sum(len(v) for v in runlists.values())
        print(f"wrote {args.runlists} — {len(runlists)} runlists, "
              f"{total_runs} run references")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
