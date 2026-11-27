"""Fail-closed tests for the focused ThreadSanitizer runner.

These run on Windows with fake clang/CTest results.  They verify the commands
and verdict logic without claiming that ThreadSanitizer itself ran there.
"""

import io
import json
import tempfile
import unittest
from contextlib import ExitStack, redirect_stdout
from pathlib import Path
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()
from xraytest import proc  # noqa: E402

ROOT = Path(__file__).resolve().parents[3]
tsan = load_module(
    "run_tsan_focused_under_test", ROOT / "scripts" / "run_tsan_focused.py"
)


def outcome(*, returncode=0, stdout=b"", stderr=b"", timed_out=False, argv=()):
    return proc.ProcResult(tuple(argv), returncode, stdout, stderr, timed_out)


class Log:
    def __init__(self):
        self.entries = []

    def __call__(self, message, *, error=False):
        self.entries.append((message, error))


class BuildIdentityTest(unittest.TestCase):
    def test_explicit_build_targets_cover_current_runtime_kernels(self):
        build = Path("build-tsan-test")
        spec = tsan.focused_build_spec(build)
        self.assertEqual(spec.build_dir, build)
        self.assertEqual(
            spec.targets,
            (
                "xray",
                "test_os_thread_token",
                "test_xir_values",
                "test_xir_signed_object_rc",
                "test_native_managed_kernel",
                "test_native_managed_root",
                "test_native_backing",
                "test_native_string_backing_ops",
                "test_native_place",
                "test_native_value_plan",
                "test_native_managed_status",
                "test_native_capture_backing",
                "test_rbc_code_owner",
                "test_xir_value_allocations",
            ),
        )
        self.assertEqual(spec.sanitizer_flags, ("ENABLE_TSAN=ON", "XR_XIR_PARANOID=ON"))
        self.assertIn("os_thread_token", tsan.KERNEL_TESTS)
        self.assertNotIn("test_os_thread_token", tsan.KERNEL_TESTS)

    def test_fake_cmake_build_command_names_the_exact_targets(self):
        spec = tsan.focused_build_spec(Path("build-tsan-test"))
        fake = mock.Mock(return_value=outcome())
        with mock.patch.object(tsan.sanitizer.proc, "run", fake):
            self.assertTrue(tsan.sanitizer.build(spec, 3, 777, Log()))
        self.assertEqual(
            tuple(str(arg) for arg in fake.call_args.args[0]),
            (
                "cmake", "--build", "build-tsan-test", "-j", "3", "--target",
                *spec.targets,
            ),
        )
        self.assertEqual(fake.call_args.kwargs["timeout"], 777)

    def test_fake_cmake_build_failure_propagates(self):
        spec = tsan.focused_build_spec(Path("build-tsan-test"))
        fake = mock.Mock(return_value=outcome(returncode=1, stderr=b"build failed"))
        with mock.patch.object(tsan.sanitizer.proc, "run", fake):
            self.assertFalse(tsan.sanitizer.build(spec, 3, 777, Log()))


class RuntimeProbeTest(unittest.TestCase):
    def test_windows_is_an_explicit_failure_not_a_skip(self):
        calls = []
        log = Log()

        self.assertFalse(
            tsan.tsan_runtime_available(
                log, run=lambda *args, **kwargs: calls.append((args, kwargs)),
                is_windows=True,
            )
        )
        self.assertEqual(calls, [])
        self.assertTrue(any(error and "unsupported" in message
                            for message, error in log.entries))

    def test_probe_compiles_with_clang_and_executes_the_runtime(self):
        calls = []

        def fake_run(argv, **kwargs):
            calls.append((tuple(str(arg) for arg in argv), kwargs))
            return outcome(argv=argv)

        self.assertTrue(tsan.tsan_runtime_available(Log(), run=fake_run, is_windows=False))
        self.assertEqual(calls[0][0][0:3],
                         ("clang", "-fsanitize=thread", "-fno-omit-frame-pointer"))
        self.assertEqual(calls[0][1]["stdin"], b"int main(void) { return 0; }\n")
        self.assertEqual(len(calls[1][0]), 1)
        self.assertEqual(calls[1][1]["env"]["TSAN_OPTIONS"],
                         "halt_on_error=1 exitcode=86")

    def test_probe_compile_failure_propagates(self):
        calls = []

        def fake_run(argv, **kwargs):
            calls.append(argv)
            return outcome(returncode=1, stderr=b"no tsan runtime")

        self.assertFalse(tsan.tsan_runtime_available(Log(), run=fake_run,
                                                     is_windows=False))
        self.assertEqual(len(calls), 1)

    def test_probe_execution_failure_propagates(self):
        results = iter((outcome(), outcome(returncode=86, stderr=b"runtime failed")))
        self.assertFalse(tsan.tsan_runtime_available(
            Log(), run=lambda argv, **kwargs: next(results), is_windows=False))


