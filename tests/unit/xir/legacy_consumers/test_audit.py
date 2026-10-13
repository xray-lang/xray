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
        cls.legacy = audit.read_json(audit.ROOT / "tests/unit/xir/product_consumers/legacy_responsibilities.json")

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
