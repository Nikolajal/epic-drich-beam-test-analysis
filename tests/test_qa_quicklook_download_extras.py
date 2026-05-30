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


class TestParseListing(unittest.TestCase):
    """``_parse_listing`` is the only side-effect-free piece of the
    remote-listing path — exercise its tolerance to the realistic
    flavours of remote output (trailing newline, blank lines, missing
    mtime, error sentinel, junk that doesn't match a run-id shape)."""

    def test_happy_path(self) -> None:
        out = (
            "20251111-105543\t1736000000\n"
            "20251111-110914\t1736001000\n"
            "20251111-111835\t1736002000\n"
        )
        pairs = download._parse_listing(out)
        self.assertEqual(len(pairs), 3)
        self.assertEqual(pairs[0], ("20251111-105543", 1736000000))

    def test_dash_mtime_becomes_none(self) -> None:
        #  ``stat`` falling through on the remote prints "-" for the
        #  mtime token — we map that to ``None`` so the iso render
        #  shows "—" rather than a 1970 epoch.
        pairs = download._parse_listing("20251111-105543\t-\n")
        self.assertEqual(pairs, [("20251111-105543", None)])

    def test_error_sentinel_is_skipped(self) -> None:
        out = "__ERR__:cd_failed\n20251111-105543\t1736000000\n"
        pairs = download._parse_listing(out)
        #  The sentinel itself doesn't make it into the parsed list —
        #  ``list_remote_runs`` is responsible for spotting it in the
        #  raw stdout and converting to an error message.
        self.assertEqual(pairs, [("20251111-105543", 1736000000)])

    def test_junk_lines_are_skipped(self) -> None:
        out = (
            "\n"
            "not-a-run-id\t0\n"
            "20251111-105543\t1736000000\n"
            "another_dir\n"
        )
        pairs = download._parse_listing(out)
        self.assertEqual(pairs, [("20251111-105543", 1736000000)])

    def test_missing_mtime_column(self) -> None:
        #  Defensive: a row with just an id (no tab) shouldn't crash —
        #  the script always emits both columns, but a corrupted
        #  stream could in theory drop a tab.
        pairs = download._parse_listing("20251111-105543\n")
        self.assertEqual(pairs, [("20251111-105543", None)])


