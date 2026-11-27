#!/usr/bin/env python3
"""Process group cleanup of the shared child runner."""

from contextlib import contextmanager
import os
from pathlib import Path
import signal
import sys
import tempfile
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
            self.assertFalse(xray_process._kill_group(1234))
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
            self.assertTrue(xray_process._kill_group(1234))
        kill.assert_called_once_with(1234, signal.SIGKILL)

    def test_darwin_proves_drain_after_reaping_a_signalled_group(self):
        child = mock.Mock(pid=1234)
        child.wait.return_value = 3

        def signal_group(group, sig):
            self.assertEqual(group, child.pid)
            if sig == signal.SIGKILL:
                child.wait.assert_called_once_with(timeout=2)
                return
            self.assertEqual(sig, 0)
            self.assertEqual(child.wait.call_args_list, [mock.call(timeout=2), mock.call()])
            if kill.call_count == 2:
                raise PermissionError()
            raise ProcessLookupError()

        with mock.patch.object(xray_process.sys, "platform", "darwin"), \
                mock.patch.object(xray_process.subprocess, "Popen", return_value=child), \
                mock.patch.object(xray_process.os, "killpg", side_effect=signal_group) as kill, \
                mock.patch.object(xray_process.time, "sleep"):
            self.assertEqual(xray_process.run_child(["fixture"], timeout=2), 3)
        self.assertEqual(kill.call_args_list, [mock.call(child.pid, signal.SIGKILL),
                                              mock.call(child.pid, 0), mock.call(child.pid, 0)])

    def test_darwin_already_gone_group_skips_the_probe(self):
        child = mock.Mock(pid=1234)
        child.wait.return_value = 3
        with mock.patch.object(xray_process.sys, "platform", "darwin"), \
                mock.patch.object(xray_process.subprocess, "Popen", return_value=child), \
                mock.patch.object(xray_process.os, "killpg", side_effect=ProcessLookupError()) as kill:
            self.assertEqual(xray_process.run_child(["fixture"]), 3)
        kill.assert_called_once_with(child.pid, signal.SIGKILL)

    def test_darwin_unreaped_leader_is_retired_before_verifying_group_exit(self):
        signals = [PermissionError(), ProcessLookupError()]
        with mock.patch.object(xray_process.sys, "platform", "darwin"), \
                mock.patch.object(xray_process.os, "killpg", side_effect=signals) as kill:
            with xray_process.terminate_posix_group(1234, leader_unreaped=True):
                kill.assert_called_once_with(1234, signal.SIGKILL)
        self.assertEqual(kill.call_args_list, [mock.call(1234, signal.SIGKILL), mock.call(1234, 0)])

    def test_darwin_uncertain_drain_fails_closed_at_the_shared_deadline(self):
        for response in (None, PermissionError(), OSError("group query failed")):
            with self.subTest(response=response):
                child = mock.Mock(pid=1234)
                child.wait.side_effect = [xray_process.subprocess.TimeoutExpired("fixture", 2), -signal.SIGKILL]
                with mock.patch.object(xray_process.sys, "platform", "darwin"), \
                        mock.patch.object(xray_process.subprocess, "Popen", return_value=child), \
                        mock.patch.object(xray_process.os, "killpg", side_effect=[None, response]) as kill, \
                        mock.patch.object(xray_process.time, "monotonic",
                                          side_effect=[0.0, xray_process.GROUP_EXIT_SECONDS]), \
                        mock.patch.object(xray_process.time, "sleep") as sleep:
                    with self.assertRaises(xray_process.RecoveryRequired):
                        xray_process.run_child(["fixture"], timeout=2)
                self.assertEqual(kill.call_args_list, [mock.call(child.pid, signal.SIGKILL), mock.call(child.pid, 0)])
                sleep.assert_not_called()

    def test_linux_waits_for_live_members_after_reaping_the_leader(self):
        child = mock.Mock(pid=1234)
        child.wait.return_value = 3

        def members(group):
            self.assertEqual(group, child.pid)
            self.assertEqual(child.wait.call_args_list, [mock.call(timeout=2), mock.call()])
            return probe.call_count == 1

        with mock.patch.object(xray_process.sys, "platform", "linux"), \
                mock.patch.object(xray_process.subprocess, "Popen", return_value=child), \
                mock.patch.object(xray_process.os, "killpg") as kill, \
                mock.patch.object(xray_process, "_linux_group_has_live_member", side_effect=members) as probe, \
                mock.patch.object(xray_process.time, "sleep") as sleep:
            self.assertEqual(xray_process.run_child(["fixture"], timeout=2), 3)
        self.assertEqual(kill.call_args_list, [mock.call(child.pid, signal.SIGKILL),
                                              mock.call(child.pid, 0), mock.call(child.pid, 0)])
        sleep.assert_called_once()

    def test_linux_distinguishes_zombie_and_running_group_members(self):
        with tempfile.TemporaryDirectory() as directory:
            entry = Path(directory) / "1235"
            entry.mkdir()
            stat = entry / "stat"
            scandir = os.scandir
            with mock.patch.object(xray_process.os, "scandir", side_effect=lambda _: scandir(directory)), \
                    mock.patch.object(xray_process.os, "getpgid", return_value=1234), \
                    mock.patch.object(xray_process.os, "killpg") as kill:
                stat.write_bytes(b"1235 (fixture (child)) Z 1 1234 1234")
                self.assertFalse(xray_process._linux_group_has_live_member(1234))
                stat.write_bytes(b"1235 (fixture (child)) R 1 1234 1234")
                self.assertTrue(xray_process._linux_group_has_live_member(1234))
                kill.assert_not_called()

    def test_linux_cannot_confirm_drain_from_an_empty_process_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            scandir = os.scandir
            with mock.patch.object(xray_process.sys, "platform", "linux"), \
                    mock.patch.object(xray_process.os, "scandir", side_effect=lambda _: scandir(directory)), \
                    mock.patch.object(xray_process.os, "killpg") as kill:
                with self.assertRaises(xray_process.RecoveryRequired):
                    xray_process._wait_posix_group(1234, xray_process.time.monotonic())
                self.assertEqual(kill.call_args_list, [mock.call(1234, 0), mock.call(1234, 0)])

    def test_real_command_exit_status_is_returned(self):
        self.assertEqual(xray_process.run_child([sys.executable, "-c", "raise SystemExit(3)"]), 3)


if __name__ == "__main__":
    unittest.main()
