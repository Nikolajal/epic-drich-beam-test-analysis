"""Unit tests for ``qa_quicklook.conf_layout``.

Builds a fake ``conf/`` tree per test in a tempdir so the helpers
exercise real symlinks without touching the repo's actual ``conf/``.

Run::

    .venv/bin/python -m unittest tests.test_qa_quicklook_conf_layout
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook.conf_layout import (  # noqa: E402
    DEFAULTS_DIR,
    SETS_DIR,
    WORKING_DIR,
    MasterKind,
    active_set_name,
    list_sets,
    promote_to_working,
    scan,
    switch_set,
)


def _make_layout(base: Path, *, sets: dict[str, list[str]] | None = None) -> Path:
    """Build a minimal conf-tree under ``base`` and return it.

    ``sets`` is a mapping ``{set_name: [file_basename, ...]}`` — each
    entry creates a set with one file per name.  Default content for
    every file is ``"# from <where>\\n"`` so tests can distinguish.
    """
    sets = sets or {}
    conf = base / "conf"
    conf.mkdir()
    (conf / DEFAULTS_DIR).mkdir()
    (conf / SETS_DIR).mkdir()

    # Three masters, each backed by a default.
    for name in ("alpha.toml", "beta.toml", "gamma.toml"):
        (conf / DEFAULTS_DIR / name).write_text(f"# default {name}\n")
        os.symlink(Path(DEFAULTS_DIR) / name, conf / name)

    for set_name, files in sets.items():
        d = conf / SETS_DIR / set_name
        d.mkdir(parents=True)
        for fname in files:
            (d / fname).write_text(f"# set {set_name} {fname}\n")

    return conf


class TestScanAndClassify(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="conflayout-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_all_default(self):
        conf = _make_layout(self.tmp)
        entries = scan(conf)
        self.assertEqual({e.name for e in entries}, {"alpha.toml", "beta.toml", "gamma.toml"})
        self.assertTrue(all(e.kind == MasterKind.DEFAULT for e in entries))
        self.assertEqual(active_set_name(entries), "default")

    def test_master_pointing_at_a_set(self):
        conf = _make_layout(self.tmp, sets={"2024": ["alpha.toml"]})
        os.unlink(conf / "alpha.toml")
        os.symlink(Path(SETS_DIR) / "2024" / "alpha.toml", conf / "alpha.toml")

        entries = scan(conf)
        alpha = next(e for e in entries if e.name == "alpha.toml")
        self.assertEqual(alpha.kind, MasterKind.SET)
        self.assertEqual(alpha.set_name, "2024")
        # The other two are still at default — active name is "2024".
        self.assertEqual(active_set_name(entries), "2024")

    def test_mixed_sets_collapses_to_mixed(self):
        conf = _make_layout(
            self.tmp,
            sets={"2024": ["alpha.toml"], "2025": ["beta.toml"]},
        )
        os.unlink(conf / "alpha.toml")
        os.unlink(conf / "beta.toml")
        os.symlink(Path(SETS_DIR) / "2024" / "alpha.toml", conf / "alpha.toml")
        os.symlink(Path(SETS_DIR) / "2025" / "beta.toml", conf / "beta.toml")

        entries = scan(conf)
        self.assertEqual(active_set_name(entries), "mixed")

    def test_working_overrides_show_as_working(self):
        conf = _make_layout(self.tmp)
        working = conf / WORKING_DIR
        working.mkdir()
        (working / "alpha.toml").write_text("# working\n")
        os.unlink(conf / "alpha.toml")
        os.symlink(Path(WORKING_DIR) / "alpha.toml", conf / "alpha.toml")

        entries = scan(conf)
        self.assertEqual(active_set_name(entries), "working")

    def test_dangling_symlink_classified_as_missing(self):
        conf = _make_layout(self.tmp)
        os.unlink(conf / "alpha.toml")
        os.symlink(Path(DEFAULTS_DIR) / "does_not_exist.toml", conf / "alpha.toml")
        entries = scan(conf)
        alpha = next(e for e in entries if e.name == "alpha.toml")
        self.assertEqual(alpha.kind, MasterKind.MISSING)


class TestListSets(unittest.TestCase):
    def test_lists_sets_alphabetically(self):
        base = Path(tempfile.mkdtemp(prefix="conflayout-sets-"))
        try:
            conf = _make_layout(base, sets={"2024": ["alpha.toml"], "2023": ["alpha.toml"]})
            self.assertEqual(list_sets(conf), ["2023", "2024"])
        finally:
            shutil.rmtree(base, ignore_errors=True)

    def test_no_sets_dir_returns_empty(self):
        base = Path(tempfile.mkdtemp(prefix="conflayout-sets-empty-"))
        try:
            conf = base / "conf"
            conf.mkdir()
            self.assertEqual(list_sets(conf), [])
        finally:
            shutil.rmtree(base, ignore_errors=True)


class TestPromoteToWorking(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="conflayout-promote-"))
        self.conf = _make_layout(self.tmp)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_promote_copies_current_content_to_working(self):
        master = self.conf / "alpha.toml"
        working = promote_to_working(master)

        self.assertTrue(working.exists())
        self.assertTrue(master.is_symlink())
        self.assertEqual(os.readlink(master), str(Path(WORKING_DIR) / "alpha.toml"))
        self.assertEqual(working.read_text(), "# default alpha.toml\n")
        # Default file is untouched.
        self.assertEqual(
            (self.conf / DEFAULTS_DIR / "alpha.toml").read_text(),
            "# default alpha.toml\n",
        )

    def test_promote_idempotent(self):
        master = self.conf / "alpha.toml"
        w1 = promote_to_working(master)
        # Operator edits the working copy.
        w1.write_text("# operator edits\n")
        # Second promote must NOT overwrite the in-flight edits.
        w2 = promote_to_working(master)
        self.assertEqual(w1, w2)
        self.assertEqual(w2.read_text(), "# operator edits\n")

    def test_promote_from_a_set(self):
        # First switch to a set, then promote.
        sets_dir = self.conf / SETS_DIR / "2024"
        sets_dir.mkdir(parents=True)
        (sets_dir / "alpha.toml").write_text("# set 2024 alpha\n")
        master = self.conf / "alpha.toml"
        master.unlink()
        os.symlink(Path(SETS_DIR) / "2024" / "alpha.toml", master)

        working = promote_to_working(master)
        self.assertEqual(working.read_text(), "# set 2024 alpha\n")


class TestSwitchSet(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="conflayout-switch-"))
        self.conf = _make_layout(
            self.tmp,
            sets={"2024": ["alpha.toml", "beta.toml"], "2025": ["alpha.toml"]},
        )

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_switch_to_set_repoints_covered_masters(self):
        entries = switch_set(self.conf, "2024")
        by_name = {e.name: e for e in entries}
        self.assertEqual(by_name["alpha.toml"].kind, MasterKind.SET)
        self.assertEqual(by_name["alpha.toml"].set_name, "2024")
        self.assertEqual(by_name["beta.toml"].kind, MasterKind.SET)
        self.assertEqual(by_name["beta.toml"].set_name, "2024")
        # gamma isn't in 2024 → falls back to default.
        self.assertEqual(by_name["gamma.toml"].kind, MasterKind.DEFAULT)

    def test_switch_back_to_default(self):
        switch_set(self.conf, "2024")
        entries = switch_set(self.conf, "default")
        self.assertTrue(all(e.kind == MasterKind.DEFAULT for e in entries))

    def test_switch_to_unknown_set_raises(self):
        with self.assertRaises(ValueError):
            switch_set(self.conf, "nope")

    def test_switch_does_not_delete_working_files(self):
        promote_to_working(self.conf / "alpha.toml")
        (self.conf / WORKING_DIR / "alpha.toml").write_text("# my edits\n")
        switch_set(self.conf, "2024")
        # Working file still on disk (operator can recover it).
        self.assertEqual(
            (self.conf / WORKING_DIR / "alpha.toml").read_text(),
            "# my edits\n",
        )


if __name__ == "__main__":
    unittest.main()