class TestListRemoteRuns(unittest.TestCase):
    """Drive ``list_remote_runs`` against a mocked ssh subprocess.

    The function builds a single argv calling ``ssh ... <script>`` and
    parses the resulting stdout, so the mock controls the whole
    surface area we care about."""

    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self.repo_root = Path(self._td.name)
        (self.repo_root / "Data").mkdir()
        self.cfg = download.RsyncConfig(
            remote_host="daq.local",
            remote_data_dir="/srv/data",
            local_data_dir="Data",
        )

    def tearDown(self) -> None:
        self._td.cleanup()

    def test_unconfigured_returns_clear_error(self) -> None:
        entries, err = download.list_remote_runs(
            download.RsyncConfig(), self.repo_root,
        )
        self.assertEqual(entries, [])
        self.assertIn("rsync remote is not configured", err)

    def test_happy_path_sorts_newest_first(self) -> None:
        stdout = (
            "20251111-105543\t1736000000\n"
            "20251112-141500\t1736100000\n"
            "20251111-110914\t1736001000\n"
        )
        fake = mock.Mock(returncode=0, stdout=stdout, stderr="")
        with mock.patch.object(subprocess, "run", return_value=fake):
            entries, err = download.list_remote_runs(self.cfg, self.repo_root)
        self.assertEqual(err, "")
        self.assertEqual([e.run_id for e in entries],
                         ["20251112-141500", "20251111-110914", "20251111-105543"])
        #  Every entry got an iso render derived from the epoch.
        for e in entries:
            self.assertIsNotNone(e.mtime_iso)

    def test_local_present_flag_set_for_mirrored_runs(self) -> None:
        #  Pre-populate Data/ with one of the run ids the remote
        #  reports — that row should come back tagged.
        (self.repo_root / "Data" / "20251111-105543").mkdir()
        stdout = (
            "20251111-105543\t1736000000\n"
            "20251111-110914\t1736001000\n"
        )
        fake = mock.Mock(returncode=0, stdout=stdout, stderr="")
        with mock.patch.object(subprocess, "run", return_value=fake):
            entries, err = download.list_remote_runs(self.cfg, self.repo_root)
        self.assertEqual(err, "")
        by_id = {e.run_id: e for e in entries}
        self.assertTrue(by_id["20251111-105543"].local_present)
        self.assertFalse(by_id["20251111-110914"].local_present)

    def test_cd_failed_sentinel_returns_pointed_error(self) -> None:
        #  Remote script printed the sentinel — exit code is 2 from
        #  the script's own ``exit 2``, but ``list_remote_runs``
        #  spots the stdout token first and produces a far more
        #  helpful message than "ssh exited 2".
        fake = mock.Mock(returncode=2, stdout="__ERR__:cd_failed\n", stderr="")
        with mock.patch.object(subprocess, "run", return_value=fake):
            entries, err = download.list_remote_runs(self.cfg, self.repo_root)
        self.assertEqual(entries, [])
        self.assertIn("does not exist", err)
        self.assertIn("/srv/data", err)

    def test_ssh_nonzero_surfaces_stderr(self) -> None:
        fake = mock.Mock(
            returncode=255, stdout="",
            stderr="ssh: Could not resolve hostname daq.local\nfoo\n",
        )
        with mock.patch.object(subprocess, "run", return_value=fake):
            entries, err = download.list_remote_runs(self.cfg, self.repo_root)
        self.assertEqual(entries, [])
        self.assertIn("Could not resolve hostname", err)

    def test_timeout_returns_clean_error(self) -> None:
        def _raise(*_a, **_kw):
            raise subprocess.TimeoutExpired(cmd="ssh", timeout=20)
        with mock.patch.object(subprocess, "run", side_effect=_raise):
            entries, err = download.list_remote_runs(
                self.cfg, self.repo_root, timeout_s=20,
            )
        self.assertEqual(entries, [])
        self.assertIn("timed out", err)

    def test_missing_ssh_binary(self) -> None:
        with mock.patch.object(
            subprocess, "run",
            side_effect=FileNotFoundError("no ssh"),
        ):
            entries, err = download.list_remote_runs(self.cfg, self.repo_root)
        self.assertEqual(entries, [])
        self.assertIn("ssh binary not found", err)

    def test_empty_listing_is_success_not_error(self) -> None:
        #  Zero runs on the remote is a valid state, not an error.
        fake = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(subprocess, "run", return_value=fake):
            entries, err = download.list_remote_runs(self.cfg, self.repo_root)
        self.assertEqual(entries, [])
        self.assertEqual(err, "")

    def test_remote_dir_with_special_chars_is_shell_quoted(self) -> None:
        #  A pathological [rsync].remote_data_dir with spaces or
        #  shell metacharacters shouldn't tear the script open —
        #  ``list_remote_runs`` uses ``shlex.quote`` and the captured
        #  argv must contain the quoted form, not the raw value.
        cfg = download.RsyncConfig(
            remote_host="h",
            remote_data_dir="/srv/data dir/2026",
            local_data_dir="Data",
        )
        captured = {}
        def _capture(argv, **kw):
            captured["argv"] = argv
            return mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(subprocess, "run", side_effect=_capture):
            download.list_remote_runs(cfg, self.repo_root)
        #  The script is the last positional arg to ssh.  shlex.quote
        #  wraps a path with a space in single-quotes.
        script = captured["argv"][-1]
        self.assertIn("'/srv/data dir/2026'", script)


if __name__ == "__main__":
    unittest.main()
