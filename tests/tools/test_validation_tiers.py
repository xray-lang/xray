"""Keep moved gates executable and preserve their build prerequisites."""
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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
