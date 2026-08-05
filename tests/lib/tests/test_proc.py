"""proc tests: bytes by default, timeouts return rather than raise, check=True
raises with full context. Uses the test's own interpreter as the child.
"""

import sys
import unittest

from _support import bootstrap_xraytest

bootstrap_xraytest()
from xraytest import proc  # noqa: E402

PY = sys.executable


class RunTest(unittest.TestCase):
    def test_captures_stdout_as_bytes(self):
        r = proc.run([PY, "-c", "import sys; sys.stdout.write('hi')"])
        self.assertTrue(r.ok)
        self.assertEqual(r.stdout, b"hi")
        self.assertIsInstance(r.stdout, bytes)

    def test_nonzero_exit_is_not_ok_but_does_not_raise(self):
        r = proc.run([PY, "-c", "import sys; sys.exit(3)"])
        self.assertFalse(r.ok)
        self.assertEqual(r.returncode, 3)

    def test_stdin_is_passed_through(self):
        r = proc.run(
            [PY, "-c", "import sys; sys.stdout.write(sys.stdin.read())"],
            stdin=b"echoed",
        )
        self.assertEqual(r.stdout, b"echoed")

    def test_timeout_returns_timed_out_not_exception(self):
        r = proc.run([PY, "-c", "import time; time.sleep(10)"], timeout=0.5)
        self.assertTrue(r.timed_out)
        self.assertFalse(r.ok)

    def test_check_raises_command_error_on_failure(self):
        with self.assertRaises(proc.CommandError):
            proc.run([PY, "-c", "import sys; sys.exit(1)"], check=True)

    def test_check_passes_through_on_success(self):
        r = proc.run([PY, "-c", "pass"], check=True)
        self.assertTrue(r.ok)

    def test_decode_helpers_only_when_asked(self):
        r = proc.run([PY, "-c", "import sys; sys.stdout.write('text')"])
        self.assertEqual(r.stdout_text(), "text")


if __name__ == "__main__":
    unittest.main()


class MissingExecutableTest(unittest.TestCase):
    """A missing tool is a red verdict, not a traceback.

    Runners call proc.run for tools that a partial build may not have produced.
    Raising there aborts the runner mid-report; the shell predecessors printed
    one line and carried a 127, so that is what callers are written against.
    """

    def test_missing_binary_reports_127(self):
        result = proc.run(["/nonexistent/xray_tool_that_is_absent"])
        self.assertEqual(result.returncode, 127)
        self.assertFalse(result.ok)
        self.assertIn("xray_tool_that_is_absent", result.stderr.decode())
        self.assertFalse(result.timed_out)

    def test_missing_binary_with_check_raises_command_error(self):
        with self.assertRaises(proc.CommandError):
            proc.run(["/nonexistent/xray_tool_that_is_absent"], check=True)
