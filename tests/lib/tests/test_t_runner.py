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
    def test_t0_is_a_bounded_exact_inventory(self):
        self.assertLess(len(runner.T0_CTEST_NAMES), 100)
        self.assertEqual(len(runner.T0_CTEST_NAMES), len(set(runner.T0_CTEST_NAMES)))
        self.assertTrue(set(runner.canonical_profile.CTEST_NAMES) <=
                        set(runner.T0_CTEST_NAMES))
        self.assertNotIn(".*", runner.T0_INCLUDE)

    def test_requested_build_dir_matches_fast_tree_selection(self):
        with mock.patch.dict(runner.os.environ, {}, clear=True), mock.patch.object(
                runner.platform, "IS_WINDOWS", True):
            self.assertEqual(runner.requested_build_dir().name, "build")
        with mock.patch.dict(runner.os.environ, {"XR_FAST": "1"}, clear=True), \
                mock.patch.object(runner.platform, "IS_WINDOWS", True):
            self.assertEqual(runner.requested_build_dir().name, "build-fast-clang")

    def test_busy_build_tree_fails_before_runner_work(self):
        lease = mock.Mock()
        lease.build_dir = Path("busy-build")
        lease.acquire.return_value = False
        lease.owner_text.return_value = '{"pid":42}'
        output = io.StringIO()
        with redirect_stdout(output), mock.patch.object(
                runner.buildlock, "BuildTreeLock", return_value=lease), \
                mock.patch.object(runner, "_run_main") as run:
            self.assertEqual(runner.main(["t.py", "t0"]), 1)
        run.assert_not_called()
        lease.release.assert_called_once()
        self.assertIn("BUILD TREE BUSY", output.getvalue())

    def test_windows_ctest_parallelism_is_capped_without_throttling_builds(self):
        with mock.patch.object(runner.platform, "IS_WINDOWS", True):
            self.assertEqual(runner.default_ctest_jobs(26), 8)
            self.assertEqual(runner.default_ctest_jobs(6), 6)
        with mock.patch.object(runner.platform, "IS_WINDOWS", False):
            self.assertEqual(runner.default_ctest_jobs(26), 26)

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

    def test_exhaustive_generator_is_reserved_for_t3(self):
        for tier in ("t0", "t1", "t2"):
            with self.subTest(tier=tier):
                _, exclude, not_covered = runner.TIERS[tier]
                self.assertRegex("xi_generator_self_test", exclude)
                self.assertNotRegex("xi_generator_fast_self_test", exclude)
                self.assertIn("exhaustive Xi generator mutations", not_covered)
        self.assertEqual(runner.TIERS["t3"][1], "")

    def test_script_only_profile_does_not_trigger_full_build(self):
        listed = SimpleNamespace(ok=True, stdout=b"phony: phony\n")
        with mock.patch.object(runner.proc, "run", return_value=listed) as run:
            self.assertTrue(runner.build_selected(
                Path("build"), ["test_tiered_test_runner"], 4,
                include_xray=False, allow_no_targets=True))
        run.assert_called_once_with(["ninja", "-C", "build",
                                     "-t", "targets", "all"])


class AutoRoutingTest(unittest.TestCase):
    def assert_route(self, expected, *paths):
        route, _ = runner.choose_run_for_paths(paths)
        self.assertEqual(route, expected)

    def test_owned_test_infrastructure_uses_script_only_profile(self):
        self.assert_route("infra", "scripts/t.py",
                          "tests/lib/tests/test_t_runner.py")

    def test_canonical_private_program_paths_use_exact_profile(self):
        self.assert_route("canonical", "src/program/xr_program_verify.c",
                          "src/aot/program/xr_backend_ir_verify.c",
                          "tests/unit/program/test_xr_program.c")

    def test_documentation_does_not_widen_an_owned_change(self):
        self.assert_route("canonical", "src/program/xr_program_verify.c",
                          "contracts/canonical-program-vm.md")

    def test_backend_and_general_source_changes_keep_broad_floors(self):
        self.assert_route("t2", "src/aot/xi_cgen.c")
        self.assert_route("t2", "CMakeLists.txt")
        self.assert_route("t1", "src/frontend/parser/xparse.c")

    def test_unknown_or_mixed_paths_conservatively_escalate(self):
        self.assert_route("t1", "new-root-tool.py")
        self.assert_route("t1", "scripts/t.py", "src/program/xr_program.c")

    def test_clean_and_documentation_only_changes_use_bounded_t0(self):
        self.assert_route("t0")
        self.assert_route("t0", "README.md", "contracts/notes.md")


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
                    self.assertEqual(ctest.call_args.args[0][-3:],
                                     ["-R", "^test_xr_program_source_build$", "--no-tests=error"])

    def test_failed_build_never_runs_ctest(self):
        with tempfile.TemporaryDirectory() as directory:
            result, output, ctest = self.run_focused(Path(directory), build_ok=False)
        self.assertEqual(result, 1)
        ctest.assert_not_called()
        self.assertNotIn("timing: ctest=", output)


