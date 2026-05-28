"""Unit tests for ``qa_quicklook.joblock``.

Stdlib only.  Runs against a temporary ``CACHE_DIR`` so the real
``~/.cache/qa_quicklook/jobs/`` is never touched.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook import joblock  # noqa: E402


class _TempCacheBase(unittest.TestCase):
    """Per-test temp cache dir; restore the module default afterward."""

    def setUp(self) -> None:
        self._original_cache = joblock.CACHE_DIR
        self._tmp = Path(tempfile.mkdtemp(prefix="qaq-joblock-"))
        joblock.CACHE_DIR = self._tmp

    def tearDown(self) -> None:
        joblock.CACHE_DIR = self._original_cache


class TestLockPath(_TempCacheBase):
    def test_path_lives_under_cache_dir(self):
        p = joblock.lock_path("lightdata", "20251111-164951")
        self.assertTrue(str(p).startswith(str(self._tmp)))
        self.assertTrue(p.name.startswith("lightdata_"))
        self.assertTrue(p.name.endswith(".json"))

    def test_path_sanitises_dangerous_chars(self):
        """A pathological writer / run name should not escape the cache dir."""
        p = joblock.lock_path("../etc", "../passwd")
        # No `..` or `/` survive in the leaf filename.
        self.assertNotIn("..", p.name)
        self.assertNotIn("/", p.name)
        # And the file would still live in our temp dir.
        self.assertTrue(str(p).startswith(str(self._tmp)))


class TestRoundTrip(_TempCacheBase):
    def _sample(self) -> joblock.JobLock:
        return joblock.JobLock(
            writer="recodata",
            run="20251111-164951",
            pid=os.getpid(),
            argv=["/bin/echo", "hi"],
            started_at="2025-01-02T03:04:05",
            state=joblock.EFFECTIVE_RUNNING,
        )

    def test_write_then_read_preserves_fields(self):
        lock = self._sample()
        joblock.write_lock(lock)
        loaded = joblock.read_lock(lock.writer, lock.run)
        self.assertEqual(loaded.writer, lock.writer)
        self.assertEqual(loaded.run, lock.run)
        self.assertEqual(loaded.pid, lock.pid)
        self.assertEqual(loaded.argv, lock.argv)
        self.assertEqual(loaded.state, joblock.EFFECTIVE_RUNNING)
        self.assertIsNone(loaded.exit_code)

    def test_update_lock_patches_fields_in_place(self):
        joblock.write_lock(self._sample())
        updated = joblock.update_lock(
            "recodata", "20251111-164951",
            state=joblock.EFFECTIVE_SUCCESS,
            exit_code=0,
            finished_at="2025-01-02T03:05:00",
        )
        self.assertEqual(updated.state, joblock.EFFECTIVE_SUCCESS)
        self.assertEqual(updated.exit_code, 0)
        # Pre-existing fields stay intact.
        self.assertEqual(updated.pid, os.getpid())
        self.assertEqual(updated.argv, ["/bin/echo", "hi"])

    def test_update_lock_returns_none_when_missing(self):
        self.assertIsNone(joblock.update_lock("recodata", "20251111-164951", state="x"))

    def test_read_lock_returns_none_when_file_missing(self):
        self.assertIsNone(joblock.read_lock("recodata", "20251111-164951"))

    def test_clear_lock_removes_file(self):
        joblock.write_lock(self._sample())
        joblock.clear_lock("recodata", "20251111-164951")
        self.assertIsNone(joblock.read_lock("recodata", "20251111-164951"))
        # Idempotent: clearing twice is fine.
        joblock.clear_lock("recodata", "20251111-164951")

    def test_list_locks_returns_all(self):
        joblock.write_lock(joblock.JobLock(
            writer="recodata", run="r1", pid=1, argv=[], started_at="t",
        ))
        joblock.write_lock(joblock.JobLock(
            writer="lightdata", run="r2", pid=2, argv=[], started_at="t",
        ))
        locks = joblock.list_locks()
        names = {(l.writer, l.run) for l in locks}
        self.assertEqual(names, {("recodata", "r1"), ("lightdata", "r2")})


class TestPidAlive(_TempCacheBase):
    def test_current_process_is_alive(self):
        self.assertTrue(joblock.pid_alive(os.getpid()))

    def test_pid_zero_is_not_alive(self):
        self.assertFalse(joblock.pid_alive(0))

    def test_negative_pid_is_not_alive(self):
        self.assertFalse(joblock.pid_alive(-1))


class TestEffectiveState(_TempCacheBase):
    def _lock(self, **overrides):
        defaults = dict(
            writer="recodata", run="r", pid=os.getpid(),
            argv=[], started_at="t",
            state=joblock.EFFECTIVE_RUNNING,
        )
        defaults.update(overrides)
        return joblock.JobLock(**defaults)

    def test_no_lock_is_idle(self):
        self.assertEqual(joblock.effective_state(None), joblock.EFFECTIVE_IDLE)

    def test_running_with_live_pid_is_running(self):
        lock = self._lock(pid=os.getpid(), state=joblock.EFFECTIVE_RUNNING)
        self.assertEqual(joblock.effective_state(lock), joblock.EFFECTIVE_RUNNING)

    def test_running_with_dead_pid_is_abandoned(self):
        # PID 1 might exist (init), but a very high PID is unlikely
        # to be alive.  Use one we know is gone — spawn and reap.
        import subprocess
        proc = subprocess.Popen(["true"])
        proc.wait()
        # proc.pid is now a dead PID; macOS reuses PIDs but not
        # immediately.
        lock = self._lock(pid=proc.pid, state=joblock.EFFECTIVE_RUNNING)
        self.assertEqual(
            joblock.effective_state(lock), joblock.EFFECTIVE_ABANDONED
        )

    def test_terminal_states_pass_through(self):
        for s in (joblock.EFFECTIVE_SUCCESS, joblock.EFFECTIVE_ERROR, joblock.EFFECTIVE_KILLED):
            lock = self._lock(state=s, exit_code=0, finished_at="t")
            self.assertEqual(joblock.effective_state(lock), s)


if __name__ == "__main__":
    unittest.main()
