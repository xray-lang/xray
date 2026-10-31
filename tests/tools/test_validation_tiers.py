"""Keep moved gates executable and preserve their build prerequisites."""
from copy import deepcopy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import xray_l1


class TierTests(unittest.TestCase):
    def test_all_original_tests_and_targets_are_still_registered(self):
        catalog = xray_l1.tier_catalog(ROOT)
        baseline = json.loads(subprocess.check_output([
            "git", "-C", str(ROOT), "show",
            "6d8e663ac084ba57ccaa39d89673a80216295843:scripts/l1-v0.json"]))
        self.assertEqual((len(baseline["tests"]), len(baseline["targets"])), (340, 177))
        self.assertTrue(set(baseline["tests"]) <= catalog["homes"].keys())
        self.assertTrue(set(baseline["targets"]) <= catalog["targets"])
        for level in ("L0", "L1", "L2", "L3"):
            self.assertIn(level, catalog["homes"].values())

    def test_l1_core_moves_tests_and_build_targets_together(self):
        plan = xray_l1.tier_plan(ROOT, "L1")
        self.assertIn("smoke", plan["tests"])
        self.assertIn("meta_ownership_inventory", plan["tests"])
        self.assertNotIn("local_merge_queue", plan["tests"])
        self.assertNotIn("test_xir_specialize_control", plan["tests"])
        self.assertTrue({"test_lir_carriers", "test_xir_records", "test_hir_body",
                         "source-sdk-public-consumers", "native-runtime-bundle"}.isdisjoint(plan["targets"]))

    def test_tooling_is_related_l0_and_nightly_contains_complete_moved_families(self):
        tools = xray_l1.tier_plan(ROOT, "L0", changed=["scripts/merge_slice.py"])
        self.assertIn("local_merge_queue", tools["tests"])
        self.assertNotIn("smoke", tools["tests"])
        inventory = xray_l1.tier_plan(ROOT, "L0", changed=["stdlib/types/array.xr"])
        self.assertIn("test_api_inventory_native_declarations", inventory["tests"])
        night = xray_l1.tier_plan(ROOT, "L3")
        self.assertIn("test_xir_specialize_control", night["tests"])
        self.assertIn("test_lir_owned_string", night["tests"])
        self.assertIn("test_lir_carriers", night["targets"])
        self.assertIn("test_xir_compile_emit_owner", night["targets"])
        self.assertIn("source_sdk_installed", night["tests"])
        self.assertIn("local_merge_queue", night["tests"])

    def test_l2_has_real_installed_consumers_and_sensitive_owner_receivers(self):
        plan = xray_l1.tier_plan(ROOT, "L2", ["public-values"],
                                changed=["src/xir/xxir_checked.h"], sensitive=["owner"])
        self.assertTrue({"source_sdk_installed", "sdk_installed_native_msvc",
                         "source_packet_writer_control", "source_product_facts_control"} <= set(plan["tests"]))
        self.assertTrue({"source-sdk-bundle", "test_xir_runtime_sdk", "test_source_request_lifetime"} <= set(plan["targets"]))
        self.assertNotIn("native_runtime_installed", plan["tests"])
        self.assertEqual(len(plan["tests"]), len(set(plan["tests"])))

    def test_source_sdk_fixture_adds_only_its_registered_test_and_target(self):
        plan = xray_l1.tier_plan(ROOT, "L2", ["public-values", "source-sdk", "checked-sdk"],
                                sensitive=["owner"])
        self.assertEqual(len(plan["tests"]), 63)
        self.assertEqual(plan["tests"].count("hir_body"), 1)
        self.assertEqual(plan["targets"].count("test_hir_body"), 1)
        self.assertNotIn("frontend", plan["groups"])
        self.assertNotIn("hir_body_cli", plan["tests"])
        catalog = xray_l1.tier_catalog(ROOT)
        self.assertIn("hir_body", catalog["groups"]["frontend"]["tests"])
        self.assertNotIn("hir_body", catalog["groups"]["source-sdk"]["tests"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_recursive_fixture_selection_matches_actual_ctest_report(self):
        catalog = {"groups": {"family": {"tier": "L2", "tests": ["leaf"], "targets": []}},
                   "l2": {"interfaceMatches": {}, "requiredOn": {}, "prerequisites": {
                       "leaf": {"tests": ["middle_setup"], "targets": ["middle_target"]},
                       "middle_setup": {"tests": ["root_setup"], "targets": ["root_target"]}}}}
        with mock.patch.object(xray_l1, "tier_catalog", return_value=catalog):
            plan = xray_l1.tier_plan(ROOT, "L2", ["family"])
        self.assertEqual(plan["tests"], ["leaf", "middle_setup", "root_setup"])
        self.assertEqual(plan["targets"], ["xray", "middle_target", "root_target"])
        with tempfile.TemporaryDirectory(prefix="xray-tier-fixture-") as directory:
            source = Path(directory)
            build, logs = source / "build", source / "logs"
            logs.mkdir()
            (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.25)
project(TierFixture NONE)
enable_testing()
add_test(NAME root_setup COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/root")
set_tests_properties(root_setup PROPERTIES FIXTURES_SETUP root)
add_test(NAME middle_setup COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/root" "${CMAKE_BINARY_DIR}/middle")
set_tests_properties(middle_setup PROPERTIES FIXTURES_REQUIRED root FIXTURES_SETUP middle)
add_test(NAME leaf COMMAND "${CMAKE_COMMAND}" -E compare_files "${CMAKE_BINARY_DIR}/root" "${CMAKE_BINARY_DIR}/middle")
set_tests_properties(leaf PROPERTIES FIXTURES_REQUIRED middle)
add_test(NAME unrelated COMMAND "${CMAKE_COMMAND}" -E false)
''', encoding="utf-8")
            xray_l1.command(["cmake", "-S", source, "-B", build, "-G", "Ninja"],
                            source, os.environ.copy(), logs / "configure.log")
            xray_l1.tests(plan["tests"], build, logs, "fixtures", os.environ.copy())
            cases = list(ET.parse(logs / "fixtures.xml").getroot().iter("testcase"))
            self.assertEqual(sorted(case.get("name") for case in cases),
                             ["leaf", "middle_setup", "root_setup"])

    def test_unknown_fixture_test_or_target_rejects_before_execution(self):
        config = json.loads((ROOT / "scripts/validation-tiers.json").read_text())
        baseline = subprocess.check_output(["git", "-C", str(ROOT), "show",
                                            config["baselineCommit"] + ":scripts/l1-v0.json"])
        for key in ("tests", "targets"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "scripts").mkdir()
                for path in ["scripts/validation-tiers.json", *config["levels"].values()]:
                    (root / path).write_bytes((ROOT / path).read_bytes())
                path = root / "scripts/l2-capability.json"
                value = json.loads(path.read_text())
                value["prerequisites"]["source_body_sdk"][key].append("unknown-fixture")
                path.write_text(json.dumps(value))
                with mock.patch.object(xray_l1.subprocess, "check_output", return_value=baseline):
                    with self.assertRaisesRegex(ValueError, "validation prerequisite"):
                        xray_l1.tier_catalog(root)

    def test_unknown_or_empty_selection_is_a_failure(self):
        for args in (("L2", ["not-a-capability"]), ("L0", []), ("L2", ["governance"]),
                     ("L4", ["governance"])):
            with self.subTest(args=args), self.assertRaises(ValueError):
                xray_l1.tier_plan(ROOT, *args)
        with self.assertRaises(ValueError):
            xray_l1.tier_plan(ROOT, "L2", ["public-values"], changed=["../foreign"])

    def test_removal_or_duplicate_home_fails_before_execution(self):
        config = json.loads((ROOT / "scripts/validation-tiers.json").read_text())
        baseline = subprocess.check_output(["git", "-C", str(ROOT), "show",
                                            config["baselineCommit"] + ":scripts/l1-v0.json"])
        for mutation in ("test", "target", "duplicate", "baseline"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "scripts").mkdir()
                for path in ["scripts/validation-tiers.json", *config["levels"].values()]:
                    (root / path).write_bytes((ROOT / path).read_bytes())
                path = root / "scripts/l3-nightly.json"
                data = json.loads(path.read_text())
                if mutation == "test":
                    data["groups"]["frozen-backend"]["tests"].remove("test_lir_owned_string")
                elif mutation == "target":
                    data["groups"]["frozen-backend"]["targets"].remove("test_lir_carriers")
                elif mutation == "duplicate":
                    data["groups"]["frozen-backend"]["tests"].append("smoke")
                else:
                    registry = deepcopy(config)
                    registry["baselineCommit"] = "HEAD"
                    (root / "scripts/validation-tiers.json").write_text(json.dumps(registry))
                path.write_text(json.dumps(data))
                with mock.patch.object(xray_l1.subprocess, "check_output", return_value=baseline):
                    with self.assertRaises(ValueError):
                        xray_l1.tier_catalog(root)


if __name__ == "__main__":
    unittest.main()