class EmptySelectionTest(unittest.TestCase):
    def run_selection(self, inventories, *, tier="t0", extra=(), no_build=False,
                      ctest_code=0):
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            stack.enter_context(redirect_stdout(output))
            environment = {"XR_BUILD_DIR": directory}
            if no_build:
                environment["XR_NO_BUILD"] = "1"
            stack.enter_context(mock.patch.dict(runner.os.environ, environment, clear=True))
            stack.enter_context(mock.patch.object(runner.sanitizer,
                                                 "activate_windows_msvc_environment",
                                                 return_value=True))
            manifest = stack.enter_context(mock.patch.object(
                runner, "refresh_cmake_manifest", return_value=True))
            pending = iter(inventories)

            def names(*args):
                self.assertTrue(manifest.called, "selection must follow manifest refresh")
                return next(pending)

            stack.enter_context(mock.patch.object(runner, "ctest_names", side_effect=names))
            build = stack.enter_context(mock.patch.object(runner, "build_selected",
                                                         return_value=True))
            ctest = stack.enter_context(mock.patch.object(runner.subprocess, "call",
                                                         return_value=ctest_code))
            corpus = stack.enter_context(mock.patch.object(runner, "run_regression_corpus"))
            process = stack.enter_context(mock.patch.object(runner.proc, "run"))
            result = runner.main(["t.py", tier, *extra])
            manifest.assert_called_once()
            corpus.assert_not_called()
            process.assert_not_called()
        return result, output.getvalue(), build, ctest

    def test_empty_selection_fails_before_build_or_test_for_every_tier(self):
        for tier in (*runner.TIERS, "canonical"):
            with self.subTest(tier=tier):
                result, output, build, ctest = self.run_selection(
                    [[], ["test_exists"]], tier=tier, extra=["-R", "^nonexistent$"])
                self.assertEqual(result, 1)
                self.assertIn("0/1 tests", output)
                self.assertIn("TEST SELECTION FAILED", output)
                self.assertNotIn("PASS", output)
                build.assert_not_called()
                ctest.assert_not_called()

    def test_empty_inventory_and_no_build_mode_also_fail(self):
        result, output, build, ctest = self.run_selection([[], []], no_build=True)
        self.assertEqual(result, 1)
        self.assertIn("0/0 tests", output)
        self.assertNotIn("PASS", output)
        build.assert_not_called()
        ctest.assert_not_called()

    def test_empty_selection_after_build_never_rebuilds_or_runs_ctest(self):
        result, output, build, ctest = self.run_selection(
            [["test_exists"], ["test_exists"], []], tier="t1")
        self.assertEqual(result, 1)
        self.assertIn("no tests remain after the build refresh", output)
        self.assertNotIn("PASS", output)
        build.assert_called_once()
        ctest.assert_not_called()

    def test_execution_enforces_empty_selection_error_after_forwarded_options(self):
        for option in ([], ["--no-tests=error"], ["--no-tests=ignore"]):
            with self.subTest(option=option):
                extra = ["-R", "^test_exists$", *option]
                result, output, build, ctest = self.run_selection(
                    [["test_exists"], ["test_exists"], ["test_exists"]],
                    extra=extra, ctest_code=8)
                self.assertEqual(result, 8)
                self.assertIn("FAIL", output)
                self.assertNotIn("PASS", output)
                build.assert_called_once()
                self.assertEqual(ctest.call_args.args[0][-len(extra) - 1:],
                                 [*extra, "--no-tests=error"])

    def test_existing_no_tests_error_option_keeps_nonempty_success(self):
        result, output, build, ctest = self.run_selection(
            [["test_exists"], ["test_exists"], ["test_exists"]],
            extra=["-R", "^test_exists$", "--no-tests=error"])
        self.assertEqual(result, 0)
        self.assertIn("PASS", output)
        build.assert_called_once()
        ctest.assert_called_once()


if __name__ == "__main__":
    unittest.main()
