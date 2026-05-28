"""Unit tests for ``qa_quicklook.summary`` and ``qa_quicklook.download``.

Stdlib only.  The summary reader is exercised against the real run
directories present in ``Data/`` so we never have to keep a fixture
``.root`` in sync with the writer schema.  Tests that need uproot
short-circuit when uproot is not importable so they're skipped in
the headless CI before the venv is bootstrapped.

Run::

    .venv/bin/python -m unittest tests.test_qa_quicklook_summary
"""

from __future__ import annotations

import sys
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook.download import (  # noqa: E402
    DownloadConfig,
    build_argv,
    describe,
    load_config,
)


# ---------------------------------------------------------------------------
# Summary reader.
# ---------------------------------------------------------------------------


class TestSummaryReader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import uproot  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("uproot not installed in this Python")
        cls.data_root = REPO_ROOT / "Data"
        if not cls.data_root.is_dir():
            raise unittest.SkipTest("no Data/ directory in the repo")

    def test_missing_run_dir_reports_no_recodata(self):
        from qa_quicklook.summary import read_summary

        s = read_summary(self.data_root / "does-not-exist")
        self.assertFalse(s.has_recodata)
        self.assertIsNone(s.sigma_mm)
        self.assertIsNone(s.n_entries)
        self.assertIsNone(s.error)

    def test_real_run_with_full_summaries(self):
        """A run we know has the bin-labelled summary populated."""
        from qa_quicklook.summary import read_summary

        run = self.data_root / "20251111-164951"
        if not (run / "recodata.root").is_file():
            self.skipTest("known-good run not present on this checkout")
        s = read_summary(run)
        self.assertTrue(s.has_recodata)
        self.assertIsNone(s.error)
        self.assertIsNotNone(s.sigma_mm)
        # σ values in mm; the first ring should land in a sane physical
        # band — anything outside [0.1, 20] mm means we read garbage.
        self.assertGreater(s.sigma_mm, 0.1)
        self.assertLess(s.sigma_mm, 20.0)
        self.assertIn("first", s.sigma_by_bin)
        self.assertGreater(s.n_entries, 0)
        self.assertGreater(s.n_triggers, 0)

    def test_legacy_run_without_summary_histo(self):
        """Older recodata.root files lack ``Rings/h_peak_sigma_summary``.

        The reader must still report ``has_recodata=True`` and surface
        the TTree entry count; the sigma fields are simply ``None``.
        """
        from qa_quicklook.summary import read_summary

        run = self.data_root / "20251112-042329"
        if not (run / "recodata.root").is_file():
            self.skipTest("legacy run not present on this checkout")
        s = read_summary(run)
        self.assertTrue(s.has_recodata)
        self.assertIsNone(s.error)
        self.assertIsNone(s.sigma_mm)
        self.assertEqual(s.sigma_by_bin, {})
        self.assertIsNotNone(s.n_entries)


# ---------------------------------------------------------------------------
# Download config parsing + argv composition.
# ---------------------------------------------------------------------------


class TestDownloadConfig(unittest.TestCase):
    def _write_toml(self, body: str) -> Path:
        import tempfile

        tmp = Path(tempfile.mkdtemp(prefix="qaq-download-")) / "qa_quicklook.toml"
        tmp.write_text(textwrap.dedent(body))
        return tmp

    def test_missing_file_disables_feature(self):
        cfg = load_config(Path("/no/such/path.toml"))
        self.assertFalse(cfg.enabled)
        self.assertEqual(cfg.source, "")
        self.assertEqual(cfg.extra_flags, ())

    def test_empty_source_disables_feature(self):
        path = self._write_toml('''
            [rsync]
            source = ""
        ''')
        cfg = load_config(path)
        self.assertFalse(cfg.enabled)
        self.assertIn("disabled", describe(cfg).lower())

    def test_populated_source_enables_feature(self):
        path = self._write_toml('''
            [rsync]
            source = "user@daq:/data/2025/Data/"
            extra_flags = ["--dry-run", "--bwlimit=20M"]
        ''')
        cfg = load_config(path)
        self.assertTrue(cfg.enabled)
        self.assertEqual(cfg.source, "user@daq:/data/2025/Data/")
        self.assertEqual(cfg.extra_flags, ("--dry-run", "--bwlimit=20M"))

    def test_extra_flags_optional(self):
        path = self._write_toml('''
            [rsync]
            source = "user@daq:/data/Data/"
        ''')
        cfg = load_config(path)
        self.assertEqual(cfg.extra_flags, ())

    def test_build_argv_appends_trailing_slash_to_dest(self):
        cfg = DownloadConfig(source="user@host:/abs/Data/")
        argv = build_argv(cfg, Path("Data"))
        self.assertEqual(
            argv,
            ["rsync", "-av", "--info=progress2", "user@host:/abs/Data/", "Data/"],
        )

    def test_build_argv_keeps_existing_trailing_slash(self):
        cfg = DownloadConfig(source="user@host:/abs/Data/")
        argv = build_argv(cfg, Path("Data/"))
        # No double slash injected.
        self.assertEqual(argv[-1], "Data/")

    def test_build_argv_with_extra_flags(self):
        cfg = DownloadConfig(
            source="user@host:/abs/Data/",
            extra_flags=("--dry-run",),
        )
        argv = build_argv(cfg, Path("Data"))
        # extra flags appear before the positional source+dest pair.
        self.assertEqual(argv[-3], "--dry-run")
        self.assertEqual(argv[-2], "user@host:/abs/Data/")
        self.assertEqual(argv[-1], "Data/")

    def test_build_argv_raises_when_disabled(self):
        cfg = DownloadConfig(source="")
        with self.assertRaises(ValueError):
            build_argv(cfg, Path("Data"))


# ---------------------------------------------------------------------------
# Chain step argv assembly.
# ---------------------------------------------------------------------------


class TestChainSteps(unittest.TestCase):
    def test_chain_step_holds_argv_verbatim(self):
        from qa_quicklook.chain import ChainStep

        s = ChainStep(name="lightdata", argv=["/bin/foo", "a", "b"])
        self.assertEqual(s.name, "lightdata")
        self.assertEqual(s.argv, ["/bin/foo", "a", "b"])


if __name__ == "__main__":
    unittest.main()
