"""Negative controls for legacy denominator and independent-observation checks."""
import copy
import unittest

import audit


class PreparationChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mapping = audit.read_json(audit.HERE / "responsibilities.json")
        cls.oracles = audit.read_json(audit.HERE / "oracles.json")
        cls.routes = audit.read_json(audit.HERE / "consumer_routes.json")
        cls.runner_routes = audit.read_json(audit.HERE / "runner_routes.json")
        cls.legacy = audit.read_json(audit.ROOT / "tests/unit/xir/product_consumers/legacy_responsibilities.json")

    def test_frozen_transitions_cannot_fall_back_to_original_rejection(self):
        for identity in audit.FROZEN_TARGETS:
            with self.subTest(identity=identity):
                bad = copy.deepcopy(self.runner_routes)
                row = next(r for r in bad["cases"] if r["case_id"] == identity)
                del row["target_disposition"]
                with self.assertRaisesRegex(ValueError, "required disposition"):
                    audit.validate_runner_routes(bad, self.mapping)

    def test_read_class_positive_requires_the_ported_source(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if r["case_id"].endswith("/007_in_instance_mutating_method.xr"))
        del row["target_source_port"]
        with self.assertRaisesRegex(ValueError, "requires its frozen source port"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_read_struct_cannot_inherit_read_class_mutation_permission(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if r["case_id"].endswith("/read_struct_mutating_receiver.xr"))
        row["target_disposition"] = {"kind": "POSITIVE_AFTER_FROZEN_CLASS_SURFACE_PORT",
                                     "owner_package": "P10", "qualification": "NOT_RUN", "reason": "invalid class-only permission"}
        with self.assertRaisesRegex(ValueError, "required disposition"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_incomplete_enum_witness_is_not_an_implemented_positive(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if r["case_id"].endswith("/020_enum_implements_rejected.xr"))
        row["target_disposition"]["kind"] = "POSITIVE_AFTER_FROZEN_CLASS_SURFACE_PORT"
        with self.assertRaisesRegex(ValueError, "required disposition"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_generator_abandonment_cannot_omit_or_duplicate_cleanup(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "generator_break_cleanup_once")
        for output in ("1\n", "1\n2\ncleanup\n", "1\ncleanup\ncleanup\n"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=output))

    def test_generator_wait_cannot_be_null_or_exhaustion(self):
        for identity, outputs in (("generator_await_preserves_stream", ("7\n", "null\n7\n2\n")),
                                  ("generator_scheduler_yield_is_not_value", ("1\nnull\n2\n", "1\n"))):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == identity)
            for output in outputs:
                with self.subTest(identity=identity, output=output), self.assertRaises(ValueError):
                    audit.compare_required(expected, dict(expected, stdout_utf8=output))

    def test_class_parameter_mutation_cannot_be_a_value_snapshot(self):
        for identity in ("read_class_direct_mutation", "read_class_indirect_mutation", "read_class_inherited_mutation"):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == identity)
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8="1\n"))

    def test_enum_witness_must_execute_both_generic_calls(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "enum_explicit_interface_witness")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="color\n"))

    def test_regression_positive_cannot_become_arbitrary_rejection(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if "original_regression_obligation" in r)
        row["original_regression_obligation"]["expected_exit"] = 1
        with self.assertRaisesRegex(ValueError, "regression positive"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_build_only_cannot_claim_test_discovery(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if "original_regression_obligation" in r)
        row["original_regression_obligation"]["test_discovery"] = "PASS"
        with self.assertRaisesRegex(ValueError, "test discovery"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_class_source_port_preserves_named_tests_and_frozen_surface(self):
        row = next(r for r in self.runner_routes["cases"] if "target_source_port" in r)
        source = (audit.ROOT / row["target_source_port"]["fixture"]).read_text(encoding="utf-8")
        self.assertIn("init()", source)
        self.assertNotIn("constructor()", source)
        self.assertNotIn("ref useWith", source)
        self.assertNotIn("ref useHandler", source)
        for name in ("defer_runs_after_a_returned_parameter_call", "defer_runs_after_a_returned_indirect_call"):
            self.assertIn("@test fn " + name, source)
        self.assertEqual(source.count("assertEqual(g.held, true)"), 2)

    def test_target_source_port_is_not_a_qualified_product(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if "target_source_port" in r)
        row["target_source_port"]["qualification"] = "PASS"
        with self.assertRaisesRegex(ValueError, "incorrectly qualified"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_empty_test_report_cannot_satisfy_the_class_cleanup_oracle(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "class_cleanup_ordinary_methods")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, {"exit_code": 0, "required_test_names": [], "all_required_tests_passed": True})

    def test_comptime_routes_reference_existing_frozen_responsibility(self):
        row = next(r for r in self.runner_routes["cases"] if r["case_id"].endswith("/001_comptime_non_const_call.xr"))
        self.assertEqual(row["primary_family"], "task318:Q12")
        self.assertEqual(row["target_disposition"]["kind"], "POSITIVE_REQUIRED_BY_FROZEN_TASK320")
        self.assertIn("P11", row["producer_packages"])
        self.assertEqual(row["original_rejection_oracle"]["state"], "PRESENT")

    def test_planned_ctfe_positive_is_not_execution_qualification(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if "target_disposition" in r)
        row["target_disposition"]["qualification"] = "PASS"
        with self.assertRaisesRegex(ValueError, "cannot publish qualification"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_unknown_task_responsibility_is_not_a_known_family(self):
        bad = copy.deepcopy(self.runner_routes)
        bad["cases"][0]["primary_family"] = "task318:Q99"
        with self.assertRaisesRegex(ValueError, "unknown family"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_imported_program_companion_is_part_of_oracle_identity(self):
        bad = copy.deepcopy(self.oracles)
        row = next(r for r in bad["cases"] if r["id"] == "module_diamond_initializes_once")
        row["inputs"][-1]["lf_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "oracle input changed"):
            audit.validate_oracles(bad)

    def test_shared_dependency_cannot_initialize_twice(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "module_diamond_initializes_once")
        wrong = dict(expected, stdout_utf8="dependency body\ndependency cleanup\ndependency body\ndependency cleanup\n41\n42\n")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)

    def test_ref_write_cannot_change_the_saved_prior_value(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "ref_write_preserves_prior_scalar_copy")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="3\n-1\n7\n7\n7\n"))

    def test_old_no_call_rejection_cannot_satisfy_target_ctfe_positive(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "ctfe_closed_ordinary_call_positive")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, {"exit_code": 1, "stdout_utf8": "", "stderr_utf8": "comptime expression must be evaluable"})

    def test_explicit_runner_routes_keep_the_original_inputs_and_oracles(self):
        audit.validate_runner_routes(self.runner_routes, self.mapping)

    def test_runner_source_change_requires_semantic_review(self):
        bad = copy.deepcopy(self.runner_routes)
        bad["cases"][0]["source_lf_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "source changed"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_runtime_trap_cannot_replace_compile_entry(self):
        bad = copy.deepcopy(self.runner_routes)
        bad["cases"][0]["original_runner_entries"][0]["entry"] = "run"
        with self.assertRaisesRegex(ValueError, "runner entry"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_wrong_diagnostic_location_is_not_preserved_oracle(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if r.get("original_rejection_oracle", {}).get("state") == "PRESENT")
        row["original_rejection_oracle"]["assertions"][0]["where"]["col"] += 1
        with self.assertRaisesRegex(ValueError, "position, code or text"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_position_gap_annotation_cannot_be_removed_to_pass(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if any(a["misplaced"] for a in r.get("original_rejection_oracle", {}).get("assertions") or []))
        next(a for a in row["original_rejection_oracle"]["assertions"] if a["misplaced"])["misplaced"] = None
        with self.assertRaisesRegex(ValueError, "position, code or text"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_missing_expected_file_stays_an_open_gap(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if r.get("original_rejection_oracle", {}).get("state") == "MISSING")
        row["original_rejection_oracle"]["assertions"] = []
        with self.assertRaisesRegex(ValueError, "cannot be fabricated"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_sibling_reader_snapshot_is_rejected_even_for_equal_backends(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "sibling_closures_share_mutable_cell")
        for backend in ("vm", "native"):
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8="1\n2\n0\n101\n100\n11\n22\n10\n"))

    def test_cleanup_escape_is_not_a_successful_caught_panic(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "cleanup_panic_escape_terminates")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, exit_code=0, stdout_utf8="body-ran\nouter-handler-must-not-run\n"))

    def test_generator_error_cannot_be_reported_as_exhaustion(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "generator_error_is_not_exhaustion")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="direct 1\ndirect-panic 432\nlooped 1\n"))

    def test_source_routes_preserve_all_original_responsibilities(self):
        audit.validate_routes(self.routes, self.legacy, self.mapping)

    def test_route_does_not_reassign_production_to_independent_validation(self):
        bad = copy.deepcopy(self.routes)
        bad["source_cases"][0]["producer_packages"] = ["P20"]
        with self.assertRaises(ValueError):
            audit.validate_routes(bad, self.legacy, self.mapping)

    def test_source_route_omission_is_rejected(self):
        bad = copy.deepcopy(self.routes)
        bad["source_cases"].pop()
        with self.assertRaisesRegex(ValueError, "responsibility set"):
            audit.validate_routes(bad, self.legacy, self.mapping)

    def test_route_cannot_borrow_another_cases_assertion_body(self):
        bad = copy.deepcopy(self.routes)
        bad["source_cases"][0]["body_lf_sha256"] = bad["source_cases"][1]["body_lf_sha256"]
        with self.assertRaisesRegex(ValueError, "assertion body"):
            audit.validate_routes(bad, self.legacy, self.mapping)

    def test_harness_exit_is_not_the_language_result(self):
        row = next(r for r in self.routes["source_cases"] if r["case_id"] ==
                   "source_owner_runs_each_dense_coroutine_state_across_private_executors")
        self.assertEqual(row["original_native_harness_expectation"]["expected_exit"], 227)
        self.assertEqual(row["existing_projection"]["expected_i64"], 42)
        bad = copy.deepcopy(self.routes)
        target = next(r for r in bad["source_cases"] if r["case_id"] == row["case_id"])
        target["original_native_harness_expectation"]["expected_exit"] = 42
        with self.assertRaisesRegex(ValueError, "harness expectation"):
            audit.validate_routes(bad, self.legacy, self.mapping)

    def test_helper_file_without_counted_tests_is_not_removable(self):
        bad = copy.deepcopy(self.routes)
        helper = next(r for r in bad["other_files"] if not r["functions"])
        bad["other_files"].remove(helper)
        with self.assertRaisesRegex(ValueError, "helper owner"):
            audit.validate_routes(bad, self.legacy, self.mapping)

    def test_complete_mapping(self):
        audit.validate_responsibilities(self.mapping)

    def test_removed_requirement_is_not_a_smaller_denominator(self):
        bad = copy.deepcopy(self.mapping)
        bad["task322_requirements"].pop()
        with self.assertRaises(ValueError):
            audit.validate_responsibilities(bad)

    def test_duplicate_requirement_cannot_replace_missing_case(self):
        bad = copy.deepcopy(self.mapping)
        bad["task322_requirements"][-1] = bad["task322_requirements"][0]
        with self.assertRaises(ValueError):
            audit.validate_responsibilities(bad)

    def test_preparation_cannot_publish_product_pass(self):
        bad = copy.deepcopy(self.mapping)
        bad["capabilities"][0]["status"] = "PASS"
        with self.assertRaises(ValueError):
            audit.validate_responsibilities(bad)

    def test_original_programs_and_explicit_oracles(self):
        audit.validate_oracles(self.oracles)

    def test_changed_positive_input_is_rejected(self):
        bad = copy.deepcopy(self.oracles)
        bad["cases"][0]["inputs"][0]["lf_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "oracle input changed"):
            audit.validate_oracles(bad)

    def test_missing_output_is_not_empty_output(self):
        with self.assertRaises(ValueError):
            audit.compare_required({"stdout_utf8": ""}, {})

    def test_identical_wrong_backends_both_fail_independent_oracle(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "text_program")
        wrong = dict(expected, return_i64=3)
        for backend in ("vm", "native"):
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)

    def test_cleanup_order_and_trailing_newline_are_observable(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "cleanup_lifo")
        audit.compare_required(expected, dict(expected))
        for output in (expected["stdout_utf8"].replace("close C\nclose B\nclose A", "close A\nclose B\nclose C"),
                       expected["stdout_utf8"].rstrip("\n")):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=output))

    def test_runtime_failure_does_not_match_compile_rejection(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "missing_name")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, {"exit_code": 1, "stderr_utf8": expected["message"]})

    def test_boolean_is_not_integer_observation(self):
        with self.assertRaises(ValueError):
            audit.compare_required({"exit_code": 0}, {"exit_code": False})


if __name__ == "__main__":
    unittest.main()
