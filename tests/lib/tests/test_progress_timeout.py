"""Progress reporting and per-case timeout behavior.

The two capabilities added because a shell runner cannot: a live counter that
stays silent under CI, and a per-subprocess timeout that turns a deadlock into
one red result instead of a hung lane.
"""

import io
import sys
import time
import unittest

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import platform, proc, progress, scheduler  # noqa: E402


class _TtyBuffer(io.StringIO):
    """A buffer that claims to be a tty, so auto-detection turns output on."""

    def isatty(self) -> bool:
        return True


class ProgressTest(unittest.TestCase):
    def test_silent_when_not_a_tty(self):
        # A StringIO is not a tty, so nothing is written -- the CI/ctest case.
        buf = io.StringIO()
        rep = progress.ProgressReporter(3, stream=buf)
        rep.tick("a")
        rep.tick("b")
        rep.finish()
        self.assertEqual(buf.getvalue(), "")

    def test_emits_when_enabled(self):
        buf = io.StringIO()
        rep = progress.ProgressReporter(2, stream=buf, enabled=True)
        rep.tick("case-one")
        out = buf.getvalue()
        self.assertIn("[1/2]", out)
        self.assertIn("case-one", out)

    def test_finish_clears_line(self):
        buf = io.StringIO()
        rep = progress.ProgressReporter(1, stream=buf, enabled=True)
        rep.tick("x")
        rep.finish()
        # Ends with a carriage return + blanking, so the summary starts clean.
        self.assertTrue(buf.getvalue().endswith("\r"))

    def test_erases_leftover_when_line_shrinks(self):
        # A long label followed by a short one must blank the tail, or the
        # previous case's name stays visible after the shorter line.
        buf = io.StringIO()
        rep = progress.ProgressReporter(2, stream=buf, enabled=True)
        rep.tick("a_very_long_case_name.xr")
        rep.tick("short.xr")
        self.assertIn("short.xr    ", buf.getvalue())

    def test_zero_total_does_not_crash(self):
        # A mode that collects no cases still constructs and finishes cleanly.
        buf = io.StringIO()
        rep = progress.ProgressReporter(0, stream=buf, enabled=True)
        rep.finish()

    def test_tick_past_total_is_tolerated(self):
        # Defensive: an extra completion must not raise mid-suite.
        buf = io.StringIO()
        rep = progress.ProgressReporter(1, stream=buf, enabled=True)
        rep.tick("a")
        rep.tick("b")
        self.assertIn("[2/1]", buf.getvalue())

    def test_env_mute_overrides_tty(self):
        import os

        os.environ["XRAY_TEST_NO_PROGRESS"] = "1"
        try:
            buf = _TtyBuffer()
            rep = progress.ProgressReporter(2, stream=buf)
            rep.tick("x")
            rep.finish()
            self.assertEqual(buf.getvalue(), "")
        finally:
            os.environ.pop("XRAY_TEST_NO_PROGRESS", None)

    def test_counts_completions_under_parallel_scheduler(self):
        # on_done fires once per task; the counter must reach exactly the total.
        buf = io.StringIO()
        rep = progress.ProgressReporter(10, stream=buf, enabled=True)
        sched = scheduler.Scheduler(scheduler.default_limits(4))
        tasks = [scheduler.Task(key=str(i), fn=(lambda n=i: n)) for i in range(10)]
        sched.run(tasks, on_done=lambda k, r: rep.tick(f"case{r}"))
        self.assertIn("[10/10]", buf.getvalue())


class EnvTimeoutTest(unittest.TestCase):
    def tearDown(self):
        import os

        os.environ.pop("XT_TO", None)

    def test_unset_uses_default(self):
        self.assertEqual(platform.env_timeout("XT_TO", 120), 120.0)

    def test_zero_disables(self):
        import os

        os.environ["XT_TO"] = "0"
        self.assertIsNone(platform.env_timeout("XT_TO", 120))

    def test_positive_overrides(self):
        import os

        os.environ["XT_TO"] = "45"
        self.assertEqual(platform.env_timeout("XT_TO", 120), 45.0)

    def test_garbage_keeps_default(self):
        import os

        os.environ["XT_TO"] = "notanumber"
        self.assertEqual(platform.env_timeout("XT_TO", 120), 120.0)


class ProcTimeoutTest(unittest.TestCase):
    def test_timeout_kills_and_flags(self):
        # A child that would sleep well past the timeout must be terminated and
        # reported as timed_out, not left to hang the caller.
        start = time.monotonic()
        r = proc.run([sys.executable, "-c", "import time; time.sleep(30)"], timeout=0.5)
        elapsed = time.monotonic() - start
        self.assertTrue(r.timed_out)
        self.assertFalse(r.ok)
        self.assertLess(elapsed, 10)  # returned promptly, did not wait 30s

    def test_timeout_kills_child_process_tree(self):
        # The child spawns a grandchild that sleeps; killpg must reach it, so
        # the call returns promptly rather than blocking on the grandchild.
        code = (
            "import subprocess, sys, time; "
            "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); "
            "time.sleep(30)"
        )
        start = time.monotonic()
        r = proc.run([sys.executable, "-c", code], timeout=0.5)
        elapsed = time.monotonic() - start
        self.assertTrue(r.timed_out)
        self.assertLess(elapsed, 10)


if __name__ == "__main__":
    unittest.main()
