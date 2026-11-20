#!/usr/bin/env python3
"""Process group cleanup of the shared child runner."""

from contextlib import contextmanager
import os
from pathlib import Path
import signal
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import xray_process


class ProcessSpawnGuardTest(unittest.TestCase):
    def test_denied_guard_does_not_spawn(self):
        class Cancelled(Exception):
            pass

        guard = mock.MagicMock()
        guard.__enter__.side_effect = Cancelled
        with mock.patch.object(xray_process.subprocess, "Popen") as spawn:
            with self.assertRaises(Cancelled):
                xray_process.run_child([sys.executable, "-c", "pass"], spawn_guard=lambda: guard)
            spawn.assert_not_called()

    def test_guard_covers_launch_and_releases_before_wait(self):
        guarded = False

        @contextmanager
        def guard():
            nonlocal guarded
            guarded = True
            try:
                yield
            finally:
                guarded = False

        popen = xray_process.subprocess.Popen

        def launch(*args, **kwargs):
            self.assertTrue(guarded)
            child = popen(*args, **kwargs)
            wait = child.wait

            def wait_outside_guard(*args, **kwargs):
                self.assertFalse(guarded)
                return wait(*args, **kwargs)

            child.wait = wait_outside_guard
            return child

        with mock.patch.object(xray_process.subprocess, "Popen", side_effect=launch):
            code = xray_process.run_child([sys.executable, "-c", "raise SystemExit(7)"],
                                          timeout=5, spawn_guard=guard)
        self.assertEqual(code, 7)

    def test_guard_exit_error_cleans_up_the_created_child(self):
        @contextmanager
        def broken_guard():
            yield
            raise ValueError("guard release failed")

        popen = xray_process.subprocess.Popen
        started = []

        def launch(*args, **kwargs):
            child = popen(*args, **kwargs)
            started.append(child)
            return child

        with mock.patch.object(xray_process.subprocess, "Popen", side_effect=launch):
            with self.assertRaisesRegex(ValueError, "guard release failed"):
                xray_process.run_child([sys.executable, "-c", "import time; time.sleep(60)"],
                                       timeout=5, spawn_guard=broken_guard)
        self.assertEqual(len(started), 1)
        self.assertIsNotNone(started[0].poll())


@unittest.skipIf(os.name == "nt", "POSIX process groups only")
class ProcessGroupCleanupTest(unittest.TestCase):
    def test_exiting_group_is_retried_until_it_disappears(self):
        # Darwin reports EPERM while unreaped members of the group exit.
        answers = [PermissionError(), PermissionError(), ProcessLookupError()]
        with mock.patch.object(xray_process.os, "killpg", side_effect=answers) as kill, \
                mock.patch.object(xray_process.time, "sleep"):
            xray_process._kill_group(1234)
        self.assertEqual(kill.call_count, 3)
        kill.assert_called_with(1234, signal.SIGKILL)

    def test_group_that_keeps_refusing_is_reported(self):
        clock = iter([0.0] + [float(xray_process.GROUP_EXIT_SECONDS) + 1.0] * 4)
        with mock.patch.object(xray_process.os, "killpg", side_effect=PermissionError()), \
                mock.patch.object(xray_process.time, "monotonic", side_effect=lambda: next(clock)), \
                mock.patch.object(xray_process.time, "sleep"):
            with self.assertRaises(xray_process.RecoveryRequired):
                xray_process._kill_group(1234)

    def test_delivered_signal_ends_cleanup(self):
        with mock.patch.object(xray_process.os, "killpg") as kill:
            xray_process._kill_group(1234)
        kill.assert_called_once_with(1234, signal.SIGKILL)

    def test_real_command_exit_status_is_returned(self):
        self.assertEqual(xray_process.run_child([sys.executable, "-c", "raise SystemExit(3)"]), 3)


if __name__ == "__main__":
    unittest.main()
