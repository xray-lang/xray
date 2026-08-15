"""Unit tests for the AOT manifest sweep's measurement preconditions."""

import contextlib
import io
import sys
import tempfile
import time
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
    def test_jobs_default_to_serial_and_accept_explicit_width(self):
        self.assertEqual(runner.configure_jobs(""), 1)
        self.assertEqual(runner.configure_jobs("4"), 4)

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

    def test_invalid_jobs_fail_closed(self):
        stderr = io.StringIO()
        with mock.patch.object(runner, "collect_cases", return_value=[Path(__file__)]):
            with mock.patch.dict(
                runner.os.environ, {"XRAY_AOT_MANIFEST_JOBS": "many"}, clear=True
            ):
                with contextlib.redirect_stderr(stderr):
                    status = runner.main(["--xray", sys.executable])

        self.assertEqual(status, 1)
        self.assertIn("must be a positive integer", stderr.getvalue())


class ParallelSchedulingTest(unittest.TestCase):
    def _run(self, root: Path, cases: list[Path], jobs: int):
        outputs: list[Path] = []

        def fake_check(_xray, case, out_c, order):
            outputs.append(out_c)
            time.sleep((len(cases) - order) * 0.001)
            failures = (f"{runner.rel(case)}: red",) if order in (0, 2) else ()
            return runner.CaseVerdict(order, runner.rel(case), failures=failures, checked=1)

        with mock.patch.object(runner, "rel", side_effect=lambda path: path.name):
            with mock.patch.object(runner, "check_case", side_effect=fake_check):
                verdicts = runner.run_cases(sys.executable, cases, root, jobs)
        return verdicts, outputs

    def test_serial_and_parallel_verdicts_are_order_equivalent(self):
        with tempfile.TemporaryDirectory(prefix="xt_manifest_parallel.") as tmp:
            root = Path(tmp)
            cases = [root / f"case-{index}.xr" for index in range(4)]
            for case in cases:
                case.touch()
            serial, serial_outputs = self._run(root, cases, 1)
            parallel, parallel_outputs = self._run(root, cases, 3)

        self.assertEqual(serial, parallel)
        self.assertEqual([v.case for v in parallel], [c.name for c in cases])
        self.assertEqual([v.failures for v in parallel], [v.failures for v in serial])
        self.assertEqual(len(set(serial_outputs)), len(cases))
        self.assertEqual(len(set(parallel_outputs)), len(cases))

    def test_missing_worker_output_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_manifest_missing.") as tmp:
            case = Path(tmp) / "case.xr"
            case.touch()
            with mock.patch.object(runner, "rel", side_effect=lambda path: path.name):
                with mock.patch.object(runner.scheduler.Scheduler, "run", return_value={}):
                    verdicts = runner.run_cases(sys.executable, [case], Path(tmp), 2)

        self.assertEqual(len(verdicts), 1)
        self.assertIn("worker produced no verdict", verdicts[0].failures[0])

    def test_worker_exception_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="xt_manifest_worker.") as tmp:
            case = Path(tmp) / "case.xr"
            case.touch()
            with mock.patch.object(runner, "rel", side_effect=lambda path: path.name):
                with mock.patch.object(runner, "check_case", side_effect=RuntimeError("boom")):
                    verdicts = runner.run_cases(sys.executable, [case], Path(tmp), 2)

        self.assertIn("worker raised RuntimeError: boom", verdicts[0].failures[0])


if __name__ == "__main__":
    unittest.main()
