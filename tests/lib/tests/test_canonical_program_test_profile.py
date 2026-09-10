import importlib.util
import io
import json
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from _support import load_module


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location(
    "canonical_program_test_profile",
    ROOT / "scripts" / "canonical_program_test_profile.py",
)
assert SPEC and SPEC.loader
profile = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(profile)


class CanonicalProgramTestProfileTests(unittest.TestCase):
    def test_inventory_is_unique_and_build_targets_are_test_evidence(self) -> None:
        self.assertEqual(len(profile.CTEST_NAMES), 60)
        self.assertEqual(len(profile.BUILD_TARGETS), 36)
        self.assertEqual(len(profile.CTEST_NAMES), len(set(profile.CTEST_NAMES)))
        self.assertEqual(len(profile.BUILD_TARGETS), len(set(profile.BUILD_TARGETS)))
        self.assertLessEqual(
            set(profile.BUILD_TARGETS) - {"test_xi_pipeline", "xray"},
            set(profile.CTEST_NAMES),
        )
        self.assertIn("canonical_source_run_cli", profile.CTEST_NAMES)
        self.assertIn("xray", profile.BUILD_TARGETS)
        self.assertIn("test_xi_pipeline_canonical", profile.CTEST_NAMES)
        self.assertIn("test_xi_pipeline", profile.BUILD_TARGETS)
        self.assertIn("test_xr_program_vm_runtime", profile.BUILD_TARGETS)

    def test_generic_identity_is_an_exact_canonical_subset(self) -> None:
        self.assertEqual(len(profile.GENERIC_IDENTITY_CTEST_NAMES), 4)
        self.assertEqual(len(profile.GENERIC_IDENTITY_BUILD_TARGETS), 4)
        self.assertEqual(
            len(profile.GENERIC_IDENTITY_CTEST_NAMES),
            len(set(profile.GENERIC_IDENTITY_CTEST_NAMES)),
        )
        self.assertEqual(
            len(profile.GENERIC_IDENTITY_BUILD_TARGETS),
            len(set(profile.GENERIC_IDENTITY_BUILD_TARGETS)),
        )
        self.assertLessEqual(
            set(profile.GENERIC_IDENTITY_CTEST_NAMES), set(profile.CTEST_NAMES)
        )
        self.assertLessEqual(
            set(profile.GENERIC_IDENTITY_BUILD_TARGETS), set(profile.BUILD_TARGETS)
        )

    def test_manifest_additions_and_removals_change_both_inventories(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xr-canonical-profile-") as directory:
            manifest = Path(directory) / "cases.json"
            source = Path(directory) / "cases.c"
            payload = {"schema": 1, "cases": [{"name": "example", "fixture": None}]}
            manifest.write_text(json.dumps(payload), encoding="utf-8")
            source.write_text("TEST(example) {\n}\n", encoding="utf-8")
            without_fixture = profile.load_inventory(manifest, source)
            payload["cases"][0]["fixture"] = {"id": "example", "expected_exit": 7,
                                              "labels": ["coroutine"]}
            manifest.write_text(json.dumps(payload), encoding="utf-8")
            source.write_text("TEST(example) {\nXR_SOURCE_FIXTURE_EXAMPLE\n}\n", encoding="utf-8")
            with_fixture = profile.load_inventory(manifest, source)
            expected = "test_xr_program_example_aot_native"
            for old, new in zip(without_fixture, with_fixture):
                self.assertEqual(new, old + (expected,))
            payload["cases"][0]["fixture"] = None
            manifest.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaises(profile.source_fixtures.FixtureError):
                profile.load_inventory(manifest, source)
            source.write_text("TEST(example) {\n}\n", encoding="utf-8")
            self.assertEqual(profile.load_inventory(manifest, source), without_fixture)

    def test_native_registration_has_no_missing_or_shadow_target(self) -> None:
        registry = profile.source_fixtures.load_registry()
        _, cmake = profile.source_fixtures.project_registry(registry)
        registered = re.findall(rb"^add_xr_program_source_native_fixture\(\w+ (\w+) ",
                                cmake, re.MULTILINE)
        targets = tuple(target.decode("ascii") for target in registered)
        self.assertEqual(targets, profile.source_fixtures.native_target_names(registry))
        for target in targets:
            self.assertEqual(profile.CTEST_NAMES.count(target), 1)
            self.assertEqual(profile.BUILD_TARGETS.count(target), 1)

    def test_missing_native_target_stops_runner_before_any_build(self) -> None:
        runner = load_module("canonical_fixture_runner_under_test", ROOT / "scripts/t.py")
        registry = profile.source_fixtures.load_registry()
        with tempfile.TemporaryDirectory(prefix="xr-canonical-target-refusal-") as directory:
            for missing in profile.source_fixtures.native_target_names(registry):
                available = "\n".join(f"{target}: phony" for target in profile.BUILD_TARGETS
                                      if target != missing)
                listed = SimpleNamespace(ok=True, stdout=available.encode("utf-8"))
                output = io.StringIO()
                with redirect_stdout(output), mock.patch.object(
                        runner.proc, "run", return_value=listed) as execute:
                    self.assertFalse(runner.build_selected(
                        Path(directory), profile.CTEST_NAMES, 1, include_xray=False,
                        required_targets=profile.BUILD_TARGETS))
                self.assertEqual(execute.call_count, 1)
                self.assertEqual(execute.call_args.args[0][0], "ninja")
                self.assertIn(missing, output.getvalue())

    def test_each_cross_module_call_has_independent_native_qualification(self) -> None:
        manifest = json.loads((ROOT / "tests/unit/program/xr_program_source_cases.json")
                              .read_text(encoding="utf-8"))
        cases = {case["name"]: case["fixture"] for case in manifest["cases"]}
        for case_name in (
            "source_owner_cross_module_coroutine_call_has_one_program_and_private_executors",
            "source_owner_cross_module_static_method_coroutine_has_one_program_and_private_executors",
        ):
            fixture = cases[case_name]
            target = f"test_xr_program_{fixture['id']}_aot_native"
            self.assertEqual(fixture["expected_exit"], 7)
            self.assertIn(target, profile.CTEST_NAMES)
            self.assertIn(target, profile.BUILD_TARGETS)

    def test_regex_is_exact(self) -> None:
        pattern = re.compile(profile.ctest_regex())
        for name in profile.CTEST_NAMES:
            self.assertIsNotNone(pattern.fullmatch(name))
            self.assertIsNone(pattern.fullmatch(name + "_shadow"))

    def test_ctest_listing_parser_ignores_noise(self) -> None:
        output = """Test project C:/work/build
          Test #12: alpha
        missing executable warning
          Test #104: beta
        Total Tests: 2
        """
        self.assertEqual(profile.listed_ctest_names(output), ("alpha", "beta"))

    def test_execution_report_reads_exact_testcase_names(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xr-canonical-junit-") as directory:
            report = Path(directory) / "ctest.xml"
            report.write_text(
                "<testsuite tests='2'>"
                "<testcase name='alpha' status='run'/>"
                "<testcase name='beta'><failure/></testcase>"
                "</testsuite>",
                encoding="utf-8",
            )
            self.assertEqual(profile.executed_ctest_names(report), ("alpha", "beta"))

    def test_execution_report_rejects_missing_malformed_and_unnamed_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xr-canonical-junit-") as directory:
            report = Path(directory) / "ctest.xml"
            with self.assertRaisesRegex(profile.source_fixtures.FixtureError, "missing"):
                profile.executed_ctest_names(report)
            report.write_text("<testsuite>", encoding="utf-8")
            with self.assertRaisesRegex(profile.source_fixtures.FixtureError, "malformed"):
                profile.executed_ctest_names(report)
            report.write_text("<testsuite><testcase/></testsuite>", encoding="utf-8")
            with self.assertRaisesRegex(profile.source_fixtures.FixtureError, "unnamed"):
                profile.executed_ctest_names(report)

    def test_execution_report_rejects_skipped_and_duplicate_tests(self) -> None:
        reports = {
            "non-executed": (
                "<testsuite><testcase name='alpha' status='notrun'/></testsuite>"
            ),
            "duplicate": (
                "<testsuite><testcase name='alpha'/><testcase name='alpha'/></testsuite>"
            ),
        }
        with tempfile.TemporaryDirectory(prefix="xr-canonical-junit-") as directory:
            report = Path(directory) / "ctest.xml"
            for message, contents in reports.items():
                with self.subTest(message=message):
                    report.write_text(contents, encoding="utf-8")
                    with self.assertRaisesRegex(profile.source_fixtures.FixtureError,
                                                message):
                        profile.executed_ctest_names(report)

    def test_preflight_covers_all_canonical_boundaries(self) -> None:
        required = {
            "test_core_spec",
            "test_xr_program",
            "test_xr_program_verify",
            "test_xr_program_source_build",
            "test_xr_program_vm",
            "test_xr_program_vm_runtime",
            "test_xr_program_aot",
            "canonical_source_run_cli",
            "test_xglobal_summary",
            "test_xi_pipeline_canonical",
            "test_xr_program_cross_module_coroutine_aot_native",
            "test_xr_program_cross_module_static_method_coroutine_aot_native",
            "test_xr_program_multi_safepoint_aot_native",
            "test_xr_program_ref_parameter_coroutine_aot_native",
            "test_xr_program_read_existential_coroutine_aot_native",
            "test_xr_program_provider_trap_cleanup_aot_native",
            "test_xr_program_child_coroutine_trap_cleanup_aot_native",
            "meta_ownership_inventory",
            "contract_freeze",
            "contract_freeze_injection",
        }
        self.assertLessEqual(required, set(profile.CTEST_NAMES))


if __name__ == "__main__":
    unittest.main()