class CTestSelectionTest(unittest.TestCase):
    def setUp(self):
        self.build = Path(tempfile.mkdtemp(prefix="xt_tsan_runner."))
        self.log = Log()

    def tearDown(self):
        import shutil

        shutil.rmtree(self.build, ignore_errors=True)

    @staticmethod
    def discovery(names):
        payload = {"tests": [{"name": name} for name in names]}
        return outcome(stdout=json.dumps(payload).encode("utf-8"))

    def test_exact_ctest_identity_and_discovery_command(self):
        calls = []

        def fake_run(argv, **kwargs):
            calls.append(tuple(str(arg) for arg in argv))
            return self.discovery(reversed(tsan.KERNEL_TESTS))

        selected = tsan.discover_kernel_ctests(self.build, self.log, run=fake_run)
        self.assertEqual(set(selected), set(tsan.KERNEL_TESTS))
        self.assertEqual(
            calls[0],
            (
                "ctest", "--test-dir", str(self.build), "--show-only=json-v1",
                "-R", tsan.KERNEL_REGEX,
            ),
        )

    def test_discovery_regex_uses_only_cmake_regex_syntax(self):
        """CTest compiles -R with CMake's own engine, not Python's.

        Every case here drives discovery through a fake run, so a pattern that
        Python accepts and CMake rejects passes all of them while the real lane
        fails: CTest prints its compile error ahead of the JSON and discovery
        reports unreadable output instead of a bad pattern.
        """
        self.assertNotIn("(?", tsan.KERNEL_REGEX)
        self.assertTrue(tsan.KERNEL_REGEX.startswith("^("))
        self.assertTrue(tsan.KERNEL_REGEX.endswith(")$"))
        for name in tsan.KERNEL_TESTS:
            self.assertIn(name, tsan.KERNEL_REGEX)

    def test_zero_selected_fails_closed(self):
        selected = tsan.discover_kernel_ctests(
            self.build, self.log, run=lambda argv, **kwargs: self.discovery(()))
        self.assertIsNone(selected)
        self.assertTrue(any("0 tests selected" in message
                            for message, _ in self.log.entries))

    def test_missing_target_fails_closed(self):
        selected = tsan.discover_kernel_ctests(
            self.build,
            self.log,
            run=lambda argv, **kwargs: self.discovery(tsan.KERNEL_TESTS[:-1]),
        )
        self.assertIsNone(selected)
        self.assertTrue(any(f"missing {tsan.KERNEL_TESTS[-1]}" in message
                            for message, _ in self.log.entries))

    def test_discovery_command_failure_propagates(self):
        selected = tsan.discover_kernel_ctests(
            self.build,
            self.log,
            run=lambda argv, **kwargs: outcome(returncode=2, stderr=b"bad ctest"),
        )
        self.assertIsNone(selected)

    def test_invalid_json_fails_closed(self):
        selected = tsan.discover_kernel_ctests(
            self.build,
            self.log,
            run=lambda argv, **kwargs: outcome(stdout=b"not json"),
        )
        self.assertIsNone(selected)

    def test_ctest_execution_command_and_environment(self):
        calls = []

        def fake_run(argv, **kwargs):
            calls.append((tuple(str(arg) for arg in argv), kwargs))
            if len(calls) == 1:
                return self.discovery(tsan.KERNEL_TESTS)
            return outcome(stdout=b"all focused tests passed\n")

        env = {"TSAN_OPTIONS": "sentinel"}
        with redirect_stdout(io.StringIO()):
            self.assertTrue(tsan.run_kernel_ctests(
                self.build, env, 777, self.log, run=fake_run))
        self.assertEqual(calls[1][0], (
            "ctest", "--test-dir", str(self.build), "--output-on-failure",
            "--no-tests=error", "-j", "1", "--timeout", "300",
            "-R", tsan.KERNEL_REGEX,
        ))
        self.assertIs(calls[1][1]["env"], env)
        self.assertEqual(calls[1][1]["timeout"], 777)

    def test_ctest_execution_failure_propagates(self):
        results = iter((
            self.discovery(tsan.KERNEL_TESTS),
            outcome(returncode=8, stderr=b"one test failed"),
        ))
        with redirect_stdout(io.StringIO()):
            self.assertFalse(tsan.run_kernel_ctests(
                self.build, {}, 777, self.log,
                run=lambda argv, **kwargs: next(results),
            ))


