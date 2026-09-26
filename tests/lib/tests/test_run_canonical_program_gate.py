"""Tests for the small exact canonical-Program lane runner."""

import os
import io
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module


bootstrap_xraytest()
ROOT = Path(__file__).resolve().parents[3]
runner = load_module(
    "canonical_program_gate_runner_under_test",
    ROOT / "scripts" / "run_canonical_program_gate.py",
)


class CanonicalProgramGateEnvironmentTests(unittest.TestCase):
    def test_provider_lane_covers_actual_stdlib_bindings(self) -> None:
        for lane in ("provider", "asan-provider"):
            targets, tests = runner.LANES[lane]
            for name in ("test_stdlib_provider_contract", "test_stdlib_provider_binding"):
                self.assertIn(name, targets)
                self.assertIn(name, tests)
            self.assertIn("test_stdlib_provider_metadata", tests)

    def test_failed_command_reports_elapsed_time_and_still_fails(self) -> None:
        output = io.StringIO()
        with mock.patch.object(runner.subprocess, "run",
                               return_value=subprocess.CompletedProcess(["probe"], 7)), \
                mock.patch.object(runner.time, "perf_counter", side_effect=(10.0, 12.5)), \
                mock.patch.object(runner.sys, "stderr", output):
            with self.assertRaises(subprocess.CalledProcessError) as failure:
                runner.run(["probe"])
        self.assertEqual(failure.exception.returncode, 7)
        self.assertEqual(runner.json.loads(output.getvalue()),
                         {"command": ["probe"], "exit_code": 7, "elapsed_seconds": 2.5})

    def test_release_lanes_use_shared_edit_loop_environment(self) -> None:
        for lane in ("semantic", "native", "provider"):
            with self.subTest(lane=lane):
                self.assertEqual(
                    runner.lane_environment(lane),
                    ("XR_BUILD_DIR", "XR_JOBS", "build"),
                )

    def test_asan_lane_uses_sanitizer_environment(self) -> None:
        for lane in ("asan-source", "asan-provider"):
            with self.subTest(lane=lane):
                self.assertEqual(
                    runner.lane_environment(lane),
                    ("XR_ASAN_BUILD_DIR", "XR_ASAN_JOBS", "build-asan"),
                )

    def test_job_environment_is_strict(self) -> None:
        with mock.patch.dict(os.environ, {"XR_JOBS": "6"}, clear=True):
            self.assertEqual(runner.environment_jobs("XR_JOBS", 2), 6)
        with mock.patch.dict(os.environ, {"XR_JOBS": "many"}, clear=True):
            with self.assertRaisesRegex(ValueError, "XR_JOBS must be an integer"):
                runner.environment_jobs("XR_JOBS", 2)

    def test_exact_regex_cannot_select_prefix_or_suffix_neighbors(self) -> None:
        regex = runner.re.compile(runner.exact_regex(("test_one", "test_two")))
        self.assertIsNotNone(regex.fullmatch("test_one"))
        self.assertIsNotNone(regex.fullmatch("test_two"))
        self.assertIsNone(regex.fullmatch("test_one_extra"))
        self.assertIsNone(regex.fullmatch("prefix_test_two"))

    def test_lane_execution_requires_the_exact_selected_set(self) -> None:
        def execute(command, **_kwargs):
            report = Path(command[command.index("--output-junit") + 1])
            report.write_text(
                "<testsuite><testcase name='one'/><testcase name='two'/></testsuite>",
                encoding="utf-8",
            )
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner.subprocess, "run", side_effect=execute):
            self.assertGreaterEqual(
                runner.run_exact_tests(["ctest"], ("two", "one")), 0.0
            )

    def test_lane_execution_rejects_equal_count_drift(self) -> None:
        def execute(command, **_kwargs):
            report = Path(command[command.index("--output-junit") + 1])
            report.write_text(
                "<testsuite><testcase name='one'/><testcase name='wrong'/></testsuite>",
                encoding="utf-8",
            )
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(runner.subprocess, "run", side_effect=execute):
            with self.assertRaisesRegex(RuntimeError,
                                        "missing two, unexpected wrong"):
                runner.run_exact_tests(["ctest"], ("one", "two"))


if __name__ == "__main__":
    unittest.main()
