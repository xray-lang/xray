"""Fail-closed tests for the Task 276 focused ThreadSanitizer runner.

These run on Windows with fake clang/CTest results.  They verify the commands
and verdict logic without claiming that ThreadSanitizer itself ran there.
"""

import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
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
    def test_explicit_build_targets_cover_every_task276_boundary(self):
        build = Path("build-tsan-test")
        spec = tsan.focused_build_spec(build)
        self.assertEqual(spec.build_dir, build)
        self.assertEqual(
            spec.targets,
            (
                "xray",
                "test_ownership_audit",
                "test_vm_decoded_cache",
                "test_xtp_resource_stress",
                "test_runtime_generation",
                "test_entry_cell_runtime_archive",
                "test_runtime_program_archive",
            ),
        )
        self.assertIn("-fsanitize=thread", " ".join(spec.sanitizer_flags))

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
            return self.discovery(reversed(tsan.TASK276_TSAN_TESTS))

        selected = tsan.discover_task276_ctests(self.build, self.log, run=fake_run)
        self.assertEqual(set(selected), set(tsan.TASK276_TSAN_TESTS))
        self.assertEqual(
            calls[0],
            (
                "ctest", "--test-dir", str(self.build), "--show-only=json-v1",
                "-R", tsan.TASK276_TSAN_REGEX,
            ),
        )

    def test_discovery_regex_uses_only_cmake_regex_syntax(self):
        """CTest compiles -R with CMake's own engine, not Python's.

        Every case here drives discovery through a fake run, so a pattern that
        Python accepts and CMake rejects passes all of them while the real lane
        fails: CTest prints its compile error ahead of the JSON and discovery
        reports unreadable output instead of a bad pattern.
        """
        self.assertNotIn("(?", tsan.TASK276_TSAN_REGEX)
        self.assertTrue(tsan.TASK276_TSAN_REGEX.startswith("^("))
        self.assertTrue(tsan.TASK276_TSAN_REGEX.endswith(")$"))
        for name in tsan.TASK276_TSAN_TESTS:
            self.assertIn(name, tsan.TASK276_TSAN_REGEX)

    def test_zero_selected_fails_closed(self):
        selected = tsan.discover_task276_ctests(
            self.build, self.log, run=lambda argv, **kwargs: self.discovery(()))
        self.assertIsNone(selected)
        self.assertTrue(any("0 tests selected" in message
                            for message, _ in self.log.entries))

    def test_missing_target_fails_closed(self):
        selected = tsan.discover_task276_ctests(
            self.build,
            self.log,
            run=lambda argv, **kwargs: self.discovery(tsan.TASK276_TSAN_TESTS[:-1]),
        )
        self.assertIsNone(selected)
        self.assertTrue(any(f"missing {tsan.TASK276_TSAN_TESTS[-1]}" in message
                            for message, _ in self.log.entries))

    def test_discovery_command_failure_propagates(self):
        selected = tsan.discover_task276_ctests(
            self.build,
            self.log,
            run=lambda argv, **kwargs: outcome(returncode=2, stderr=b"bad ctest"),
        )
        self.assertIsNone(selected)

    def test_invalid_json_fails_closed(self):
        selected = tsan.discover_task276_ctests(
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
                return self.discovery(tsan.TASK276_TSAN_TESTS)
            return outcome(stdout=b"all focused tests passed\n")

        env = {"TSAN_OPTIONS": "sentinel"}
        with redirect_stdout(io.StringIO()):
            self.assertTrue(tsan.run_task276_ctests(
                self.build, env, 777, self.log, run=fake_run))
        self.assertEqual(calls[1][0], (
            "ctest", "--test-dir", str(self.build), "--output-on-failure",
            "--no-tests=error", "-j", "1", "--timeout", "300",
            "-R", tsan.TASK276_TSAN_REGEX,
        ))
        self.assertIs(calls[1][1]["env"], env)
        self.assertEqual(calls[1][1]["timeout"], 777)

    def test_ctest_execution_failure_propagates(self):
        results = iter((
            self.discovery(tsan.TASK276_TSAN_TESTS),
            outcome(returncode=8, stderr=b"one test failed"),
        ))
        with redirect_stdout(io.StringIO()):
            self.assertFalse(tsan.run_task276_ctests(
                self.build, {}, 777, self.log,
                run=lambda argv, **kwargs: next(results),
            ))


if __name__ == "__main__":
    unittest.main()
