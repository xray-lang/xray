#!/usr/bin/env python3
"""Process group cleanup of the shared child runner."""

import os
from pathlib import Path
import signal
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import xray_process


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
