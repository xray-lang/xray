"""Diagnostic gates must not hide compiler rejection or unexpected stderr."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()
ROOT = Path(__file__).resolve().parents[3]
errors = load_module("uncaught_error_gate_under_test", ROOT / "tests/cli/run_uncaught_error_tests.py")
panics = load_module("panic_report_gate_under_test", ROOT / "tests/cli/run_panic_report_tests.py")


class DiagnosticGateTest(unittest.TestCase):
    def test_silent_case_rejects_unexpected_stderr(self):
        for stderr, failures in ((b"", 0), (b"unexpected error\n", 1), (b"\n", 1), (b"\r\n", 1)):
            with self.subTest(stderr=stderr), contextlib.redirect_stderr(io.StringIO()), \
                    contextlib.redirect_stdout(io.StringIO()):
                rec = errors.Recorder()
                result = errors.proc.ProcResult(("program",), 0, b"caught\n", stderr, False)
                rec.check("caught", 0, "caught", "", result)
                self.assertEqual(rec.failed, failures)
                self.assertEqual(rec.passed, 1 - failures)

    def test_compiler_rejection_is_a_failure_with_its_diagnostic(self):
        rejected = errors.proc.ProcResult(("xray", "build"), 1, b"", b"source rejected", False)
        for gate in (errors, panics):
            with self.subTest(gate=gate.__name__), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                rec = gate.Recorder()
                err, out = io.StringIO(), io.StringIO()
                with mock.patch.object(gate.proc, "run", return_value=rejected), \
                        contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
                    if gate is errors:
                        self.assertIsNone(gate.build_native(rec, Path("xray"), work / "bad.xr",
                                                           work / "bad.exe", 5))
                    else:
                        gate.check_aot(rec, Path("xray"), work / "bad.xr", work, 5)
                self.assertEqual(rec.failed, 1)
                self.assertIn("source rejected", err.getvalue())
                self.assertNotIn("SKIP", out.getvalue())

    def test_default_build_requires_product_exit_status(self):
        for exit_code, failures in ((1, 0), (214, 1)):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                rec = panics.Recorder()
                calls = []

                def run(command, **kwargs):
                    calls.append(command)
                    if "build" in command:
                        output = Path(command[command.index("-o") + 1])
                        output.write_text("generated artifact", encoding="utf-8")
                        return panics.proc.ProcResult(tuple(command), 0, b"", b"", False)
                    return panics.proc.ProcResult(tuple(command), exit_code, b"before\n",
                                                  panics.DIV_REPORT.encode(), False)

                with (
                    mock.patch.object(panics.proc, "run", side_effect=run),
                    mock.patch.object(panics.os, "access", return_value=True),
                    contextlib.redirect_stderr(io.StringIO()),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    panics.check_default_build(rec, Path("xray"), work / "div.xr", work, 5)
                self.assertEqual(rec.failed, failures)
                self.assertEqual(rec.passed, 1 - failures)
                self.assertEqual(len(calls), 3)

    def test_success_without_an_artifact_is_a_failure(self):
        empty = errors.proc.ProcResult(("xray", "build"), 0, b"", b"", False)
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(errors.proc, "run", return_value=empty), \
                contextlib.redirect_stderr(io.StringIO()):
            rec = errors.Recorder()
            self.assertIsNone(errors.build_native(rec, Path("xray"), Path("valid.xr"),
                                                  Path(temporary) / "absent.exe", 5))
            self.assertEqual(rec.failed, 1)


if __name__ == "__main__":
    unittest.main()
