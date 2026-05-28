"""Unit tests for ``scripts/backfill_audit_legacy.py``.

Exercises the one-shot migration that tags pre-history run-DB
records with ``source="legacy"``:
  - Fresh run on a small DB writes exactly one entry per (run, field)
    tuple, the audit file is parseable as TOML.
  - Re-run on the same DB is idempotent (skips already-migrated).
  - ``--dry-run`` doesn't touch disk.

Run::

    .venv/bin/python -m unittest tests.test_backfill_audit_legacy
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

if sys.version_info >= (3, 11):
    import tomllib
else:  # pragma: no cover
    import tomli as tomllib  # type: ignore


def _load_script():
    """Import scripts/backfill_audit_legacy.py as a module.

    Not under any package so we side-load via importlib.  Keeps the
    test agnostic to the script's filename if it ever moves.
    """
    path = REPO_ROOT / "scripts" / "backfill_audit_legacy.py"
    spec = importlib.util.spec_from_file_location("backfill_audit_legacy", path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


class TestBackfill(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self._db = Path(self._td.name) / "2025.database.toml"
        self._db.write_text(
            '[runs."20251110-100000"]\n'
            'v_bias = 35.0\n'
            'quality = "good"\n'
            '\n'
            '[runs."20251112-080000"]\n'
            'v_bias = 36.0\n'
        )
        self._mod = _load_script()

    def tearDown(self) -> None:
        self._td.cleanup()

    def _audit_path(self) -> Path:
        return self._db.with_name("2025.database.audit.toml")

    def test_first_pass_writes_one_entry_per_field(self) -> None:
        stats = self._mod._backfill_one(self._db, dry_run=False)
        self.assertEqual(stats["considered"], 3)  # 2 + 1 own fields
        self.assertEqual(stats["skipped"], 0)
        self.assertEqual(stats["appended"], 3)
        audit = tomllib.loads(self._audit_path().read_text())
        self.assertEqual(len(audit.get("entry", [])), 3)
        sources = {e["source"] for e in audit["entry"]}
        self.assertEqual(sources, {"legacy"})

    def test_second_pass_is_idempotent(self) -> None:
        self._mod._backfill_one(self._db, dry_run=False)
        stats = self._mod._backfill_one(self._db, dry_run=False)
        self.assertEqual(stats["appended"], 0)
        self.assertEqual(stats["skipped"], 3)

    def test_dry_run_does_not_touch_disk(self) -> None:
        stats = self._mod._backfill_one(self._db, dry_run=True)
        self.assertEqual(stats["appended"], 3)
        self.assertFalse(self._audit_path().exists())

    def test_toml_value_helpers(self) -> None:
        self.assertEqual(self._mod._toml_value(None), '""')
        self.assertEqual(self._mod._toml_value(True), "true")
        self.assertEqual(self._mod._toml_value(42), "42")
        #  String escaping mirrors rundb's behaviour.
        self.assertEqual(self._mod._toml_value('a"b'), '"a\\"b"')


if __name__ == "__main__":
    unittest.main()
