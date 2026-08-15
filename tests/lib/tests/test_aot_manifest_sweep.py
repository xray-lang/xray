"""Unit tests for the AOT manifest sweep's measurement preconditions."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from _support import load_module

_AOT_DIR = Path(__file__).resolve().parents[2] / "aot"
runner = load_module(
    "run_aot_manifest_sweep_tests_under_test",
    _AOT_DIR / "run_aot_manifest_sweep_tests.py",
)


class EntryIdentityTest(unittest.TestCase):
    def test_missing_xray_binary_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_manifest_sweep.") as tmp:
            missing = Path(tmp) / "missing-xray"
            stderr = io.StringIO()

            with contextlib.redirect_stderr(stderr):
                status = runner.main(["--xray", str(missing)])

            self.assertEqual(status, 1)
            self.assertIn("FAIL: xray binary not executable", stderr.getvalue())

    def test_zero_governed_cases_fail_closed(self):
        stderr = io.StringIO()

        with mock.patch.object(runner, "collect_cases", return_value=[]):
            with contextlib.redirect_stderr(stderr):
                status = runner.main(["--xray", sys.executable])

        self.assertEqual(status, 1)
        self.assertIn("no governed AOT manifest cases", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
