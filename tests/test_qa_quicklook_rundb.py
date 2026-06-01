"""Unit tests for ``qa_quicklook.rundb``.

Covers the bits a normal shifter workflow actually depends on:
  - ``append_runs`` fast path (newer-than-everything) appends as-is
  - ``append_runs`` slow path inserts historical runs chronologically
  - Audit-log sibling path computation + idempotent file emit
  - ``update_run_field`` writes the audit log when ``source`` is set
  - ``update_run_field`` skips the audit log when ``source=""``
  - ``_toml_value`` rendering for None / bool / int / float / str

Run::

    .venv/bin/python -m unittest tests.test_qa_quicklook_rundb
"""

from __future__ import annotations

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

from qa_quicklook import rundb  # noqa: E402
from qa_quicklook.rundb import (  # noqa: E402
    _audit_path,
    _toml_value,
    append_runs,
    load_database,
    update_run_field,
)


def _write(path: Path, body: str) -> None:
    path.write_text(body.lstrip("\n"))


class TestAuditPath(unittest.TestCase):
    def test_appends_audit_toml_to_double_suffix_name(self) -> None:
        p = Path("/x/y/2025.database.toml")
        self.assertEqual(_audit_path(p).name, "2025.database.audit.toml")

    def test_handles_single_extension(self) -> None:
        p = Path("/x/y/foo.toml")
        self.assertEqual(_audit_path(p).name, "foo.audit.toml")

    def test_handles_no_extension(self) -> None:
        p = Path("/x/y/foo")
        self.assertEqual(_audit_path(p).name, "foo.audit.toml")


class TestTomlValue(unittest.TestCase):
    def test_none_emits_empty_string(self) -> None:
        self.assertEqual(_toml_value(None), '""')

    def test_bool_emits_lowercase(self) -> None:
        self.assertEqual(_toml_value(True), "true")
        self.assertEqual(_toml_value(False), "false")

    def test_int_and_float_emit_via_repr(self) -> None:
        self.assertEqual(_toml_value(42), "42")
        self.assertIn(".", _toml_value(3.5))

    def test_string_escapes_backslashes_and_quotes(self) -> None:
        self.assertEqual(_toml_value('a"b\\c'), '"a\\"b\\\\c"')


