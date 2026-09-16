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

    def test_generic_identity_profile_reuses_the_shared_exact_inventory(self):
        profile = runner.EXACT_PROFILES["generic-identity"]
        self.assertIs(profile.tests, runner.canonical_profile.GENERIC_IDENTITY_CTEST_NAMES)
        self.assertIs(
            profile.targets, runner.canonical_profile.GENERIC_IDENTITY_BUILD_TARGETS
        )
        self.assertFalse(profile.include_xray)

    def test_h2_reference_profile_reuses_the_shared_exact_inventory(self):
        profile = runner.EXACT_PROFILES["h2-reference"]
        self.assertIs(profile.tests, runner.canonical_profile.H2_REFERENCE_CTEST_NAMES)
        self.assertIs(profile.targets, runner.canonical_profile.H2_REFERENCE_BUILD_TARGETS)
        self.assertFalse(profile.include_xray)
        self.assertIn("VM", profile.not_covered)

    def test_h2_aggregate_reuses_the_shared_exact_inventory(self):
        profile = runner.EXACT_PROFILES["h2"]
        self.assertIs(profile.tests, runner.canonical_profile.H2_CTEST_NAMES)
        self.assertIs(profile.targets, runner.canonical_profile.H2_BUILD_TARGETS)
        for required in ("test_xr_program_provider_requirements", "test_provider_logical_admission"):
            self.assertIn(required, profile.tests)
            self.assertIn(required, profile.targets)
        self.assertFalse(profile.include_xray)
        self.assertIn("canonical", profile.not_covered)
        self.assertIn("t2", profile.not_covered)
        self.assertIn("ASan/LSan", profile.not_covered)

    def test_h2_private_profiles_reuse_the_shared_exact_inventories(self):
        for name, tests, targets in (
            ("h2-source", runner.canonical_profile.H2_SOURCE_CTEST_NAMES,
             runner.canonical_profile.H2_SOURCE_BUILD_TARGETS),
            ("h2-vm", runner.canonical_profile.H2_VM_CTEST_NAMES,
             runner.canonical_profile.H2_VM_BUILD_TARGETS),
            ("h2-aot", runner.canonical_profile.H2_AOT_CTEST_NAMES,
             runner.canonical_profile.H2_AOT_BUILD_TARGETS),
        ):
            with self.subTest(name=name):
                profile = runner.EXACT_PROFILES[name]
                self.assertIs(profile.tests, tests)
                self.assertIs(profile.targets, targets)
                self.assertFalse(profile.include_xray)

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

    def test_complete_t3_deduplicates_fast_but_focused_t3_keeps_it(self):
        _, complete_exclude, _ = runner.tier_ctest_filters("t3", False)
        self.assertRegex(runner.XI_GENERATOR_FAST, complete_exclude)
        self.assertNotRegex(runner.SLOW_EXHAUSTIVE, complete_exclude)
        _, focused_exclude, _ = runner.tier_ctest_filters("t3", True)
        self.assertEqual(focused_exclude, "")

    def test_script_only_profile_does_not_trigger_full_build(self):
        listed = SimpleNamespace(ok=True, stdout=b"phony: phony\n")
        with mock.patch.object(runner.proc, "run", return_value=listed) as run:
            self.assertTrue(runner.build_selected(
                Path("build"), ["test_tiered_test_runner"], 4,
                include_xray=False, allow_no_targets=True))
        run.assert_called_once_with(["ninja", "-C", "build",
                                     "-t", "targets", "all"])

    def test_required_executable_target_uses_real_ninja_inventory(self):
        with tempfile.TemporaryDirectory(prefix="xray ninja targets ") as directory:
            source = Path(directory)
            build = source / "build"
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(target_inventory C)\n"
                "add_executable(xray main.c)\n", encoding="utf-8")
            (source / "main.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
            configured = runner.proc.run(["cmake", "-G", "Ninja", "-S", source, "-B", build])
            self.assertTrue(configured.ok, configured.combined_text())
            self.assertTrue(runner.build_selected(
                build, [], 1, include_xray=False, required_targets=("xray",)))
            executed = runner.proc.run([build / runner.platform.exe_name("xray")])
            self.assertTrue(executed.ok, executed.combined_text())
            self.assertFalse(runner.build_selected(
                build, [], 1, include_xray=False, required_targets=("missing_target",)))


class ProductionBuildPreflightTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xr-production-cache-")
        self.build = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def write_cache(self, overrides=None, *, omitted=(), duplicate=None):
        values = {
            "CMAKE_HOME_DIRECTORY": str(runner.REPO_ROOT.resolve()),
            **runner.PRODUCTION_CACHE_VALUES,
        }
        values.update(overrides or {})
        lines = [f"{name}:STRING={value}" for name, value in values.items()
                 if name not in omitted]
        if duplicate is not None:
            lines.append(f"{duplicate}:STRING={values[duplicate]}")
        (self.build / "CMakeCache.txt").write_text(
            "\n".join(lines) + "\n", encoding="utf-8")

    def test_exact_production_configuration_is_accepted(self):
        self.write_cache()
        self.assertEqual(runner.production_build_identity_errors(self.build), ())

    def test_missing_and_duplicate_cache_entries_fail_closed(self):
        self.assertIn("cannot read", " ".join(
            runner.production_build_identity_errors(self.build)))
        for name in ("CMAKE_HOME_DIRECTORY", *runner.PRODUCTION_CACHE_VALUES):
            with self.subTest(missing=name):
                self.write_cache(omitted=(name,))
                self.assertIn(f"missing CMake cache entry: {name}",
                              runner.production_build_identity_errors(self.build))
        self.write_cache(duplicate="XR_STDLIB_FROM_FILE")
        self.assertIn("duplicate CMake cache entry: XR_STDLIB_FROM_FILE",
                      runner.production_build_identity_errors(self.build))

    def test_every_nonproduction_identity_dimension_is_rejected(self):
        cases = {
            "source root": {"CMAKE_HOME_DIRECTORY": str(self.build / "other")},
            "relative source root": {"CMAKE_HOME_DIRECTORY": "."},
            "generator": {"CMAKE_GENERATOR": "Visual Studio 17 2022"},
            "build type": {"CMAKE_BUILD_TYPE": "Debug"},
            "source stdlib": {"XR_STDLIB_FROM_FILE": "ON"},
            "missing fastpaths": {"XRAY_STDLIB_VM_FASTPATHS": "OFF"},
            "ASan": {"ENABLE_ASAN": "ON"},
            "UBSan": {"ENABLE_UBSAN": "ON"},
            "TSan": {"ENABLE_TSAN": "ON"},
            "MSan": {"ENABLE_MSAN": "ON"},
        }
        for label, overrides in cases.items():
            with self.subTest(label=label):
                self.write_cache(overrides)
                self.assertTrue(runner.production_build_identity_errors(self.build))

    def test_t2_and_t3_refuse_before_manifest_build_or_ctest(self):
        for tier in ("t2", "t3"):
            with self.subTest(tier=tier), ExitStack() as stack:
                stack.enter_context(mock.patch.dict(
                    runner.os.environ, {"XR_BUILD_DIR": str(self.build)}, clear=True))
                stack.enter_context(mock.patch.object(
                    runner.sanitizer, "activate_windows_msvc_environment",
                    return_value=True))
                stack.enter_context(mock.patch.object(
                    runner, "validate_production_build", return_value=False))
                refresh = stack.enter_context(mock.patch.object(
                    runner, "refresh_cmake_manifest"))
                build = stack.enter_context(mock.patch.object(runner, "build_selected"))
                ctest = stack.enter_context(mock.patch.object(runner, "ctest_names"))
                execute = stack.enter_context(mock.patch.object(runner.subprocess, "call"))
                self.assertEqual(runner._run_main(["t.py", tier]), 1)
                refresh.assert_not_called()
                build.assert_not_called()
                ctest.assert_not_called()
                execute.assert_not_called()

    def test_canonical_and_product_tiers_reject_fast_configuration(self):
        for tier in ("canonical", "t2", "t3"):
            with self.subTest(tier=tier), ExitStack() as stack:
                stack.enter_context(mock.patch.dict(
                    runner.os.environ, {"XR_FAST": "1"}, clear=True))
                stack.enter_context(mock.patch.object(
                    runner.sanitizer, "activate_windows_msvc_environment",
                    return_value=True))
                refresh = stack.enter_context(mock.patch.object(
                    runner, "refresh_cmake_manifest"))
                build = stack.enter_context(mock.patch.object(runner, "build_selected"))
                execute = stack.enter_context(mock.patch.object(runner.subprocess, "call"))
                self.assertEqual(runner._run_main(["t.py", tier]), 1)
                refresh.assert_not_called()
                build.assert_not_called()
                execute.assert_not_called()


class AutoRoutingTest(unittest.TestCase):
    def assert_route(self, expected, *paths):
        route, _ = runner.choose_run_for_paths(paths)
        self.assertEqual(route, expected)

    def test_owned_test_infrastructure_uses_script_only_profile(self):
        self.assert_route("infra", "scripts/t.py",
                          "tests/lib/tests/test_t_runner.py")

    def test_pure_h2_program_vm_aot_verifier_and_source_paths_use_aggregate(self):
        paths = (
            "src/program/xr_program_encode.c",
            "src/vm/xr_program_vm.c",
            "src/aot/program/xr_backend_ir_verify.c",
            "src/program/xr_program_verify.c",
            "src/program/xr_program_source_build.c",
        )
        for path in paths:
            with self.subTest(path=path):
                self.assert_route("h2", path)
        self.assert_route("h2", *paths)

    def test_h2_owned_tests_and_contract_checkers_use_aggregate(self):
        self.assert_route(
            "h2",
            "tests/unit/core/test_core_spec.c",
            "tests/unit/program/test_xr_program_source_build.c",
            "tests/unit/program/xr_program_panic_fixture.h",
            "tests/unit/vm/test_xr_program_vm_runtime.c",
            "tests/unit/aot/test_xr_program_aot_class.inc.c",
            "scripts/check_xr_program_h2_backend_differential.py",
            "tests/lib/tests/test_h2_backend_differential.py",
        )

    def test_canonical_only_program_governance_keeps_exact_profile(self):
        self.assert_route("canonical", "scripts/program_source_fixtures.py",
                          "tests/unit/program/xr_program_source_cases.json")

    def test_documentation_does_not_widen_an_owned_change(self):
        self.assert_route("h2", "src/program/xr_program_verify.c",
                          "contracts/canonical-program-vm.md")

    def test_backend_and_general_source_changes_keep_broad_floors(self):
        self.assert_route("t2", "src/aot/xi_cgen.c")
        self.assert_route("t2", "CMakeLists.txt")
        self.assert_route("t1", "src/frontend/parser/xparse.c")

    def test_h2_mixed_with_canonical_only_paths_returns_to_canonical(self):
        self.assert_route("canonical", "src/program/xr_program_verify.c",
                          "scripts/program_source_fixtures.py")

    def test_h2_mixed_with_cross_domain_or_unknown_paths_escalates_to_t2(self):
        self.assert_route("t2", "src/program/xr_program_verify.c",
                          "src/frontend/parser/xparse.c")
        self.assert_route("t2", "src/vm/xr_program_vm.c", "new-root-tool.py")
        self.assert_route("t2", "src/aot/program/xr_backend_ir.c",
                          "src/aot/xi_cgen.c")
        self.assert_route("t2", "src/program/xr_program_source_build.c",
                          "xisa/core/registry.json")

    def test_unknown_or_non_h2_mixed_paths_keep_existing_conservative_floor(self):
        self.assert_route("t1", "new-root-tool.py")
        self.assert_route("t1", "scripts/t.py", "src/frontend/parser/xparse.c")

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


class ExactExecutionInventoryTest(unittest.TestCase):
    @staticmethod
    def write_report(command, names, *, skipped=()):
        report = Path(command[command.index("--output-junit") + 1])
        cases = "".join(
            f"<testcase name='{name}'/>" for name in names
        ) + "".join(
            f"<testcase name='{name}'><skipped/></testcase>" for name in skipped
        )
        report.write_text(f"<testsuite>{cases}</testsuite>", encoding="utf-8")

    def run_exact(self, expected, actual, *, skipped=(), returncode=0):
        output = io.StringIO()

        def execute(command, **_kwargs):
            self.write_report(command, actual, skipped=skipped)
            return returncode

        with redirect_stdout(output), mock.patch.object(
                runner.subprocess, "call", side_effect=execute) as call:
            code = runner.run_ctest(Path("build"), ["-R", "exact"], {}, expected)
        return code, output.getvalue(), call

    def test_exact_run_accepts_only_the_complete_execution_set(self):
        code, output, call = self.run_exact(("alpha", "beta"), ("beta", "alpha"))
        self.assertEqual(code, 0)
        self.assertIn("exact executed CTest names (2)", output)
        self.assertIn("--output-junit", call.call_args.args[0])

    def test_equal_count_with_one_missing_and_one_unexpected_fails(self):
        code, output, _ = self.run_exact(("alpha", "beta"), ("alpha", "gamma"))
        self.assertEqual(code, 1)
        self.assertIn("missing: beta", output)
        self.assertIn("unexpected: gamma", output)

    def test_skipped_test_and_duplicate_fail_closed(self):
        for actual, skipped, message in (
            (("alpha",), ("beta",), "non-executed tests"),
            (("alpha", "alpha"), (), "duplicate tests"),
        ):
            with self.subTest(message=message):
                code, output, _ = self.run_exact(("alpha", "beta"), actual,
                                                 skipped=skipped)
                self.assertEqual(code, 1)
                self.assertIn(message, output)

    def test_nonexact_run_does_not_claim_junit_output(self):
        with mock.patch.object(runner.subprocess, "call", return_value=8) as call:
            self.assertEqual(runner.run_ctest(Path("build"), ["-R", "focus"], {}), 8)
        self.assertNotIn("--output-junit", call.call_args.args[0])

    def test_exact_profile_rejects_forwarded_junit_destination(self):
        self.assertTrue(runner.has_ctest_option(["--output-junit", "mine.xml"],
                                                "--output-junit"))
        self.assertTrue(runner.has_ctest_option(["--output-junit=mine.xml"],
                                                "--output-junit"))


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
            stack.enter_context(mock.patch.object(runner, "validate_production_build",
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
        for tier in (*runner.TIERS, *runner.EXACT_PROFILES):
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
