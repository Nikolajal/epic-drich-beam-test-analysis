#!/usr/bin/env python3
"""One-shot backfill: tag pre-history run-DB records with ``source="legacy"``.

When the audit log went live (task #156), the dashboard started
emitting one ``[[entry]]`` per (run, field, value) edit into a
sibling ``<db>.audit.toml`` file.  Records that existed *before*
that switch have no audit trail at all — they're indistinguishable
from a record that was edited but somehow lost its history.

This script closes that gap by walking every ``run-lists/*.database.toml``,
inspecting the **own** fields on each ``[runs."<id>"]`` block (inherited
fields are not "edits" — only ones the operator explicitly set count),
and appending a corresponding ``[[entry]]`` to the sibling audit log
with ``source = "legacy"`` and the current value as ``new_value``.
The ``old_value`` slot is left empty since by definition we don't
know what came before.

Idempotent: a (source, run, field) trio already present in the audit
file is skipped, so re-runs are safe.  Emits a summary at the end:
how many entries it considered, how many it skipped (already there),
and how many it actually appended.

Usage
-----

    scripts/backfill_audit_legacy.py [--dry-run] [PATH ...]

Without positional arguments, walks every ``run-lists/*.database.toml``
under the current working directory.  Pass explicit paths to scope
the migration (e.g. just ``run-lists/2025.database.toml``).

``--dry-run`` reports what would happen without touching disk.
"""

from __future__ import annotations

import argparse
import sys
from datetime import datetime
from pathlib import Path


def _load_existing_audit(audit_path: Path) -> set[tuple[str, str, str]]:
    """Set of ``(source, run, field)`` keys already in ``audit_path``.

    Uses ``tomllib`` for the read — comment-preserving round-trip
    isn't needed because we only ever *append* to the audit file.
    Missing file → empty set (first run).
    """
    if not audit_path.is_file():
        return set()
    if sys.version_info >= (3, 11):
        import tomllib
    else:  # pragma: no cover
        import tomli as tomllib  # type: ignore
    try:
        with audit_path.open("rb") as fh:
            doc = tomllib.load(fh)
    except Exception:  # noqa: BLE001
        # Unparseable audit file → treat as empty so we don't refuse
        # to migrate.  Operator can rotate the broken file later.
        return set()
    out: set[tuple[str, str, str]] = set()
    for entry in doc.get("entry", []):
        if not isinstance(entry, dict):
            continue
        key = (
            str(entry.get("source", "")),
            str(entry.get("run", "")),
            str(entry.get("field", "")),
        )
        out.add(key)
    return out


def _toml_value(v) -> str:
    """Render ``v`` as a TOML literal — same surface as rundb._toml_value.

    Kept in-script (not imported from rundb) so this migration runs
    standalone without the Qt-heavy dashboard import chain.
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


def _audit_path_for(db_path: Path) -> Path:
    """Sibling ``<basename>.audit.toml`` for ``db_path``.

    Mirrors ``qa_quicklook.rundb._audit_path`` so the on-disk layout
    is consistent.  Inlined here to avoid the dashboard import.
    """
    name = db_path.name
    if name.endswith(".toml"):
        name = name[: -len(".toml")]
    return db_path.with_name(name + ".audit.toml")


def _backfill_one(db_path: Path, *, dry_run: bool) -> dict[str, int]:
    """Run the migration on a single database; return stats."""
    if sys.version_info >= (3, 11):
        import tomllib
    else:  # pragma: no cover
        import tomli as tomllib  # type: ignore

    with db_path.open("rb") as fh:
        doc = tomllib.load(fh)

    runs = doc.get("runs")
    if not isinstance(runs, dict):
        return {"considered": 0, "skipped": 0, "appended": 0}

    audit_path = _audit_path_for(db_path)
    existing = _load_existing_audit(audit_path)

    stamp = datetime.now().isoformat(timespec="seconds")
    blocks: list[str] = []
    considered = skipped = appended = 0

    for run_id, run_block in runs.items():
        if not isinstance(run_block, dict):
            continue
        for field, value in run_block.items():
            considered += 1
            key = ("legacy", run_id, field)
            if key in existing:
                skipped += 1
                continue
            blocks.append(
                "[[entry]]\n"
                f'at        = "{stamp}"\n'
                'source    = "legacy"\n'
                f'run       = "{run_id}"\n'
                f'field     = "{field}"\n'
                'old_value = ""\n'
                f'new_value = {_toml_value(value)}\n\n'
            )
            existing.add(key)  # protect against same-(run, field) twice in one DB
            appended += 1

    if blocks and not dry_run:
        with audit_path.open("a", encoding="utf-8") as fh:
            #  Tag the first append with a header so the file's
            #  provenance is self-describing on a human read.
            if not audit_path.exists() or audit_path.stat().st_size == 0:
                fh.write(
                    "# Audit log for " + db_path.name + "\n"
                    "# Appended by scripts/backfill_audit_legacy.py "
                    "for entries that pre-date task #156.\n\n"
                )
            for block in blocks:
                fh.write(block)

    return {"considered": considered, "skipped": skipped, "appended": appended}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", type=Path,
                        help="Specific database files to backfill.  "
                             "Default: every run-lists/*.database.toml.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Report what would change without writing.")
    args = parser.parse_args()

    if args.paths:
        targets = [p for p in args.paths if p.is_file()]
    else:
        targets = sorted(Path("run-lists").glob("*.database.toml"))
    if not targets:
        print("no database files found", file=sys.stderr)
        return 1

    total = {"considered": 0, "skipped": 0, "appended": 0}
    for db in targets:
        stats = _backfill_one(db, dry_run=args.dry_run)
        audit = _audit_path_for(db)
        print(f"{db.name}: considered {stats['considered']}, "
              f"skipped {stats['skipped']}, "
              f"appended {stats['appended']} → {audit.name}"
              + ("  [DRY-RUN]" if args.dry_run else ""))
        for k in total:
            total[k] += stats[k]

    print(f"\nTOTAL  considered={total['considered']}  "
          f"skipped={total['skipped']}  appended={total['appended']}"
          + ("  [DRY-RUN]" if args.dry_run else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