class TestAppendRuns(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self._db = Path(self._td.name) / "2025.database.toml"
        _write(self._db, '''
[runs."20251110-100000"]
v_bias = 35.0
''')

    def tearDown(self) -> None:
        self._td.cleanup()

    def test_fast_path_appends_newer_run(self) -> None:
        added = append_runs(self._db, ["20251112-080000"])
        self.assertEqual(added, 1)
        ids = [r.run_id for r in load_database(self._db)]
        self.assertEqual(ids, ["20251110-100000", "20251112-080000"])

    def test_slow_path_inserts_historical_run_chronologically(self) -> None:
        added = append_runs(self._db, ["20251109-050000"])
        self.assertEqual(added, 1)
        ids = [r.run_id for r in load_database(self._db)]
        #  Historical insert lands BEFORE the existing entry, not at
        #  the end of the file.  This is the bug-fix path verified by
        #  task #172's predecessor.
        self.assertEqual(ids, ["20251109-050000", "20251110-100000"])

    def test_mixed_call_in_one_invocation(self) -> None:
        #  Adding one historical + one future in the same call should
        #  produce a strictly-sorted source order.
        added = append_runs(self._db, ["20251112-080000", "20251109-050000"])
        self.assertEqual(added, 2)
        ids = [r.run_id for r in load_database(self._db)]
        self.assertEqual(
            ids,
            ["20251109-050000", "20251110-100000", "20251112-080000"],
        )

    def test_idempotent_on_existing(self) -> None:
        added = append_runs(self._db, ["20251110-100000"])
        self.assertEqual(added, 0)

    def test_empty_input_is_noop(self) -> None:
        self.assertEqual(append_runs(self._db, []), 0)


class TestUpdateRunFieldAudit(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self._db = Path(self._td.name) / "2025.database.toml"
        _write(self._db, '''
[runs."20251110-100000"]
v_bias = 35.0

[runs."20251112-080000"]
''')

    def tearDown(self) -> None:
        self._td.cleanup()

    def _audit_entries(self):
        audit = _audit_path(self._db)
        if not audit.is_file():
            return []
        return tomllib.loads(audit.read_text()).get("entry", [])

    def test_dashboard_source_emits_one_entry(self) -> None:
        update_run_field(self._db, "20251110-100000", "quality", "good",
                         source="dashboard")
        entries = self._audit_entries()
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["source"], "dashboard")
        self.assertEqual(entries[0]["run"], "20251110-100000")
        self.assertEqual(entries[0]["field"], "quality")
        self.assertEqual(entries[0]["new_value"], "good")

    def test_empty_source_suppresses_audit(self) -> None:
        update_run_field(self._db, "20251110-100000", "quality", "good",
                         source="")
        self.assertEqual(self._audit_entries(), [])

    def test_clearing_field_with_none_removes_override(self) -> None:
        #  Regression: clearing a set field (None new value) must NOT
        #  raise tomlkit ConvertError — it removes the per-run override
        #  so the field falls back to forward-inheritance.
        update_run_field(self._db, "20251110-100000", "v_bias", None,
                         source="dashboard")
        recs = load_database(self._db)
        rec = next(r for r in recs if r.run_id == "20251110-100000")
        self.assertNotIn("v_bias", rec.own_fields)

    def test_clearing_absent_field_is_noop(self) -> None:
        #  Clearing a field the run never set must not crash either.
        update_run_field(self._db, "20251112-080000", "note", None,
                         source="dashboard")
        recs = load_database(self._db)
        rec = next(r for r in recs if r.run_id == "20251112-080000")
        self.assertNotIn("note", rec.own_fields)

    def test_multiple_edits_accumulate(self) -> None:
        update_run_field(self._db, "20251110-100000", "quality", "good",
                         source="dashboard")
        update_run_field(self._db, "20251112-080000", "quality", "bad",
                         source="dashboard")
        entries = self._audit_entries()
        self.assertEqual(len(entries), 2)
        self.assertEqual(entries[0]["run"], "20251110-100000")
        self.assertEqual(entries[1]["run"], "20251112-080000")


class TestIsPopulatedRunDir(unittest.TestCase):
    """The phantom filter: a run dir lists only when it matches the
    run-id convention AND holds real content (not just markers)."""

    def setUp(self) -> None:
        self._tmp = tempfile.mkdtemp()
        self.data = Path(self._tmp)

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self._tmp, ignore_errors=True)

    def _mk(self, run_id: str) -> Path:
        d = self.data / run_id
        d.mkdir(parents=True)
        return d

    def test_dir_with_real_file_is_populated(self) -> None:
        d = self._mk("20251111-164951")
        (d / "lightdata.root").write_bytes(b"x")
        self.assertTrue(rundb.is_populated_run_dir(d))

    def test_dir_with_only_device_subdir_is_populated(self) -> None:
        d = self._mk("20251111-164951")
        (d / "rdo-196").mkdir()
        self.assertTrue(rundb.is_populated_run_dir(d))

    def test_empty_dir_is_phantom(self) -> None:
        d = self._mk("20251111-164951")
        self.assertFalse(rundb.is_populated_run_dir(d))

    def test_marker_only_dir_is_phantom(self) -> None:
        #  A pruned-to-nothing run left holding only bookkeeping
        #  dotfiles must NOT list as a real run.
        d = self._mk("20251111-164951")
        (d / ".DS_Store").write_bytes(b"x")
        (d / ".qa_managed").touch()
        (d / ".qa_persistent").touch()
        self.assertFalse(rundb.is_populated_run_dir(d))

    def test_non_run_id_name_is_rejected(self) -> None:
        d = self.data / "baseline_pre_refactor"
        d.mkdir()
        (d / "recodata.root").write_bytes(b"x")
        self.assertFalse(rundb.is_populated_run_dir(d))

    def test_list_populated_runs_filters_and_sorts(self) -> None:
        good1 = self._mk("20251111-164951"); (good1 / "f.root").write_bytes(b"x")
        good2 = self._mk("20260528-191848"); (good2 / "f.root").write_bytes(b"x")
        self._mk("20251112-042329")                       # phantom (empty)
        phantom2 = self._mk("20251117-173828")            # marker-only
        (phantom2 / ".DS_Store").write_bytes(b"x")
        (self.data / "not-a-run").mkdir()                 # non-run dir
        runs = rundb.list_populated_runs(self.data)
        #  Newest-first, phantoms + non-run dirs excluded.
        self.assertEqual(runs, ["20260528-191848", "20251111-164951"])


class TestNewestCampaignFile(unittest.TestCase):
    """Auto-newest campaign-file resolution for the dashboard default."""

    def setUp(self) -> None:
        self._tmp = tempfile.mkdtemp()
        self.rl = Path(self._tmp)

    def tearDown(self) -> None:
        import shutil
        shutil.rmtree(self._tmp, ignore_errors=True)

    def test_picks_highest_year(self) -> None:
        for y in ("2023", "2024", "2026", "2025"):
            (self.rl / f"{y}.database.toml").write_text("")
            (self.rl / f"{y}.runlists.toml").write_text("")
        self.assertEqual(
            rundb.newest_campaign_file(self.rl, "database").name,
            "2026.database.toml")
        self.assertEqual(
            rundb.newest_campaign_file(self.rl, "runlists").name,
            "2026.runlists.toml")

    def test_kind_isolation(self) -> None:
        #  A newer runlists-only year must not pull the database default.
        (self.rl / "2025.database.toml").write_text("")
        (self.rl / "2026.runlists.toml").write_text("")
        self.assertEqual(
            rundb.newest_campaign_file(self.rl, "database").name,
            "2025.database.toml")

    def test_fallback_when_none_present(self) -> None:
        got = rundb.newest_campaign_file(self.rl, "database", fallback_year=2025)
        self.assertEqual(got.name, "2025.database.toml")

    def test_ignores_non_campaign_files(self) -> None:
        (self.rl / "2099.database.toml.bak").write_text("")  # not a match
        (self.rl / "notes.txt").write_text("")
        (self.rl / "2025.database.toml").write_text("")
        self.assertEqual(
            rundb.newest_campaign_file(self.rl, "database").name,
            "2025.database.toml")


if __name__ == "__main__":
    unittest.main()
