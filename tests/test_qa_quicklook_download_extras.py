"""Unit tests for ``qa_quicklook.download`` helpers added after the
initial v1 (``save_address``, ``probe_ssh_keyauth``).

Covers:
  - ``save_address`` round-trip: persists ``remote_host`` + ``remote_data_dir``
    via tomlkit, preserves comments and unrelated tables.
  - ``save_address`` against a missing file: bootstraps with just
    ``[rsync]`` + the two new keys.
  - ``probe_ssh_keyauth`` with empty host (fast-fail).
  - ``probe_ssh_keyauth`` against a mocked ssh subprocess
    (success / non-zero / timeout / FileNotFoundError).

Run::

    .venv/bin/python -m unittest tests.test_qa_quicklook_download_extras
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook import download  # noqa: E402


class TestSaveAddress(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self._cfg = Path(self._td.name) / "qa_quicklook.toml"

    def tearDown(self) -> None:
        self._td.cleanup()

    def test_round_trip_preserves_other_sections(self) -> None:
        self._cfg.write_text(
            '# header comment\n'
            '\n'
            '[ui]\n'
            'theme = "dark"\n'
            '\n'
            '[rsync]\n'
            'remote_host = ""\n'
            'remote_data_dir = ""\n'
            'local_data_dir = "Data"\n'
            '\n'
            '[detector]\n'
            'foo = 1\n'
        )
        download.save_address(
            self._cfg, remote_host="h.example", remote_data_dir="/data/x",
        )
        cfg = download.load_config(self._cfg)
        self.assertEqual(cfg.remote_host, "h.example")
        self.assertEqual(cfg.remote_data_dir, "/data/x")
        #  local_data_dir preserved through tomlkit reference semantics.
        self.assertEqual(cfg.local_data_dir, "Data")
        body = self._cfg.read_text()
        self.assertIn("# header comment", body)
        self.assertIn("[ui]", body)
        self.assertIn("[detector]", body)

    def test_creates_fresh_file_when_missing(self) -> None:
        self.assertFalse(self._cfg.exists())
        download.save_address(
            self._cfg, remote_host="fresh", remote_data_dir="/y",
        )
        self.assertTrue(self._cfg.exists())
        cfg = download.load_config(self._cfg)
        self.assertEqual(cfg.remote_host, "fresh")
        self.assertEqual(cfg.remote_data_dir, "/y")


class TestProbeSshKeyauth(unittest.TestCase):
    def test_empty_host_short_circuits(self) -> None:
        ok, msg = download.probe_ssh_keyauth("")
        self.assertFalse(ok)
        self.assertIn("no remote_host", msg)

    def test_zero_return_is_success(self) -> None:
        fake = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(subprocess, "run", return_value=fake):
            ok, msg = download.probe_ssh_keyauth("user@host", timeout_s=1)
        self.assertTrue(ok)
        self.assertIn("ok", msg)

    def test_nonzero_surfaces_first_stderr_line(self) -> None:
        fake = mock.Mock(
            returncode=255,
            stdout="",
            stderr="Permission denied (publickey).\nfoo bar baz\n",
        )
        with mock.patch.object(subprocess, "run", return_value=fake):
            ok, msg = download.probe_ssh_keyauth("user@host", timeout_s=1)
        self.assertFalse(ok)
        self.assertIn("Permission denied", msg)

    def test_timeout_surfaces_human_readable_message(self) -> None:
        def _raise_timeout(*_args, **_kw):
            raise subprocess.TimeoutExpired(cmd="ssh", timeout=1)
        with mock.patch.object(subprocess, "run", side_effect=_raise_timeout):
            ok, msg = download.probe_ssh_keyauth("user@host", timeout_s=1)
        self.assertFalse(ok)
        self.assertIn("timed out", msg)

    def test_missing_ssh_binary(self) -> None:
        with mock.patch.object(
            subprocess, "run",
            side_effect=FileNotFoundError("no ssh on PATH"),
        ):
            ok, msg = download.probe_ssh_keyauth("user@host", timeout_s=1)
        self.assertFalse(ok)
        self.assertIn("ssh binary not found", msg)


if __name__ == "__main__":
    unittest.main()
