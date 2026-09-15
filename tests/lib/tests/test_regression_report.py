"""Regression evidence must not turn missing or timed-out cases into passes."""

import base64
import copy
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()
from xraytest import proc, ratchet, regression_report

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
runner = load_module("regression_runner_under_test", ROOT / "scripts/run_regression_tests.py")
gate = load_module("regression_gate_under_test", ROOT / "scripts/check_regression_corpus.py")


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

    def run_gate(self, payload, baseline, *, code=1, dump=False):
        with tempfile.TemporaryDirectory() as directory:
            baseline_file = Path(directory) / "baseline.txt"
            baseline_file.write_text("\n".join(baseline))
            def run_corpus(args, path):
                path.write_text(json.dumps(payload))
                return code, "raw failure evidence\n"
            stdout = io.StringIO()
            cases = [ROOT / row["case_id"] for row in payload["manifest"]]
            argv = ["gate", "--baseline", str(baseline_file), "--nondeterministic",
                    str(Path(directory) / "empty.txt")]
            if dump:
                argv.append("--dump-failed")
            with mock.patch.object(gate, "run_corpus", side_effect=run_corpus), \
                    mock.patch.object(gate, "collect_cases", return_value=cases), \
                    redirect_stdout(stdout):
                code = gate.main(argv)
            return code, stdout.getvalue()

    def test_timeout_stays_baselined_and_dump_flag_preserves_output(self):
        name = "tests/regression/a.xr"
        payload = report({name: "TIMEOUT"})
        payload.update(passed=0, executed=0, elapsed_seconds=10)
        code, output = self.run_gate(payload, {name}, dump=True)
        self.assertEqual(code, 0)
        self.assertIn("raw failure evidence", output)
        self.assertNotIn("now pass:", output)

    def test_missing_baseline_is_not_claimed_as_fixed(self):
        code, output = self.run_gate(report({"tests/regression/a.xr": "PASS"}),
                                     {"tests/regression/missing.xr"}, code=0)
        self.assertEqual(code, 1)
        self.assertIn("absence is not a passing result", output)
        self.assertNotIn("now pass:", output)

    def test_changed_inputs_and_auxiliary_failures_are_red(self):
        name = "tests/regression/a.xr"
        for changed, auxiliary in [(True, None), (False, {"outcome": "FAIL"})]:
            payload = report({name: "PASS"})
            payload.update(stable_inputs=not changed, backend_diff=auxiliary)
            code, _ = self.run_gate(payload, set())
            self.assertEqual(code, 1)

    def test_existing_report_cannot_survive_a_runner_startup_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "old.json"
            path.write_text(json.dumps(report({"tests/regression/a.xr": "PASS"})))
            with mock.patch.object(gate, "run_corpus", return_value=(1, "startup failure")), \
                    redirect_stdout(io.StringIO()) as output:
                code = gate.main(["gate", "--json", str(path)])
            self.assertEqual(code, 1)
            self.assertIn("no JSON report", output.getvalue())
            self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
