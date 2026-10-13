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

    def test_dual_entry_input_cannot_lose_either_original_entry(self):
        for suite in ("regression", "differential"):
            bad = copy.deepcopy(self.runner_routes)
            row = next(r for r in bad["cases"] if len(r["original_runner_entries"]) == 2)
            row["original_runner_entries"] = [e for e in row["original_runner_entries"] if e["suite"] != suite]
            with self.subTest(suite=suite), self.assertRaisesRegex(ValueError, "original runner entry"):
                audit.validate_runner_routes(bad, self.mapping)

    def test_unique_program_counts_do_not_replace_entry_responsibilities(self):
        counts = audit.validate_runner_routes(self.runner_routes, self.mapping)
        self.assertEqual(counts["reviewed_dual_runner_inputs"], 66)
        self.assertEqual(counts["reviewed_runner_entry_links"], counts["reviewed_runner_inputs"] + 66)

    def test_differential_arguments_and_exact_stdin_cannot_be_lost(self):
        for field in ("arguments", "stdin_hex", "stdin_sidecar", "arguments_sidecar", "execution"):
            bad = copy.deepcopy(self.runner_routes)
            row = next(r for r in bad["cases"] if "original_differential_obligation" in r)
            if field == "arguments":
                row["original_differential_obligation"][field] = ["unreviewed-argument"]
            elif field == "stdin_hex":
                row["original_differential_obligation"][field] = "00"
            elif field == "execution":
                row["original_differential_obligation"][field] = "PASS"
            else:
                row["original_differential_obligation"].pop(field)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "stdin bytes"):
                audit.validate_runner_routes(bad, self.mapping)

    def test_coroutine_textual_tests_cannot_be_removed_from_test_obligation(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if r["case_id"].endswith("1164_channel_recv_raw_match_nullable_payload.xr"))
        row.pop("original_named_test_obligation")
        with self.assertRaisesRegex(ValueError, "falsely qualified discovery"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_sticky_array_result_cannot_keep_already_taken_target(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if r["case_id"].endswith("080_transfer_task_second_await_rejected.xr"))
        self.assertEqual(row["original_rejection_oracle"]["assertions"][0]["code"], "E0356")
        row["target_disposition"]["kind"] = "REJECTION_FOR_ALREADY_TAKEN"
        with self.assertRaisesRegex(ValueError, "required disposition"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_sticky_result_copies_cannot_alias_or_consume_terminal(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "task_array_sticky_independent_copies")
        for output in ("3\nAlreadyTaken\n", "3\n3\n9\n9\n9\n9\n2\n"):
            for backend in ("vm", "native"):
                with self.subTest(backend=backend, output=output), self.assertRaises(ValueError):
                    audit.compare_required(expected, dict(expected, stdout_utf8=output))

    def test_mutable_class_task_result_is_not_sendable_by_const_or_identity(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "task_mutable_class_result_rejected")
        for observed in (dict(expected, diagnostic_reason="UNSUPPORTED_SYNTAX"),
                         dict(expected, product_published=True), {"exit_code": 0, "stdout_utf8": "42\n"}):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_closed_waiters_cannot_be_counted_from_timeout_or_closed_report_channel(self):
        source = (audit.HERE / "fixtures/channel_close_distinct_from_timeout/root.xr").read_text(encoding="utf-8")
        self.assertIn("const waiter_count = 64", source)
        self.assertIn("Recv.Closed -> done.send(-1)", source)
        self.assertIn("Recv.Timeout -> done.send(-2)", source)
        self.assertIn("_ -> -1000001", source)
        self.assertNotIn("_ -> done.send(-1)", source)

    def test_map_stress_keeps_all_sends_without_worker_module_var_read(self):
        source = (audit.HERE / "fixtures/channel_map_value_copy_stress/root.xr").read_text(encoding="utf-8")
        self.assertIn("const PRODUCERS = 4", source)
        self.assertIn("const SENDS_PER_PRODUCER = 5000", source)
        self.assertIn("maps.send(m)", source)
        self.assertIn("assertEqual(sum, total * 2)", source)
        self.assertIn("assertEqual(completed, total)", source)
        self.assertNotIn("copy(m)", source)

    def test_cancel_terminal_must_follow_exactly_one_cleanup(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "task_cancel_publishes_after_cleanup")
        for output in ("cancelled\ndefer: cleanup ran\n", "defer: cleanup ran\ndefer: cleanup ran\ncancelled\n"):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=output))

    def test_overlapping_waiters_have_distinct_required_results(self):
        for identity, wrong in (("1161_multiple_task_awaiters", "318\ndone\n"),
                                ("1162_overlapping_await_any_waiters", "314\ndone\n")):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == identity + "_independent_results")
            with self.subTest(identity=identity), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, test_body_stdout_utf8=wrong))

    def test_named_source_obligations_cannot_be_dropped_or_marked_discovered(self):
        for change in ("drop", "PASS"):
            bad = copy.deepcopy(self.runner_routes)
            row = next(r for r in bad["cases"] if "original_named_test_obligation" in r)
            if change == "drop":
                row["original_named_test_obligation"]["declarations"].pop()
            else:
                row["original_named_test_obligation"]["execution"] = change
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "falsely qualified discovery"):
                audit.validate_runner_routes(bad, self.mapping)

    def test_target_named_test_migration_must_cover_every_original_identity(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if "named_test_migration" in r)
        row["named_test_migration"]["mapping"].pop(next(iter(row["named_test_migration"]["mapping"])))
        with self.assertRaisesRegex(ValueError, "omitted"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_partial_reverse_program_cannot_replace_the_thirteen_test_suite(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if "partial_target_program" in r)
        row["partial_target_program"]["complete_file_replacement"] = True
        with self.assertRaisesRegex(ValueError, "remaining original tests"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_partial_reverse_program_cannot_delete_stringbuilder_obligations(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if "partial_target_program" in r)
        row["partial_target_program"]["remaining_original_tests"] = []
        with self.assertRaisesRegex(ValueError, "remaining original tests"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_returned_array_is_not_a_shared_mutable_receiver(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "array_return_logical_copy")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="3\n3\n1\n4\n4\n3\n1\n9\n9\n60\n"))

    def test_nested_record_port_retires_shared_child_writes_without_losing_tests(self):
        source = (audit.HERE / "fixtures/record_nullable_array_value_copy/root.xr").read_text(encoding="utf-8")
        self.assertIn("while (i < 32)", source)
        self.assertIn("assert(frame_total(original) == 84)", source)
        self.assertIn("assert(frame_total(independent.frame) == 84)", source)
        self.assertIn("assert(frame_total(child) == 49)", source)
        self.assertNotIn("copy(original)", source)
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "record_nullable_array_value_copy_named_tests")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, required_test_names=expected["required_test_names"][:-1]))

    def test_literal_marker_coordinates_reference_actual_attribute_lines(self):
        row = next(r for r in self.runner_routes["cases"] if "original_named_test_obligation" in r)
        lines = (audit.ROOT / row["case_id"]).read_text(encoding="utf-8").splitlines()
        for declaration in row["original_named_test_obligation"]["declarations"]:
            self.assertTrue(lines[declaration["marker_line"] - 1].strip().startswith("@test"))

    def test_complete_original_rejections_remain_bound_even_with_missing_oracle(self):
        counts = audit.validate_runner_routes(self.runner_routes, self.mapping)
        self.assertEqual(counts["reviewed_rejection_cases"], 891)
        self.assertEqual(counts["unreviewed_rejection_inputs"], 0)
        self.assertEqual(counts["reviewed_missing_rejection_oracles"], 1)

    def test_prior_class_ports_have_explicit_non_field_init_parameter_types(self):
        for identity in ("read_class_direct_mutation", "read_class_indirect_mutation", "read_class_inherited_mutation"):
            source = (audit.HERE / "fixtures" / identity / "root.xr").read_text(encoding="utf-8")
            self.assertIn("init(v: i64)", source)
            self.assertNotIn("init(v)", source)

    def test_type_value_port_cannot_use_the_original_retired_constructor_surface(self):
        for name in ("084_const_object_field_store_rejected.xr", "087_const_object_alias_field_store_rejected.xr",
                     "090_const_object_mutable_alias_init_rejected.xr", "099_shared_object_field_store_rejected.xr",
                     "100_shared_object_compound_field_store_rejected.xr"):
            source = (audit.HERE / "fixtures/type_value_ports" / name).read_text(encoding="utf-8")
            self.assertIn("init(v: i64)", source)
            self.assertNotIn("constructor(", source)

    def test_const_value_copy_mutation_cannot_change_the_prior_array_or_map(self):
        for name, wrong in (("085_const_array_alias_mutating_method", "1\n1\n"),
                            ("086_const_map_alias_index_store_rejected", "2\n2\n"),
                            ("088_const_array_mutable_alias_init_rejected", "9\n9\n"),
                            ("089_const_map_mutable_alias_init_rejected", "9\n9\n")):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "type_value_" + name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_class_identity_copy_cannot_be_a_deep_snapshot(self):
        for name, wrong in (("087_const_object_alias_field_store_rejected", "1\n2\n"),
                            ("090_const_object_mutable_alias_init_rejected", "1\n9\n")):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "type_value_" + name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_class_array_field_projection_copy_cannot_mutate_the_field(self):
        for name, wrong in (("105_shared_member_array_alias_index_store_rejected", "1\n1\n"),
                            ("106_shared_member_array_alias_mutating_method_rejected", "2\n2\n"),
                            ("118_shared_derived_closure_capture_mutation_rejected", "9\n9\n")):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "type_value_" + name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_same_family_union_tag_is_not_numeric_representability(self):
        for name in ("union_indiscriminable_int_members", "union_indiscriminable_float_members", "union_indiscriminable_via_alias"):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "type_value_" + name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8="true\ntrue\ntrue\ntrue\n"))

    def test_local_tuple_view_exception_does_not_open_array_view_storage(self):
        row = next(r for r in self.runner_routes["cases"] if r["case_id"].endswith("/span_tuple_literal_escape.xr"))
        self.assertEqual(row["target_disposition"]["kind"], "POSITIVE_LOCAL_VIEW_TUPLE_AFTER_FROZEN_TASK313_PORT")
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if r["case_id"].endswith("/span_array_literal_escape.xr"))
        row["target_disposition"] = {"kind": "POSITIVE_LOCAL_VIEW_TUPLE_AFTER_FROZEN_TASK313_PORT", "owner_package": "P10", "qualification": "NOT_RUN", "reason": "invalid container exemption"}
        with self.assertRaisesRegex(ValueError, "required disposition"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_imported_diagnostics_cannot_omit_companion_inputs(self):
        for identity in audit.IMPORTED_ORACLE_INPUTS:
            with self.subTest(identity=identity):
                bad = copy.deepcopy(self.runner_routes)
                row = next(r for r in bad["cases"] if r["case_id"] == identity)
                del row["original_imported_inputs"]
                with self.assertRaisesRegex(ValueError, "required companion inputs"):
                    audit.validate_runner_routes(bad, self.mapping)

    def test_imported_effect_diagnostic_cannot_bind_changed_companion(self):
        bad = copy.deepcopy(self.runner_routes)
        row = next(r for r in bad["cases"] if "original_imported_inputs" in r)
        row["original_imported_inputs"][0]["lf_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "companion changed"):
            audit.validate_runner_routes(bad, self.mapping)

    def test_oracle_cannot_execute_a_different_unbound_source(self):
        bad = copy.deepcopy(self.oracles)
        row = next(r for r in bad["cases"] if r["id"] == "atomic_var_handle_root_parameter")
        row["execution_source"] = "tests/diff/cases/basic/arith.xr"
        with self.assertRaisesRegex(ValueError, "execution source"):
            audit.validate_oracles(bad)

    def test_raw_string_interpolates_but_raw_byte_backslash_survives(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "quoted_literal_family_bytes_and_newlines")
        for output in (expected["stdout_utf8"].replace("Xray\\n", "${name}\\n"),
                       expected["stdout_utf8"].replace("hello \"Xray\"\n\n", "hello \"Xray\"\n"),
                       expected["stdout_utf8"].replace("\n92\n", "\n10\n")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=output))

    def test_parallel_defaults_must_execute_all_original_operations(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "parallel_library_owned_default_options")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="10\n4\n9\n3\n13\n"))

    def test_atomic_named_test_report_cannot_be_empty_or_partial(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "atomic_original_named_tests")
        for names in ([], expected["required_test_names"][:-1]):
            with self.subTest(names=names), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, required_test_names=names))

    def test_atomic_var_rebinding_cannot_overwrite_the_old_identity(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "atomic_var_handle_root_parameter")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="30\n31\n31\n2\n2\n"))

    def test_atomic_worker_rejection_requires_the_concrete_root_binding(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "atomic_var_worker_direct_rejection")
        for observed in ({"diagnostic_stage": "CHECK", "product_published": False},
                         dict(expected, rejected_root_binding="Atomic var binding"),
                         dict(expected, diagnostic_stage="RUNTIME", product_published=True)):
            with self.subTest(observed=observed), self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

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