class LaneVerdictTest(unittest.TestCase):
    def run_lane(self, failed_case=None, *, empty_first_report=False,
                 kernels_ok=True, closed_case=None):
        calls = []

        def fake_run(argv, **kwargs):
            calls.append(tuple(str(arg) for arg in argv))
            if len(calls) == 1:
                return closed_case or outcome()
            if argv[1] == "test":
                passed = 0 if len(calls) == 2 and empty_first_report else 1
                Path(argv[3]).write_text(json.dumps({"schema": 1, "files": [{
                    "tests": passed, "passed": passed, "failed": 0,
                    "errors": 0, "timeouts": 0, "skipped": 0,
                }]}), encoding="utf-8")
            return failed_case if len(calls) == 2 and failed_case else outcome()

        output = io.StringIO()
        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(tsan, "tsan_runtime_available", return_value=True))
            stack.enter_context(mock.patch.object(tsan.sanitizer, "verify_build_tree", return_value=None))
            build = stack.enter_context(mock.patch.object(tsan.sanitizer, "build", return_value=True))
            stack.enter_context(mock.patch.object(tsan, "run_kernel_ctests", return_value=kernels_ok))
            stack.enter_context(mock.patch.object(tsan.Path, "is_file", return_value=True))
            stack.enter_context(mock.patch.object(tsan.os, "access", return_value=True))
            stack.enter_context(mock.patch.object(tsan.proc, "run", side_effect=fake_run))
            stack.enter_context(redirect_stdout(output))
            result = tsan.main(["run_tsan_focused.py"])
        build.assert_called_once()
        self.assertEqual(len(calls), len(tsan.CASES) + 1)
        self.assertEqual(Path(calls[0][0]).stem, tsan.CLOSED_THREADS_TARGET)
        self.assertEqual(calls[0][1:], ("--closed-threads",))
        self.assertEqual(
            [(call[1], call[-1]) for call in calls[1:]],
            [(mode, str(tsan.PROJECT_DIR / path)) for mode, path in tsan.CASES],
        )
        return result, output.getvalue()

    def test_success_requires_all_source_cases_to_execute(self):
        code, output = self.run_lane()
        self.assertEqual(code, 0)
        self.assertIn("VERDICT: PASS", output)

    def test_source_failures_without_race_warnings_fail_the_lane(self):
        for failed in (
            outcome(returncode=1, stderr=b"unsupported canonical operation\n"),
            outcome(returncode=-6, stderr=b"publication invariant failed\n"),
            outcome(returncode=124, timed_out=True, stdout=b"partial trace\n"),
        ):
            with self.subTest(exit=failed.returncode):
                code, output = self.run_lane(failed)
                self.assertEqual(code, 1)
                self.assertIn("VERDICT: FAIL", output)
                self.assertNotIn("VERDICT: PASS", output)
                self.assertIn(failed.combined_text().strip(), output)

    def test_zero_executed_source_tests_fail_even_with_zero_exit(self):
        code, output = self.run_lane(empty_first_report=True)
        self.assertEqual(code, 1)
        self.assertIn("VM test report has no successful execution", output)
        self.assertNotIn("VERDICT: PASS", output)

    def test_kernel_failure_keeps_original_source_cases_and_fails_the_lane(self):
        code, output = self.run_lane(kernels_ok=False)
        self.assertEqual(code, 1)
        self.assertIn("VERDICT: FAIL (focused kernel CTest set failed)", output)

    def test_closed_thread_failure_propagates(self):
        code, output = self.run_lane(closed_case=outcome(returncode=86))
        self.assertEqual(code, 1)
        self.assertIn("VERDICT: FAIL (closed thread allocations failed)", output)


if __name__ == "__main__":
    unittest.main()
