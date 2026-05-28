"""Unit tests for ``qa_quicklook.download``.

Pure-Python — no Qt, no real rsync.  Exercises ``load_config`` against
synthetic ``qa_quicklook.toml`` files and ``build_argv`` for every
configured / not-configured / bad-input combination.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook import download  # noqa: E402


class LoadConfigTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = Path(tempfile.mkdtemp(prefix="qaq-dl-"))
        self.cfg_path = self._tmp / "qa_quicklook.toml"

    def test_missing_file_returns_unconfigured_default(self) -> None:
        # No file at all → default RsyncConfig with is_configured = False.
        cfg = download.load_config(self.cfg_path)
        self.assertFalse(cfg.is_configured)
        self.assertEqual(cfg.local_data_dir, "Data")
        self.assertEqual(cfg.extra_args, "-av")

    def test_section_missing_returns_default(self) -> None:
        # File exists but no [rsync] table.
        self.cfg_path.write_text("[ui]\ntheme = \"system\"\n")
        cfg = download.load_config(self.cfg_path)
        self.assertFalse(cfg.is_configured)

    def test_fully_configured(self) -> None:
        self.cfg_path.write_text(
            "[rsync]\n"
            "remote_host = \"daq.local\"\n"
            "remote_data_dir = \"/srv/data\"\n"
            "local_data_dir = \"Data\"\n"
            "extra_args = \"-avz --partial\"\n"
        )
        cfg = download.load_config(self.cfg_path)
        self.assertTrue(cfg.is_configured)
        self.assertEqual(cfg.remote_host, "daq.local")
        self.assertEqual(cfg.remote_data_dir, "/srv/data")
        self.assertEqual(cfg.extra_args, "-avz --partial")

    def test_blank_remote_host_is_not_configured(self) -> None:
        # The "" sentinel disables the Download button — make sure
        # whitespace-only stays "not configured" too so an operator
        # who fat-fingered a space doesn't silently break.
        self.cfg_path.write_text(
            "[rsync]\n"
            "remote_host = \"   \"\n"
            "remote_data_dir = \"/srv/data\"\n"
        )
        cfg = download.load_config(self.cfg_path)
        self.assertFalse(cfg.is_configured)

    def test_partial_config_is_not_configured(self) -> None:
        # Host set but no remote_data_dir — incomplete = disabled.
        self.cfg_path.write_text(
            "[rsync]\nremote_host = \"daq.local\"\n"
        )
        cfg = download.load_config(self.cfg_path)
        self.assertFalse(cfg.is_configured)


class BuildArgvTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path("/tmp/repo")

    def test_typical_case(self) -> None:
        cfg = download.RsyncConfig(
            remote_host="daq.local",
            remote_data_dir="/srv/data",
            local_data_dir="Data",
            extra_args="-av",
        )
        argv = download.build_argv(cfg, "20250528-141500", self.repo_root)
        self.assertEqual(argv[0], "rsync")
        # Flags come before paths.
        self.assertIn("-av", argv)
        # Remote endpoint: host:dir/run_id (no trailing slash).
        self.assertIn("daq.local:/srv/data/20250528-141500", argv)
        # Destination is the project's local Data/ with a trailing
        # slash so rsync drops the run-id subdir under it.
        dest = str(self.repo_root / "Data") + "/"
        self.assertIn(dest, argv)

    def test_strips_trailing_slash_on_remote_dir(self) -> None:
        # Operator-friendliness: a trailing slash on remote_data_dir
        # shouldn't double up to "//".
        cfg = download.RsyncConfig(
            remote_host="daq.local",
            remote_data_dir="/srv/data/",
        )
        argv = download.build_argv(cfg, "RUN-1", self.repo_root)
        joined = " ".join(argv)
        self.assertNotIn("//", joined)
        self.assertIn("daq.local:/srv/data/RUN-1", joined)

    def test_extra_args_splits_like_shell(self) -> None:
        # Multi-word extra_args should be tokenised, not passed as one.
        cfg = download.RsyncConfig(
            remote_host="daq.local",
            remote_data_dir="/srv/data",
            extra_args="-av --bwlimit=2000 --partial",
        )
        argv = download.build_argv(cfg, "RUN-1", self.repo_root)
        self.assertIn("--bwlimit=2000", argv)
        self.assertIn("--partial", argv)

    def test_absolute_local_data_dir_is_honoured(self) -> None:
        cfg = download.RsyncConfig(
            remote_host="daq.local",
            remote_data_dir="/srv/data",
            local_data_dir="/var/dRICH/Data",
        )
        argv = download.build_argv(cfg, "RUN-1", self.repo_root)
        self.assertIn("/var/dRICH/Data/", argv)

    def test_unconfigured_raises(self) -> None:
        cfg = download.RsyncConfig()  # nothing set
        with self.assertRaises(ValueError):
            download.build_argv(cfg, "RUN-1", self.repo_root)

    def test_empty_run_id_raises(self) -> None:
        cfg = download.RsyncConfig(
            remote_host="daq.local",
            remote_data_dir="/srv/data",
        )
        with self.assertRaises(ValueError):
            download.build_argv(cfg, "   ", self.repo_root)


class ExpectedLocalPathTests(unittest.TestCase):
    def test_relative_local_dir_anchors_to_repo(self) -> None:
        cfg = download.RsyncConfig(local_data_dir="Data")
        p = download.expected_local_path(cfg, "RUN-1", Path("/tmp/repo"))
        self.assertEqual(p, Path("/tmp/repo/Data/RUN-1"))

    def test_absolute_local_dir_wins(self) -> None:
        cfg = download.RsyncConfig(local_data_dir="/var/data")
        p = download.expected_local_path(cfg, "RUN-1", Path("/tmp/repo"))
        self.assertEqual(p, Path("/var/data/RUN-1"))


if __name__ == "__main__":
    unittest.main()
