"""Tests for focused selection in the tiered test runner."""

import io
import sys
import tempfile
import unittest
from contextlib import ExitStack, redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from _support import bootstrap_xraytest, load_module

bootstrap_xraytest()

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
runner = load_module("tiered_test_runner_under_test", ROOT / "scripts" / "t.py")


class FocusedSelectionTest(unittest.TestCase):
    def test_broad_options_do_not_select_out_auxiliary_corpora(self):
        self.assertFalse(runner.has_explicit_ctest_selection([]))
        self.assertFalse(runner.has_explicit_ctest_selection(["--output-on-failure"]))
        self.assertFalse(runner.has_explicit_ctest_selection(["-E", "slow"]))

    def test_name_and_label_selectors_are_focused(self):
        for arguments in (["-R", "parser"], ["-Rparser"],
                          ["--tests-regex=parser"], ["-L", "unit"], ["-Lunit"],
                          ["--label-regex", "unit"]):
            with self.subTest(arguments=arguments):
                self.assertTrue(runner.has_explicit_ctest_selection(arguments))

    def test_ninja_manifest_is_refreshed_before_test_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "build.ninja").write_text("# ninja\n", encoding="utf-8")
            completed = SimpleNamespace(ok=True, combined_text=lambda: "")
            with mock.patch.object(runner.proc, "run", return_value=completed) as run:
                self.assertTrue(runner.refresh_cmake_manifest(build, 7))
            run.assert_called_once_with([
                "cmake", "--build", str(build), "-j", "7", "--target", "build.ninja"
            ])

    def test_non_ninja_manifest_needs_no_refresh(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(runner.proc, "run") as run:
                self.assertTrue(runner.refresh_cmake_manifest(Path(directory), 3))
            run.assert_not_called()

    def test_stateful_ctest_selectors_are_focused(self):
        for arguments in (["-I", "1,1"], ["--tests-information", "1,1"],
                          ["--rerun-failed"], ["--tests-from-file", "tests.txt"]):
            with self.subTest(arguments=arguments):
                self.assertTrue(runner.has_explicit_ctest_selection(arguments))


class PhaseTimingTest(unittest.TestCase):
    def test_timer_reports_elapsed_time_and_propagates_failure(self):
        for fails in (False, True):
            with self.subTest(fails=fails):
                output = io.StringIO()
                with redirect_stdout(output), mock.patch.object(
                        runner.time, "perf_counter", side_effect=[10.0, 12.5]):
                    if fails:
                        with self.assertRaisesRegex(ValueError, "phase failure"):
                            with runner.timed_phase("probe"):
                                raise ValueError("phase failure")
                    else:
                        with runner.timed_phase("probe"):
                            pass
                self.assertIn("timing: probe=2.500s", output.getvalue())

    def run_focused(self, build, *, build_ok=True, ctest_code=0):
        output = io.StringIO()
        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(output))
            stack.enter_context(mock.patch.dict(runner.os.environ,
                                               {"XR_BUILD_DIR": str(build)}, clear=True))
            stack.enter_context(mock.patch.object(runner.sanitizer,
                                                 "activate_windows_msvc_environment",
                                                 return_value=True))
            stack.enter_context(mock.patch.object(runner, "refresh_cmake_manifest",
                                                 return_value=True))
            stack.enter_context(mock.patch.object(runner, "ctest_names",
                                                 return_value=["test_xr_program_source_build"]))
            stack.enter_context(mock.patch.object(runner, "build_selected",
                                                 return_value=build_ok))
            ctest = stack.enter_context(mock.patch.object(runner.subprocess, "call",
                                                         return_value=ctest_code))
            corpus = stack.enter_context(mock.patch.object(runner, "run_regression_corpus"))
            result = runner.main(["t.py", "t0", "-R", "^test_xr_program_source_build$"])
            corpus.assert_not_called()
        return result, output.getvalue(), ctest

    def test_timing_keeps_focused_selection_and_test_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            for code in (0, 8):
                with self.subTest(code=code):
                    result, output, ctest = self.run_focused(Path(directory), ctest_code=code)
                    self.assertEqual(result, code)
                    self.assertIn("timing: toolchain setup=", output)
                    self.assertIn("timing: ctest=", output)
                    self.assertIn("unselected t0 tests and auxiliary corpora", output)
                    self.assertEqual(ctest.call_args.args[0][-2:],
                                     ["-R", "^test_xr_program_source_build$"])

    def test_failed_build_never_runs_ctest(self):
        with tempfile.TemporaryDirectory() as directory:
            result, output, ctest = self.run_focused(Path(directory), build_ok=False)
        self.assertEqual(result, 1)
        ctest.assert_not_called()
        self.assertNotIn("timing: ctest=", output)


if __name__ == "__main__":
    unittest.main()
