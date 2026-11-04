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
        catalog = xray_l1.tier_catalog(ROOT)
        expected = {test for name in ("public-values", "source-sdk", "checked-sdk")
                    for test in catalog["groups"][name]["tests"]}
        expected.update(catalog["l2"]["requiredOn"]["owner"]["tests"])
        self.assertEqual(set(plan["tests"]), expected | {"hir_body", "runtime_sdk_recipe_incremental"})
        self.assertEqual(len(plan["tests"]), len(expected) + 2)
        self.assertEqual(plan["tests"].count("runtime_sdk_recipe_incremental"), 1)
        self.assertEqual(plan["tests"].count("hir_body"), 1)
        self.assertEqual(plan["targets"].count("test_hir_body"), 1)
        self.assertNotIn("frontend", plan["groups"])
        self.assertNotIn("hir_body_cli", plan["tests"])
        self.assertIn("hir_body", catalog["groups"]["frontend"]["tests"])
        self.assertNotIn("hir_body", catalog["groups"]["source-sdk"]["tests"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_embedded_stdlib_source_selects_source_sdk_without_unrelated_families(self):
        plan = xray_l1.tier_plan(ROOT, "L2", changed=["stdlib/xml/xml.xr"])
        self.assertEqual(plan["groups"], ["source-sdk"])
        self.assertIn("source_sdk_installed", plan["tests"])
        self.assertNotIn("sdk_installed_native_msvc", plan["tests"])
        self.assertNotIn("native_runtime_installed", plan["tests"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_compression_sources_and_oracles_select_their_runtime_gates(self):
        paths = (
            "stdlib/compress/compress.xr",
            "stdlib/types/array.xr",
            "src/xir/xxir_byte_array_value.inc.c",
            "src/xir/xxir_emit_byte_array.inc.c",
            "src/xir/xxir_vm_byte_array.inc.c",
            "tests/unit/xir/test_xir_byte_array_value.c",
            "tests/unit/xir/test_xir_compress_suite.c",
            "tests/unit/xir/compress_suite_runtime.h",
            "tests/unit/xir/run_compress_suite.py",
            "tests/unit/xir/slice_u8_byte_bulk_cases.h",
            "tests/diff/cases/semantics/stdlib/compress_external_codec_direct.xr.expected",
            "tests/regression/10_stdlib/1301_compress.xr",
            "tests/stdlib/contracts/compress/diff_cases.txt",
        )
        for path in paths:
            with self.subTest(path=path):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=[path])
                self.assertIn("public-values", plan["groups"])
                self.assertTrue({"test_xir_byte_array_value", "xir_compress_suite",
                                 "cli_compress_consumers"} <= set(plan["tests"]))
                self.assertIn("test_xir_compress_suite_native", plan["targets"])
                self.assertNotIn("native-runtime", plan["groups"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_generated_api_knowledge_selects_its_existing_sync_gates(self):
        for path in ("src/app/mcp/xmcp_knowledge_generated.c",
                     "src/app/lsp/xlsp_stdlib_generated.inc", "scripts/gen_stdlib_types.py",
                     "stdlib/types/array.xr",
                     "stdlib/compress/compress.xr", "scripts/gen_language_docs.py",
                     "scripts/gen_api_inventory.py", "spec/source/cards/stdlib/compress.json"):
            with self.subTest(path=path):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=[path])
                self.assertIn("api-knowledge", plan["groups"])
                self.assertTrue({"stdlib_metadata_sync", "stdlib_analyzer_builtins_sync",
                                 "stdlib_lsp_stdlib_sync", "stdlib_def_source_gate",
                                 "api_inventory_docs_coverage", "test_mcp_knowledge_generation"}
                                <= set(plan["tests"]))
                self.assertNotIn("native-runtime", plan["groups"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_installed_source_sdk_scripts_select_their_installation_family(self):
        for script in ("source_sdk_install.py", "source_sdk_owned.py"):
            with self.subTest(script=script):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=["tests/unit/xir/" + script])
                self.assertIn("source-sdk", plan["groups"])
                self.assertIn("source_sdk_installed", plan["tests"])
                self.assertIn("source_sdk_owned_installed", plan["tests"])
                self.assertNotIn("native-runtime", plan["groups"])

    def test_host_os_changes_select_paired_source_and_native_consumers(self):
        for path in ("stdlib/sys/sys.xr", "src/os/win/fs_mutation_win.c", "src/os/unix/fs_mutation_unix.c",
                     "src/os/os_system.h", "src/os/win/system_win.c", "src/os/unix/system_unix.c",
                     "src/os/os_signal.h", "src/os/win/signal_win.c",
                     "src/os/unix/signal_unix.c", "tests/unit/base/test_signal.c",
                     "src/os/os_file_mutation.h", "src/os/win/file_mutation_win.c",
                     "src/os/unix/file_mutation_unix.c", "tests/unit/base/test_file_mutation.c",
                     "src/os/os_file_metadata.h", "src/os/file_metadata_path.inc.h",
                     "src/os/win/file_metadata_win.c", "src/os/unix/file_metadata_unix.c",
                     "tests/unit/base/test_file_metadata.c",
                     "src/xir/xxir_instance_system.inc.c", "tests/unit/base/test_system_queries.c",
                     "src/xir/xxir_instance_sys_tokens.inc.c",
                     "src/xir/xxir_os_system_native.h",
                     "src/xir/xxir_file_stat_native.h", "src/xir/xxir_source_file_stat.inc.c",
                     "src/xir/xxir_instance_file_metadata.inc.c", "src/xir/xxir_nominal.inc.c",
                     "src/xir/xxir_nominal_native.inc.c", "src/xir/xxir_nominal_access.inc.c",
                     "src/xir/xxir_source_nominal_contract.inc.c", "src/xir/xxir_path_verify.inc.c",
                     "src/xir/xxir_source.c", "src/xir/xxir_source_struct.inc.c",
                     "src/xir/xxir_source_implements.inc.c",
                     "src/xir/xxir_source_methods.inc.c",
                     "src/hir/xhir_body_nominal.inc.c",
                     "src/os/os_fs.h", "src/base/xwindows_utf8.h",
                     "src/os/os_thread_token.h", "src/os/win/thread_token_win.c",
                     "src/os/unix/thread_token_unix.c", "tests/unit/base/test_thread_token.c",
                     "tests/unit/base/thread_token.cmake",
                     "src/xir/xxir_instance_io_mutation.inc.c",
                     "src/xir/xxir_call_signal_frontier.inc.c",
                     "src/xir/xxir_instance_signal_frontier.inc.c",
                     "src/xir/xxir_signal_frontier_internal.h",
                     "src/xir/xxir_instance_io.inc.c", "src/xir/xxir_source_io.inc.c",
                     "src/xir/xxir_path_prelude.h", "src/xir/xxir_native_callable.h",
                     "src/xir/xxir_declarations.c", "src/xir/xxir_instance.c",
                     "src/xir/xxir_ctfe_native_callable.inc.c",
                     "tests/unit/xir/host_os/host_os_path_cases.h"):
            with self.subTest(path=path):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=[path])
                self.assertIn("host-os", plan["groups"])
                self.assertTrue({"host_os_source", "host_os_native"} <= set(plan["tests"]))
                self.assertTrue({"test_host_os_source", "test_host_os_native"} <= set(plan["targets"]))
                self.assertIn("os_system_queries", plan["tests"])
                self.assertIn("test_os_system_queries", plan["targets"])
                self.assertIn("os_file_metadata", plan["tests"])
                self.assertIn("test_os_file_metadata", plan["targets"])
                self.assertIn("os_file_mutation", plan["tests"])
                self.assertIn("test_os_file_mutation", plan["targets"])
                self.assertIn("os_thread_token", plan["tests"])
                self.assertIn("test_os_thread_token", plan["targets"])
                self.assertIn("os_signal", plan["tests"])
                self.assertIn("test_os_signal", plan["targets"])
                self.assertNotIn("native-runtime", plan["groups"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_pipe_product_and_test_leaves_select_both_carriers(self):
        for path in ("tests/unit/xir/pipe_runtime/test_pipe_source.c",
                     "tests/unit/xir/pipe_runtime/test_pipe_native.c",
                     "tests/unit/xir/pipe_runtime/pipe_cases.h",
                     "tests/unit/xir/pipe_runtime/pipe_runtime.h",
                     "tests/unit/xir/pipe_runtime/pipe_runtime.cmake",
                     "src/xir/xxir_instance_pipe.inc.c",
                     "src/xir/xxir_instance_pipe_owner.inc.c",
                     "src/xir/xxir_sys_pipe_native.h"):
            with self.subTest(path=path):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=[path])
                self.assertIn("host-os", plan["groups"])
                self.assertTrue({"test_xir_pipe_source", "test_xir_pipe_native"} <= set(plan["tests"]))
                self.assertTrue({"test_xir_pipe_source", "test_xir_pipe_native"} <= set(plan["targets"]))
                self.assertNotIn("native-runtime", plan["groups"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_process_product_and_test_leaves_select_both_carriers(self):
        for path in ("stdlib/time/time.xr", "src/os/os_process_owned.h", "src/os/win/process_owned_win.c",
                     "src/os/win/proc_text_win.inc.c", "src/os/unix/process_owned_unix.c",
                     "tests/unit/xir/process_runtime/test_process_source.c",
                     "tests/unit/xir/process_runtime/test_process_native.c",
                     "tests/unit/xir/process_runtime/process_cases.h",
                     "tests/unit/xir/process_runtime/process_runtime.h",
                     "tests/unit/xir/process_runtime/process_runtime.cmake",
                     "src/xir/xxir_instance_process.inc.c",
                     "src/xir/xxir_instance_process_owner.inc.c",
                     "src/xir/xxir_sys_process_native.h"):
            with self.subTest(path=path):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=[path])
                self.assertIn("host-os", plan["groups"])
                self.assertTrue({"test_xir_process_source", "test_xir_process_native"} <= set(plan["tests"]))
                self.assertTrue({"test_xir_process_source", "test_xir_process_native"} <= set(plan["targets"]))
                self.assertNotIn("native-runtime", plan["groups"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_dylib_product_and_test_leaves_select_both_carriers(self):
        for path in ("stdlib/path/path.xr", "src/os/os_dylib_owned.h",
                     "src/os/win/dylib_owned_win.c", "src/os/unix/dylib_owned_unix.c",
                     "src/xir/xxir_pointer_origin_internal.h",
                     "src/xir/xxir_pointer_value.inc.c", "src/xir/xxir_value.c",
                     "src/xir/xxir_pointer.h",
                     "src/xir/xxir_view_affine.c",
                     "src/xir/xxir_view_origins.inc.c",
                     "src/xir/xxir_view_roots.inc.c",
                     "src/xir/xxir_view_types.inc.c",
                     "src/xir/xxir_view_pointer_calls.inc.c",
                     "src/xir/xxir_view_access.inc.c", "src/xir/xxir_view_loan_origins.inc.c",
                     "src/xir/xxir_instance_dylib.inc.c",
                     "src/xir/xxir_instance_dylib_owner.inc.c",
                     "src/xir/xxir_sys_dylib_native.h",
                     "tests/unit/xir/dylib_runtime/test_dylib_source.c",
                     "tests/unit/xir/dylib_runtime/test_dylib_native.c",
                     "tests/unit/xir/dylib_runtime/dylib_cases.h",
                     "tests/unit/xir/dylib_runtime/dylib_runtime.h",
                     "tests/unit/xir/dylib_runtime/test_dylib_data.c",
                     "tests/unit/xir/dylib_runtime/dylib_runtime.cmake",
                     "tests/unit/xir/xir_genuine_sys_path.h"):
            with self.subTest(path=path):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=[path])
                self.assertIn("host-os", plan["groups"])
                self.assertTrue({"test_xir_dylib_source", "test_xir_dylib_native"} <= set(plan["tests"]))
                self.assertTrue({"test_xir_dylib_source", "test_xir_dylib_native"} <= set(plan["targets"]))
                self.assertNotIn("native-runtime", plan["groups"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_callback_mechanisms_and_test_recipes_select_the_existing_runtime_home(self):
        for path in ("src/execution/xr_xir_host_execution.c",
                     "src/xir/xxir_root_callback_internal.h",
                     "src/xir/xxir_task.c", "src/xir/xxir_task_internal.h",
                     "src/xir/xxir_task_root_callback.inc.c",
                     "src/xir/xxir_task_generator.inc.c", "src/xir/xxir_task_channel.inc.c",
                     "src/xir/xxir_instance_root_callback.inc.c",
                     "tests/unit/xir/test_xir_root_callback.c",
                     "tests/unit/xir/root_callback.cmake"):
            with self.subTest(path=path):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=[path])
                self.assertIn("runtime", plan["groups"])
                self.assertIn("test_xir_root_callback", plan["tests"])
                self.assertIn("test_xir_root_callback", plan["targets"])
                self.assertNotIn("native-runtime", plan["groups"])
        self.assertEqual(len(xray_l1.tier_plan(ROOT, "L1")["tests"]), 30)

    def test_signal_registration_sources_and_native_root_replay_select_both_carriers(self):
        for path in ("src/xir/xxir_call_signal.inc.c", "src/xir/xxir_call_signal_internal.h",
                     "src/xir/xxir_instance_signal.inc.c", "src/xir/xxir_instance_signal_internal.h",
                     "src/xir/xxir_source_sys.inc.c", "src/xir/xxir_source.c",
                     "src/xir/xxir_sys_native.h", "src/xir/xxir_verify.c",
                     "src/xir/xxir_effects.c", "src/xir/xxir_effects.h",
                     "src/xir/xxir_effect_invocation_causes.inc.c",
                     "src/xir/xxir_effect_invocation_primitive.inc.c",
                     "src/xir/xxir_effect_invocation_query.inc.c",
                     "src/xir/xxir_effect_invocation_solve.inc.c",
                     "tests/unit/xir/test_xir_signal_source.c",
                     "tests/unit/xir/test_xir_signal_native.c",
                     "tests/unit/xir/root_signal_runtime.h", "tests/unit/xir/root_signal.cmake"):
            with self.subTest(path=path):
                plan = xray_l1.tier_plan(ROOT, "L2", changed=[path])
                self.assertIn("runtime", plan["groups"])
                self.assertTrue({"test_xir_signal_source", "test_xir_signal_native"} <= set(plan["tests"]))
                self.assertTrue({"test_xir_signal_source", "test_xir_signal_native"} <= set(plan["targets"]))
                self.assertNotIn("native-runtime", plan["groups"])
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
