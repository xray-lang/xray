"""Regression evidence must not turn missing or timed-out cases into passes."""

import base64
import copy
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()
from xraytest import proc, ratchet, regression_report

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
runner = load_module("regression_runner_under_test", ROOT / "scripts/run_regression_tests.py")


def report(outcomes):
    return {
        "schema_version": regression_report.SCHEMA_VERSION,
        "manifest": [{"case_id": name} for name in outcomes],
        "results": [{"case_id": name, "outcome": result} for name, result in outcomes.items()],
        "counts": {kind: sum(value == kind for value in outcomes.values())
                   for kind in regression_report.OUTCOMES},
        "stable_inputs": True,
    }


class RegressionReportTests(unittest.TestCase):
    def test_timeout_and_same_basename_do_not_change_case_identity(self):
        timeout = "tests/regression/a/same.xr"
        passed = "tests/regression/b/same.xr"
        outcomes = regression_report.validate(report({timeout: "TIMEOUT", passed: "PASS"}),
                                              {timeout, passed})
        verdict = ratchet.evaluate(failed={name for name, outcome in outcomes.items()
                                          if outcome in regression_report.FAILED_OUTCOMES},
                                   baseline={timeout})
        self.assertTrue(verdict.ok)
        self.assertEqual(verdict.now_passing, [])

    def test_missing_duplicate_foreign_and_mismatched_results_are_rejected(self):
        name = "tests/regression/a.xr"
        complete = report({name: "PASS"})
        mutations = [
            lambda data: data["results"].clear(),
            lambda data: data["results"].append(data["results"][0]),
            lambda data: data["results"][0].update(case_id="tests/regression/foreign.xr"),
            lambda data: data["counts"].update(PASS=7),
            lambda data: data.update(schema_version=1),
        ]
        for mutate in mutations:
            data = copy.deepcopy(complete)
            mutate(data)
            with self.assertRaises(ValueError):
                regression_report.validate(data, {name})

    def test_runner_preserves_raw_streams_and_distinguishes_process_outcomes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = root / "tests/regression/one.xr"
            case.parent.mkdir(parents=True)
            case.write_text("@test fn sample() {}\n")
            for code, timed_out, expected in [(0, False, "PASS"), (1, False, "FAIL"),
                                               (-11, False, "CRASH"),
                                               (-9, True, "TIMEOUT"),
                                               (0xC0000005, False, "CRASH")]:
                child = proc.ProcResult((), code, b"2 passed\n\xff", b"failure\xfe", timed_out)
                with mock.patch.object(runner, "PROJECT_ROOT", root), \
                        mock.patch.object(runner.proc, "run", return_value=child):
                    result = runner.run_one(Path("xray"), case, 10)
                self.assertEqual(result.case_id, "tests/regression/one.xr")
                self.assertEqual(result.verdict, expected)
                self.assertEqual(base64.b64decode(result.as_json()["stdout_base64"]), child.stdout)
                self.assertEqual(base64.b64decode(result.as_json()["stderr_base64"]), child.stderr)

if __name__ == "__main__":
    unittest.main()
