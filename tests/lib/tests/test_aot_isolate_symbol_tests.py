"""Unit tests for the AOT isolate/symbol gate's fail-closed entry checks."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

_AOT_DIR = Path(__file__).resolve().parents[2] / "aot"
runner = load_module(
    "run_aot_isolate_symbol_tests_under_test",
    _AOT_DIR / "run_aot_isolate_symbol_tests.py",
)


class EntryIdentityTest(unittest.TestCase):
    def test_missing_xray_binary_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_aot_isolate.") as tmp:
            missing = Path(tmp) / "missing-xray"
            output = io.StringIO()

            with contextlib.redirect_stdout(output):
                status = runner.main(
                    ["run_aot_isolate_symbol_tests.py", "--xray", str(missing)]
                )

            self.assertEqual(status, 1)
            self.assertIn("FAIL: xray binary not executable", output.getvalue())
            self.assertIn("0 passed, 1 failed", output.getvalue())


if __name__ == "__main__":
    unittest.main()
