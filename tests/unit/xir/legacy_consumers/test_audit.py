"""Negative controls for legacy denominator and independent-observation checks."""
import copy
import collections
import tempfile
from pathlib import Path
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
        # One immutable discovery fixture per control suite. The separate
        # inventory CTest and audit CLI always rediscover the actual filesystem.
        regression = audit.load_module("control_suite_regression", "scripts/run_regression_tests.py")
        differential = audit.load_module("control_suite_differential", "tests/diff/run_backend_diff.py")
        errors = audit.load_module("control_suite_errors", "tests/compile_errors/run_compile_error_tests.py")
        cls.actual_runner_entries = collections.defaultdict(list)
        for path in regression.collect_cases():
            cls.actual_runner_entries[audit.relative(path)].append({"suite":"regression", "entry":"test" if regression.is_test_module(path) else "run"})
        for path in differential.collect_cases("", str(differential.SCRIPT_DIR / "coro_regression_cases.txt")):
            cls.actual_runner_entries[audit.relative(path)].append({"suite":"differential", "entry":"run"})
        for path in errors.collect_cases():
            cls.actual_runner_entries[audit.relative(path)].append({"suite":"compile_errors", "entry":"implicit_compile"})

    def validate_runner_routes(self, data):
        # Source, sidecar, fixture existence/content/hash and every obligation
        # are checked freshly on each call; only runner discovery is fixture data.
        return audit.validate_runner_routes(data, self.mapping, self.actual_runner_entries)


    def core_expected(self, name):
        return next(r['expected'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_'+name)

    def test_core_named_manifest_and_per_test_values_cannot_be_omitted(self):
        counts = self.validate_runner_routes(self.runner_routes)
        self.assertEqual(counts['unreviewed_named_regression_test_files'], 0)
        rows = [r for r in self.oracles['cases'] if r['id'].startswith('regression_core_review_')]
        self.assertEqual(len(rows), 130)
        self.assertEqual(sum(len(r['expected']['per_test_observations']) for r in rows), 1242)
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_0301_int64_overflow')))
        row['expected']['per_test_observations'].pop('test_div_min_neg_one_in_loop')
        with self.assertRaisesRegex(ValueError, 'per-test independent obligation'):
            audit.validate_oracles(bad)

    def test_core_per_test_obligation_cannot_be_only_a_nonempty_success_label(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_0100_int_basic')))
        row['expected']['per_test_observations']['test_hexadecimal'] = {'label':'PASS'}
        with self.assertRaisesRegex(ValueError, 'per-test independent obligation'):
            audit.validate_oracles(bad)

    def test_core_original_named_bodies_cannot_disappear_after_explicit_coverage(self):
        bad, row = self.mutated_runner_route('tests/regression/01_literals/0100_int_basic.xr')
        del row['original_named_test_obligation']
        with self.assertRaisesRegex(ValueError, 'named source obligations'):
            self.validate_runner_routes(bad)

    def test_decimal_only_radix_labels_do_not_qualify_actual_lexical_values(self):
        expected = self.core_expected('0100_int_basic')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, actual_lexical_forms_required=['255','16','43981']))
        row = next(r for r in self.runner_routes['cases'] if r['case_id'].endswith('/0100_int_basic.xr'))
        source = (audit.ROOT/row['target_source_port']['fixture']).read_text(encoding='utf-8')
        for token in expected['actual_lexical_forms_required']:
            self.assertIn(token, source)

    def test_float_bits_and_decimal_name_do_not_grant_separator_lexing(self):
        expected = self.core_expected('0110_float_basic')
        wrong = copy.deepcopy(expected)
        wrong['binary64_bits']['0.30000000000000004'] = '3fd3333333333333'
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, float_separator_lexical_coverage='PASS'))
        row = next(r for r in self.runner_routes['cases'] if r['case_id'].endswith('/0110_float_basic.xr'))
        self.assertEqual(row['named_test_migration']['mapping']['test_float_underscore'], 'test_float_decimal_values')

    def test_narrow_wrapping_and_chained_result_width_cannot_use_i64_answer(self):
        expected = self.core_expected('0304_fixed_width_wrapping')
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_int32_wrapping']['values'][0] = 2147483648
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, retained_receiver_widths_and_result_widths=[64]*5))

    def test_min_division_shift_and_nan_loops_cannot_keep_answer_but_shrink_pressure(self):
        for name, key, count in (('0301_int64_overflow','full_min_division_loop_iterations',200),
                                 ('0331_shift_mod64','retained_loop_iterations',300),
                                 ('0310_comparison','runtime_float_loop_iterations',260),
                                 ('0310_comparison','nan_loop_iterations',260)):
            expected = self.core_expected(name)
            self.assertEqual(expected[key],count)
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, **{key:2}))
        expected = self.core_expected('0310_comparison')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, nan_reflexivity_granted=True))

    def test_present_map_null_and_absent_lookup_are_independent_outer_states(self):
        expected = self.core_expected('0140_special_values')
        for key in ('contains_key','outer_some','inner_none','absent_key_outer_none'):
            wrong = copy.deepcopy(expected)
            wrong['map_get_outer_presence_and_inner_none'][key] = False
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)

    def test_unsigned_formatters_cannot_agree_on_signed_twin(self):
        expected = self.core_expected('0302_uint64_print_compare')
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_uint64_unsigned_format_and_compare']['values'][:2] = ['-9223372036854775808']*2
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)

    def test_optional_argument_skip_and_range_terminal_domain_cannot_be_shortcut(self):
        expected = self.core_expected('0381_optional_call')
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_optional_call_on_null_short_circuits']['values'][1] = 100
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)
        for name,key,field in (('0372_range_inclusive','full_domain_count','MAX_singleton_continue_iterations'),
                               ('0373_range_value_for_in','entry_domain_frozen_across_rebind','actual_required_iterations')):
            expected = self.core_expected(name)
            wrong = copy.deepcopy(expected)
            wrong[key][field] = 100
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)

    def test_legacy_generic_labels_and_ordering_ordinal_cannot_grant_identity(self):
        expected = self.core_expected('0230_type_annotation')
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_array_annotation']['values'][-1] = 'Array'
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)
        expected = self.core_expected('0126_implicit_language_core')
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_implicit_language_core']['values'][-1] = 4
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)

    def test_untyped_nested_optional_reverse_body_cannot_pose_as_execution_source(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_0380_optional_chain')))
        row['execution_source'] = row['inputs'][0]['path']
        with self.assertRaisesRegex(ValueError, 'cannot pose as an executable'):
            audit.validate_oracles(bad)

    def test_control_named_values_and_full_projected_sequences_remain_required(self):
        expected = self.core_expected('0410_while')
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_while_continue_and_break']['values'][0] = [1,3,5,9]
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)
        bad, row = self.mutated_runner_route('tests/regression/04_control_flow/0451_labeled_break_continue.xr')
        del row['original_named_test_obligation']
        with self.assertRaisesRegex(ValueError, 'named source obligations'):
            self.validate_runner_routes(bad)

    def test_panic_faults_cannot_be_ordinary_errors_or_compile_only_rejections(self):
        for name,key in (('0413_null_unwrap','null_unwrap_channel'),('0440_match','non_exhaustive_match_channel')):
            expected = self.core_expected(name)
            for channel in ('ORDINARY_ERROR','COMPILE_REJECTION','PASS'):
                with self.assertRaises(ValueError):
                    audit.compare_required(expected, dict(expected, **{key:channel}))

    def test_labeled_continue_must_reach_correct_outer_increment_and_trace(self):
        expected = self.core_expected('0451_labeled_break_continue')
        for name,wrong_values in (('test_labeled_break_exits_named_for_in',['00,01,10,11,20,21,']),
                                  ('test_labeled_continue_targets_named_for_in',['00,01,10,11,20,21,']),
                                  ('test_labeled_continue_targets_traditional_for_increment',[4])):
            wrong = copy.deepcopy(expected)
            wrong['per_test_observations'][name]['values'] = wrong_values
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)

    def test_deep_tail_input_or_result_does_not_establish_physical_tco(self):
        expected = self.core_expected('0511_tail_recursion')
        for wrong in (dict(expected, retained_deep_inputs=[100,100,100]),
                      dict(expected, actual_tco_frame_stack_proof='PASS')):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)
        expected = self.core_expected('0510_recursion')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, retained_depth_inputs=[10,50]))

    def test_indirect_callee_observes_held_before_cleanup_and_capture_allocation(self):
        expected = self.core_expected('0512_defer_around_return_call')
        for key in ('indirect_callee_held_until_return','cleanup_after_actual_callee',
                    'indirect_owned_capture_cycle_has_scoped_break',
                    'scoped_break_registered_before_first_capture_assignment'):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, **{key:False}))
        row = next(r for r in self.runner_routes['cases'] if r['case_id'].endswith('/0512_defer_around_return_call.xr'))
        source = (audit.ROOT/row['target_source_port']['fixture']).read_text(encoding='utf-8')
        self.assertEqual(source.count('assertEqual(g.held, true)'),4)
        self.assertLess(source.index('defer { g.handlers.clear() }'),source.index('g.handlers[0] = fn'))

    def test_shared_or_independent_closure_cells_cannot_be_replaced_by_one_counter(self):
        expected = self.core_expected('0521_cell_upval')
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_two_cells']['values'] = [1,2,3,4]
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)
        for wrong in (dict(expected, retained_independent_instances=1),dict(expected, retained_additions=10)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)
        expected = self.core_expected('0520_closure')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, fresh_loop_bindings_values=[3,3,3]))

    def test_function_unit_is_not_null_or_an_erased_debug_name(self):
        expected = self.core_expected('0500_basic')
        for value in (None,'()'):
            wrong = copy.deepcopy(expected)
            wrong['per_test_observations']['test_empty_function']['values'] = [value]
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)

    def test_default_stdout_and_original_single_call_duties_cannot_be_smoke(self):
        expected = self.core_expected('0580_default_params')
        for wrong in (dict(expected, body_stdout_utf8='Hello, World\n'),
                      dict(expected, original_body_calls_preserved_exactly_once=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)
        row = next(r for r in self.runner_routes['cases'] if r['case_id'].endswith('/0580_default_params.xr'))
        source = (audit.ROOT/row['target_source_port']['fixture']).read_text(encoding='utf-8')
        self.assertEqual(source.count('= add(1)'),1)
        self.assertEqual(source.count('= formatName("John")'),1)

    def test_fn_items_generic_optional_and_synchronous_callback_are_distinct(self):
        expected = self.core_expected('0581_function_item_value_binding')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, only_function_items_are_hoisted=False))
        expected = self.core_expected('0540_higher_order')
        for key in ('typed_optional_identity_none_and_some_zero_required','processAsync_is_only_synchronous_body'):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, **{key:False}))

    def test_unpublished_generator_iterator_cannot_pose_as_qualified_execution_source(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_0431_for_in_try_scope')))
        row['execution_source'] = row['inputs'][0]['path']
        with self.assertRaisesRegex(ValueError,'cannot pose as an executable'):
            audit.validate_oracles(bad)

    def test_collection_payload_writeback_and_seed_copy_cannot_be_legacy_alias(self):
        expected = self.core_expected('0651_nested_higher_order_combo')
        for key in ('map_get_payload_mutation_written_back','chunk_append_uses_nested_writable_projection','seed_values_unchanged'):
            with self.assertRaises(ValueError):
                audit.compare_required(expected,dict(expected,**{key:False}))
        row = next(r for r in self.runner_routes['cases'] if r['case_id'].endswith('/0651_nested_higher_order_combo.xr'))
        source = (audit.ROOT/row['target_source_port']['fixture']).read_text(encoding='utf-8')
        self.assertIn('next.set(e.group, items)',source)
        self.assertIn('next[lastIndex].push(val)',source)
        self.assertNotIn('copy(acc)',source)

    def test_collection_stress_sum_and_all500_inputs_cannot_be_smoke(self):
        expected = self.core_expected('0651_nested_higher_order_combo')
        self.assertEqual(expected['exact_stress_sum'],13847013)
        for wrong in (dict(expected,retained_stress_input=50),dict(expected,exact_stress_sum=10000001)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected,wrong)

    def test_fill_cannot_inherit_slice_clamp_or_publish_failed_receiver(self):
        expected = self.core_expected('0640_array_methods')
        for wrong in (dict(expected,fill_negative_and_reversed_channel='PASS_NOOP'),
                      dict(expected,fill_failure_keeps_prior_complete_receiver=False),
                      dict(expected,readonly_view_write_or_owner_mutation_under_active_borrow=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected,wrong)
        row = next(r for r in self.runner_routes['cases'] if r['case_id'].endswith('/0640_array_methods.xr'))
        source = (audit.ROOT/row['target_source_port']['fixture']).read_text(encoding='utf-8')
        self.assertIn('var s: MutSlice<i64>',source)
        self.assertIn('catch panic (p: PanicInfo)',source)
        self.assertEqual(source.count('assertEqual(arr, beforeInvalid)'),2)

    def test_mixed_map_present_none_and_missing_key_cannot_be_flattened(self):
        expected = self.core_expected('0620_map')
        with self.assertRaises(ValueError):
            audit.compare_required(expected,dict(expected,present_null_distinct_from_absent_key=False))
        with self.assertRaises(ValueError):
            audit.compare_required(expected,dict(expected,typed_union_optional_layers_preserved=False))

    def test_owned_utf8_builder_replacement_keeps_snapshot_and_full_append_pressure(self):
        expected = self.core_expected('0720_stringbuilder')
        for wrong in (dict(expected,retired_builder_api_restored=True),
                      dict(expected,owned_utf8_append_iterations=10),
                      dict(expected,old_owned_snapshot_survives_clear_reuse=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected,wrong)
        row = next(r for r in self.runner_routes['cases'] if r['case_id'].endswith('/0720_stringbuilder.xr'))
        source = (audit.ROOT/row['target_source_port']['fixture']).read_text(encoding='utf-8')
        self.assertNotIn('StringBuilder()',source)
        self.assertIn('output: ref Array<u8>',source)
        self.assertIn('assertEqual(firstSnapshot, "first")',source)

    def test_lossy_utf8_malformed_subparts_and_byte_ranges_cannot_be_empty(self):
        expected = self.core_expected('0732_utf8_conversion')
        wrong = copy.deepcopy(expected)
        wrong['independent_lossy_results'][3] = 'A€�B'
        with self.assertRaises(ValueError):
            audit.compare_required(expected,wrong)
        for wrong in (dict(expected,raw_lossy_inputs=[[255]]),
                      dict(expected,typed_utf8_failure_channel='PANIC'),
                      dict(expected,strict_byte_range_invalid_samples=[[0,7]])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected,wrong)

    def test_scalar_and_octet_counts_cannot_use_utf16_or_grant_chr_none(self):
        expected = self.core_expected('0730_unicode')
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_length_emoji']['values'] = [2,6]
        with self.assertRaises(ValueError):
            audit.compare_required(expected,wrong)
        expected = self.core_expected('0780_chr_function')
        with self.assertRaises(ValueError):
            audit.compare_required(expected,dict(expected,invalid_scalar_channel='NONE'))

    def test_full_concurrent_content_cannot_be_only_its_length_or_threshold_label(self):
        expected = self.core_expected('0751_threshold_branch')
        for wrong in (dict(expected,concurrent_job_returns_actual_owned_content=False),
                      dict(expected,retained_dimensions=[1,1,1,1,1,1]),
                      dict(expected,actual_worker_buffer_pool_path='PASS')):
            with self.assertRaises(ValueError):
                audit.compare_required(expected,wrong)
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_concurrent_threshold_boundary']['values'][0][0] = 'a'*63
        with self.assertRaises(ValueError):
            audit.compare_required(expected,wrong)

    def test_claimed200_pressure_is_distinct_from_original100_body(self):
        expected = self.core_expected('0770_string_pool_concurrent_property')
        self.assertEqual(expected['original_last_actual_jobs'],100)
        self.assertEqual(expected['planned_last_actual_jobs'],200)
        with self.assertRaises(ValueError):
            audit.compare_required(expected,dict(expected,planned_last_actual_jobs=100))
        self.assertEqual(len(expected['per_test_observations']['test_property_concurrent_stress_200']['values'][0]),200)

    def test_json_cycles_custom_hash_and_sort_labels_do_not_grant_builtin_proof(self):
        for name,key in (('0690_user_hashable_key','canonical_key_witness'),
                         ('0680_deep_eq_cyclic','canonical_recursive_json_equal'),
                         ('0640_array_methods','sort_compare_qualification')):
            expected = self.core_expected(name)
            with self.assertRaises(ValueError):
                audit.compare_required(expected,dict(expected,**{key:'PASS'}))
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_0690_user_hashable_key')))
        row['execution_source'] = row['inputs'][0]['path']
        with self.assertRaisesRegex(ValueError,'cannot pose as an executable'):
            audit.validate_oracles(bad)

    def test_enum_error_cannot_be_panic_or_skip_throw_cleanup(self):
        expected = self.core_expected('0830_throw_catch_value_return')
        for wrong in (dict(expected, error_channel='PANIC'),
                      dict(expected, defer_on_throw_complete=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['defer_runs_on_throw']['values'] = [True, False]
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)

    def test_successful_local_openfile_body_does_not_prove_failure_or_filesystem(self):
        expected = self.core_expected('0840_enum_throw')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, readConfig_failure_or_real_file_io_proven=True))

    def test_owned_slice_copy_must_keep_all_growth_and_source_independence(self):
        expected = self.core_expected('1403_slice_grow_lifetime')
        for wrong in (dict(expected, retained_growth_dimensions=[4,64,3]),
                      dict(expected, copied_snapshot_independent=False),
                      dict(expected, actual_heap_realloc_or_physical_release='PASS')):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_byte_array_copy_survives_source_grow']['values'][-3] = 99
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)

    def test_exclusive_view_overlap_or_escaping_slice_cannot_replace_owned_capture(self):
        expected = self.core_expected('1405_span_static_surface')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, overlapping_exclusive_views_or_owner_access_under_live_borrow=True))
        expected = self.core_expected('1408_span_closure_owner_capture')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, capture='ESCAPING_SLICE'))

    def test_local_field_array_copy_cannot_deepfreeze_class_or_alias_its_field(self):
        expected = self.core_expected('1410_shared_provenance_rebind_reset')
        for key in ('ordinary_local_field_array_copy_writable', 'class_field_unchanged'):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, **{key:False}))

    def test_readonly_reexport_cannot_drop_transitive_defining_module(self):
        identity = 'tests/regression/14_typed_array/1414_shared_provenance_reexported_readonly_function_value_param.xr'
        bad, row = self.mutated_runner_route(identity)
        row['original_imported_inputs'].pop()
        with self.assertRaisesRegex(ValueError, 'required companion inputs'):
            self.validate_runner_routes(bad)
        expected = self.core_expected('1414_shared_provenance_reexported_readonly_function_value_param')
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, unused_imported_mutator_definition_cannot_be_skipped=False))

    def test_user_copy_and_renamed_callee_require_same_definition_owned_view_relation(self):
        for name in ('1420_view_return_through_user_copy', '1421_view_return_through_renamed_callee'):
            expected = self.core_expected(name)
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, callee_from_definition_identity_not_name_or_arity=False))

    def test_nullable_last_field_cannot_flatten_optional_chain_result(self):
        expected = self.core_expected('1602_optional_chain_shortcircuit')
        for wrong in (dict(expected, last_nullable_field_layers=1),
                      dict(expected, implicit_optional_flattening=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)
        wrong = copy.deepcopy(expected)
        wrong['per_test_observations']['test_chain_last_link_may_be_nullable']['values'] = [{'state':'Some','value':'tagged'}]
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)

    def test_unpublished_byte_query_or_chain_replacements_cannot_claim_execution_source(self):
        for name in ('1402_byte_array_intrinsics', '1416_byte_receiver_effect_matrix',
                     '1601_narrowing_facts', '1602_optional_chain_shortcircuit'):
            bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_' + name)))
            row['execution_source'] = row['inputs'][0]['path']
            with self.assertRaisesRegex(ValueError, 'cannot pose as an executable'):
                audit.validate_oracles(bad)

    def test_loop_zero_entry_and_continue_write_cannot_use_stale_fact(self):
        for name, test, values in (
                ('1604_for_in_loop_header','test_for_in_empty_collection_keeps_pre_loop_value',['gone']),
                ('1604_for_in_loop_header','test_for_in_body_write_guarded_after_loop',['x']),
                ('1604_loop_control_early_exit','test_n10_continue_write_reaches_header',[3,1])):
            expected = self.core_expected(name)
            wrong = copy.deepcopy(expected)
            wrong['per_test_observations'][test]['values'] = values
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)

    def test_full_return_suite_cannot_shrink_back_to_three_reverse_duties(self):
        expected = self.core_expected('0804_self_returning_method_rc')
        self.assertEqual(len(expected['required_test_names']), 13)
        wrong = copy.deepcopy(expected)
        wrong['required_test_names'] = wrong['required_test_names'][:3]
        with self.assertRaises(ValueError):
            audit.compare_required(expected, wrong)
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_0804_self_returning_method_rc')))
        row['expected']['per_test_observations'].pop('test_stringbuilder_clear_escapes_receiver_death')
        with self.assertRaisesRegex(ValueError, 'per-test independent obligation'):
            audit.validate_oracles(bad)

    def test_full_return_suite_cannot_restore_builder_alias_or_temporary_ref_chain(self):
        expected = self.core_expected('0804_self_returning_method_rc')
        for wrong in (dict(expected, returned_array_and_receiver_logically_independent=False),
                      dict(expected, retired_builder_api_restored=True),
                      dict(expected, chained_ref_call_uses_writable_intermediate=False),
                      dict(expected, old_owned_text_survives_clear=False),
                      dict(expected, retained_loop_iterations=2)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)

    def test_full_return_draft_cannot_qualify_unpublished_compare_capacity_or_set(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_core_review_0804_self_returning_method_rc')))
        row['execution_source'] = row['inputs'][1]['path']
        with self.assertRaisesRegex(ValueError, 'cannot pose as an executable'):
            audit.validate_oracles(bad)

    def mutated_runner_route(self, identity):
        # Preserve the complete input set, copying only the row under mutation.
        # Check that row first so a negative control fails before rereading all
        # unrelated inputs. Positive inventory gates still check every input.
        row = copy.deepcopy(next(r for r in self.runner_routes["cases"] if r["case_id"] == identity))
        data = dict(self.runner_routes)
        data["cases"] = [row] + [r for r in self.runner_routes["cases"] if r["case_id"] != identity]
        return data, row

    def mutated_oracle(self, identity):
        # Retain the whole corpus and isolate only the deliberately wrong row.
        row = copy.deepcopy(next(r for r in self.oracles["cases"] if r["id"] == identity))
        data = dict(self.oracles)
        data["cases"] = [row] + [r for r in self.oracles["cases"] if r["id"] != identity]
        return data, row

    def test_complete_explicit_discovery_and_named_review_preserve_input_denominator(self):
        counts = self.validate_runner_routes(self.runner_routes)
        audit.require_full_explicit_runner_coverage(counts)
        self.assertEqual(counts["unreviewed_runner_inputs"], 0)
        self.assertEqual(counts["unreviewed_runner_entry_links"], 0)
        self.assertEqual(counts["unreviewed_named_regression_test_files"], 0)
        bad = copy.deepcopy(self.runner_routes)
        bad["cases"] = [r for r in bad["cases"] if not r["case_id"].endswith("/1500_project_mode_detect.xr")]
        counts = self.validate_runner_routes(bad)
        with self.assertRaisesRegex(ValueError, "lost original discovered"):
            audit.require_full_explicit_runner_coverage(counts)

    def test_accept_helpers_do_not_defer_close_before_launched_child_uses_handle(self):
        for filename, function, variable in (("1434_net_byte_array_loopback.xr", "acceptBytesEcho", "conn"),
                                              ("1434_net_byte_array_loopback.xr", "acceptBytesMaxLen", "conn"),
                                              ("1436_net_halfclose_bidi.xr", "acceptReadUntilEof", "c")):
            row = next(r for r in self.runner_routes["cases"] if r["case_id"].endswith("/"+filename))
            source = (audit.ROOT/row["target_source_port"]["fixture"]).read_text(encoding="utf-8")
            body = source.split("fn "+function+"(",1)[1].split("\n}",1)[0]
            self.assertIn("go ", body)
            self.assertNotIn("defer { net.close("+variable+") }", body)

    def test_websocket_full_roundtrips_and_first_local_counter_remain_required(self):
        expected = self.regression_library_expected("1438_ws_self_connect")
        for observed in (dict(expected, message_roundtrip_counts=[1,2,3]),
                         dict(expected, passed_final=1),
                         dict(expected, first_local_name="running"),
                         dict(expected, running_stopped_on_all_exits=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_tls_absolute_budget_and_actual_profile_cannot_be_skip_or_rearm(self):
        expected = self.regression_library_expected("1437_net_tls_timeout")
        for observed in (dict(expected, tls_provider_required=False),
                         dict(expected, absolute_deadline_not_per_yield_rearmed=False),
                         dict(expected, upgrade_failure_fd=9)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_http_binary_framings_and_body_publication_cannot_be_header_only(self):
        expected = self.regression_library_expected("1502_http_request_options")
        for observed in (dict(expected, three_response_framings=["content-length"]),
                         dict(expected, all_framings_raw_body=[0,127]),
                         dict(expected, request_body_tail="\r\n\r\n")):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_hpack_dynamic_state_and_control_frame_bytes_are_independent(self):
        expected = self.regression_library_expected("1512_http2_protocol")
        observed = copy.deepcopy(expected)
        observed["independent_hpack_vectors"]["decoded_blocks"]["request_second"][-1][1] = "wrong"
        with self.assertRaises(ValueError):
            audit.compare_required(expected, observed)
        observed = copy.deepcopy(expected)
        observed["independent_hpack_vectors"]["frame_vectors"]["window_update"] = "000004080000000001ffff0000"
        with self.assertRaises(ValueError):
            audit.compare_required(expected, observed)
        for observed in (dict(expected, valid_then_invalid_payload_leaves_all_prior_settings_unchanged=False),
                         dict(expected, header28_positive_admission_actually_called=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_cluster_opaque_queue_pressure_and_query_policy_cannot_be_shortcut(self):
        expected = self.regression_library_expected("1511_cluster_opaque_transport")
        for observed in (dict(expected, queued_envelopes=6),
                         dict(expected, envelope_bytes=63),
                         dict(expected, overflow_delivery="Accepted"),
                         dict(expected, opaque_buffer_mutable_view_and_owned_transport="PASS")):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_project_print_body_cannot_claim_config_or_actual_project_resolution(self):
        expected = self.regression_library_expected("1500_project_mode_detect")
        for observed in (dict(expected, original_case_directory_xray_toml="PRESENT"),
                         dict(expected, real_project_mode_resolution="PASS"),
                         dict(expected, runner_changes_cwd_to_case_directory=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_json_unicode_scalar_and_octet_duties_do_not_accept_erased_strings(self):
        expected = self.regression_library_expected("1002_json_string")
        for observed in (dict(expected, emoji_scalar_counts=[4,4,12]),
                         dict(expected, unicode_escape_values=expected["unicode_escape_values"][:-1]),
                         dict(expected, control_escape_values=["hello"]*10)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_json_numeric_independent_integer_and_binary64_answers_keep_precision(self):
        expected = self.regression_library_expected("1009_json_number")
        observed = copy.deepcopy(expected)
        observed["exact_integer_vectors"]["72057594037927935"] = 72057594037927936
        with self.assertRaises(ValueError):
            audit.compare_required(expected, observed)
        observed = copy.deepcopy(expected)
        observed["independent_ieee_f64_bits"]["-0.0"] = "0000000000000000"
        with self.assertRaises(ValueError):
            audit.compare_required(expected, observed)

    def test_json_complete_depth_large_array_and_key_pressure_cannot_be_smoke(self):
        expected = self.regression_library_expected("1007_json_fuzz")
        for observed in (dict(expected, deep_array_depth=2),
                         dict(expected, large_array_count=20),
                         dict(expected, many_key_count=5),
                         dict(expected, bool_null_values=[1,0,None,1,None,0])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_unpublished_json_query_cannot_pose_as_a_qualified_executable_target(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_library_review_1000_json')))
        row["execution_source"] = row["inputs"][0]["path"]
        with self.assertRaisesRegex(ValueError, "cannot pose as an executable"):
            audit.validate_oracles(bad)

    def test_absent_environment_variable_and_owned_temp_cleanup_must_be_restored(self):
        expected = self.regression_library_expected("1162_os_home")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, previously_absent_home_restored_to_absent=False))
        expected = self.regression_library_expected("1197_io_temp")
        for observed in (dict(expected, absent_TMPDIR_restored_to_absent=False),
                         dict(expected, all_acquired_temp_paths_have_failure_cleanup=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_owned_tree_parent_cleanup_precedes_child_creation_in_complete_draft(self):
        row = next(r for r in self.runner_routes["cases"] if r["case_id"].endswith("/1193_io_read_dir_recursive.xr"))
        source = (audit.ROOT / row["target_source_port"]["fixture"]).read_text(encoding="utf-8")
        self.assertIn('var parent = io.tempDir()!\n    defer { io.removeAll(parent) }\n    var root = tree(parent, "rdr_tree")', source)
        self.assertIn('fn tree(parent: Path, name: string)', source)
        self.assertNotIn('path.join(io.tempDir()!', source)

    def test_deep_refusal_retains_pressure_without_claiming_unpublished_atomic_policy(self):
        expected = self.regression_library_expected("1195_io_remove_all")
        for observed in (dict(expected, deep_levels=7),
                         dict(expected, whole_refusal_without_partial_deletion=True),
                         dict(expected, deep_cleanup_descending_level_order=list(range(70)))):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_invalid_utf8_raw_bytes_and_actual_null_cannot_be_empty_or_partial(self):
        for name in ("1193_io_invalid_utf8", "1198_io_invalid_utf8"):
            expected = self.regression_library_expected(name)
            for observed in (dict(expected, invalid_decoded_result=""),
                             dict(expected, invalid_raw_bytes=[255]),
                             dict(expected, invalid_null_payload_string_forbidden=False)):
                with self.assertRaises(ValueError):
                    audit.compare_required(expected, observed)

    def test_log_source_boolean_or_literal_marker_is_not_actual_callsite(self):
        expected = self.regression_library_expected("1202_log_source")
        for observed in (dict(expected, json_source_is_real_file_and_line_not_boolean_true=False),
                         dict(expected, text_source_is_real_location_not_literal_marker=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_buffered_logging_publishes_after_marker_then_flush_in_order(self):
        expected = self.regression_library_expected("1203_log_async")
        wrong = expected["body_publication_sequence"][1:4]+["before-flush"]+expected["body_publication_sequence"][4:]
        for observed in (dict(expected, body_publication_sequence=wrong),
                         dict(expected, queue_mode_implies_os_worker_offload=True),
                         dict(expected, flush_drains_and_releases_owned_queue_values=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_runtime_pressure_and_retained_payload_reads_cannot_be_counters_only(self):
        expected = self.regression_library_expected("1207_runtime_stress")
        for observed in (dict(expected, retained_strings=50),
                         dict(expected, retained_batch_cycles=[19]),
                         dict(expected, class_churn_nodes=5),
                         dict(expected, child_append_rounds=1),
                         dict(expected, collector_generational_strategy_qualified=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)
        expected = self.regression_library_expected("1206_runtime_enhanced")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, post_sample_payload_values=[]))

    def test_windows_real_byte_fixture_is_not_printf_skip_or_fabricated_exit_zero(self):
        expected = self.regression_library_expected("1161_os_extended")
        observed = copy.deepcopy(expected)
        observed["byte_command_windows"]["exit_code"] = 0
        with self.assertRaises(ValueError):
            audit.compare_required(expected, observed)
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, captured_stdout="123456789"))

    def test_compression_retains_original_and_real_large_payloads_and_all_levels(self):
        expected = self.regression_library_expected("1301_compress")
        for observed in (dict(expected, original_large_iterations=[50,50]),
                         dict(expected, target_additional_large_bytes=[22500,18500]),
                         dict(expected, all_compression_levels=[0,9]),
                         dict(expected, partial_decompression_published=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_yaml_block_content_and_multidocument_result_are_not_nonempty_smoke(self):
        expected = self.regression_library_expected("1304_yaml_advanced")
        for field, value in (("literal", "line one\nline two\nline three\n"),
                             ("literal_keep", "hello\nworld\n\n\n\n"),
                             ("folded", "line one\nline two\nline three")):
            observed = copy.deepcopy(expected)
            observed["block_scalar_exact_values"][field] = value
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, multi_documents=[{"first":1}]))

    def test_toml_literal_unicode_and_nan_cannot_be_wrong_nonempty_values(self):
        expected = self.regression_library_expected("1307_toml_advanced")
        for observed in (dict(expected, multiline_literal="different"),
                         dict(expected, unicode_heart="X"),
                         dict(expected, float_classes=["positive_infinity","negative_infinity","zero"])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_csv_report_partial_recovery_is_distinct_from_strict_refusal(self):
        expected = self.regression_library_expected("1305_csv_basic")
        for observed in (dict(expected, unterminated_partial_row=[]),
                         dict(expected, unterminated_report_diagnostics=0),
                         dict(expected, exact_delimiter_rune=44),
                         dict(expected, mixed_linebreak_metadata="lf")):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_xml_node_kinds_entities_and_diagnostic_channels_cannot_be_flattened(self):
        expected = self.regression_library_expected("1309_xml_advanced")
        for observed in (dict(expected, preserved_cdata={"kind":"text","content":"<not-xml>&"}),
                         dict(expected, unicode_entity_text=""),
                         dict(expected, unknown_entity_kind="SyntaxError"),
                         dict(expected, mixed_child_kinds=["text","text","text"])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_crypto_known_digest_content_cannot_be_length_only_success(self):
        expected = self.regression_library_expected("1400_crypto_hash")
        observed = copy.deepcopy(expected)
        observed["hash_vectors"]["sha256"]["hello"] = "0" * 64
        with self.assertRaises(ValueError):
            audit.compare_required(expected, observed)
        expected = self.regression_library_expected("1403_crypto_sha512")
        observed = copy.deepcopy(expected)
        observed["hmac_sha512_vectors"][0]["hex"] = "0" * 128
        with self.assertRaises(ValueError):
            audit.compare_required(expected, observed)

    def test_aead_fixed_vectors_and_padding_matrix_cannot_drop_content_or_dimensions(self):
        expected = self.regression_library_expected("1406_crypto_aead")
        for observed in (dict(expected, reference_wire_vectors=expected["reference_wire_vectors"][:1]),
                         dict(expected, padding_lengths_tested=list(range(16))),
                         dict(expected, plaintext_padding_each=[0]*34)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)
        observed = copy.deepcopy(expected)
        observed["reference_wire_vectors"][0]["tag_hex"] = "0"*64
        with self.assertRaises(ValueError):
            audit.compare_required(expected, observed)

    def test_aead_authentication_covers_iv_and_precedes_plaintext_publication(self):
        expected = self.regression_library_expected("1406_crypto_aead")
        for observed in (dict(expected, iv_is_authenticated=False),
                         dict(expected, tag_checked_before_decrypt_or_plaintext_publication=False),
                         dict(expected, truncated_length_channel="PANIC"),
                         dict(expected, all_three_tamper_regions_refused=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_random_uuid_hex_shape_is_not_entropy_or_generic_uuid_version(self):
        expected = self.regression_library_expected("1402_crypto_random")
        for observed in (dict(expected, lowercase_hex_only=False),
                         dict(expected, uuid_version_nibble="1"),
                         dict(expected, uuid_variant_nibbles=["0","1","2","3"]),
                         dict(expected, uuid_loop_calls=3),
                         dict(expected, shape_and_distinctness_establish_entropy=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_aes_unicode_body_and_authenticated_wire_overhead_cannot_be_omitted(self):
        expected = self.regression_library_expected("1404_crypto_aes")
        for observed in (dict(expected, target_additional_unicode_plaintext="Hello World!"),
                         dict(expected, target_unicode_utf8_bytes=3),
                         dict(expected, test_text_wire_hex_length=64),
                         dict(expected, tag_bytes=0)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_query_snapshot_and_percent_form_semantics_are_independent(self):
        expected = self.regression_library_expected("1411_url_parse")
        for observed in (dict(expected, snapshot_after_set_before_write="a=9"),
                         dict(expected, query_value_after_returned_snapshot_write="7"),
                         dict(expected, query_invalid_percent_offset=8),
                         dict(expected, query_duplicate_values=["3"])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)
        expected = self.regression_library_expected("1410_url_encode")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, percent_decode_plus_is_literal="hello world"))

    def test_calendar_iso_epoch_preservation_and_month_clamp_do_not_accept_shape_only(self):
        expected = self.regression_library_expected("1421_datetime_format")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, fixed_timestamp_iso="2024-06-15T00:00:00.000Z"))
        expected = self.regression_library_expected("1422_datetime_calc")
        for observed in (dict(expected, utc_local_preserve_epoch_and_ms=False),
                         dict(expected, month_add_target=[2024,3,2]),
                         dict(expected, month_add_source_after_call=[2024,2,29])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_regression_library_cannot_remove_a_whole_oracle_record(self):
        bad = copy.deepcopy(self.oracles)
        bad["cases"] = [r for r in bad["cases"] if r["id"] != "regression_library_review_1300_encoding"]
        with self.assertRaisesRegex(ValueError, "lost a complete original input"):
            audit.validate_oracles(bad)

    def regression_library_expected(self, name):
        return next(r["expected"] for r in self.oracles["cases"]
                    if r["id"] == "regression_library_review_" + name)

    def test_regression_library_cannot_omit_original_named_bodies(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('/1101_regex_engine.xr'))))
        row.pop("original_named_test_obligation")
        with self.assertRaisesRegex(ValueError, "named source obligations"):
            self.validate_runner_routes(bad)

    def test_regression_library_banner_or_partial_report_cannot_be_all_tests(self):
        for name in ("1100_regex", "1101_regex_engine", "1120_regex_re2_compat", "1180_base64_basic"):
            expected = self.regression_library_expected(name)
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, required_test_names=expected["required_test_names"][:-1]))

    def test_regression_library_oracle_cannot_drop_source_or_companion(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_library_review_1032_reexport')))
        row["inputs"] = [i for i in row["inputs"] if not i["path"].endswith("/product.xr")]
        with self.assertRaisesRegex(ValueError, "lost complete source or imported companion"):
            audit.validate_oracles(bad)

    def test_regression_library_oracle_cannot_run_only_root(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'regression_library_review_1131_regex_source_field_ownership')))
        row["execution_entry"] = "run"
        with self.assertRaisesRegex(ValueError, "full actual test entry"):
            audit.validate_oracles(bad)

    def test_imported_array_send_and_return_copies_cannot_alias(self):
        expected = self.regression_library_expected("1037_cross_module_owned_transfer_summary")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, received_first_after_source_write=[901, 902]))
        expected = self.regression_library_expected("1038_cross_module_return_ownership_summary")
        for observed in (dict(expected, logical_return_independent=False),
                         dict(expected, copy_after_owner_write_offset=1),
                         dict(expected, return_loop_iterations=[100,100,100,50])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_regex_engine_custom_decisions_cannot_be_replaced_by_external_defaults(self):
        expected = self.regression_library_expected("1101_regex_engine")
        for observed in (dict(expected, leftmost_longest_a_or_ab="a"),
                         dict(expected, class_backslash_d_matches_digit_5=True),
                         dict(expected, dollar_accepts_final_newline_without_multiline=True),
                         dict(expected, comma_split_count=256),
                         dict(expected, offset_anchor_at_start2=0)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_regex_warmup_and_retained_source_pressure_cannot_be_smoke(self):
        expected = self.regression_library_expected("1122_regex_jit_dynamic_null_index")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, warmup_iterations=1))
        expected = self.regression_library_expected("1131_regex_source_field_ownership")
        for observed in (dict(expected, churn_rounds_each=8),
                         dict(expected, strings_per_churn_round=4),
                         dict(expected, construct_read_drop_rounds=20),
                         dict(expected, returned_match_groups=["target77","target", "7"])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_regression_binary_property_lengths_differ_from_differential_corpus(self):
        expected = self.regression_library_expected("1181_binary_codec_properties")
        for observed in (dict(expected, generated_lengths=list(range(97))),
                         dict(expected, base64_alphabet_roundtrip_counts=[97,97]),
                         dict(expected, single_byte_utf8_cases=128),
                         dict(expected, valid_two_byte_utf8_pairs=64)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_true_simd_selection_or_xor_cannot_replace_vm_or_user_declaration(self):
        expected = self.regression_library_expected("1060_simd_intrinsic_same_name")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, ordinary_user_U32x4_bitXor=1))
        expected = self.regression_library_expected("1061_simd_runtime_selected_vm")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, vm_runtime_selected=True))

    def test_sleep_non_decreasing_clock_does_not_qualify_real_delay(self):
        expected = self.regression_library_expected("1010_time")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, sleep_elapsed_lower_bounds_ms=[0]))
        expected = self.regression_library_expected("1011_time_edge")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, sleep_requests_ms=[10,0,0]))

    def test_math_round_and_declared_numeric_types_cannot_use_erased_shape(self):
        expected = self.regression_library_expected("1022_math_type_preserve")
        for observed in (dict(expected, abs_min_max_clamp_type="i64"),
                         dict(expected, negative_round_half_away_from_zero=[-3,-3])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_retired_string_builder_replacement_keeps_whole_ascii_helper(self):
        row = next(r for r in self.runner_routes["cases"] if r["case_id"].endswith("/1150_string_byte_ops.xr"))
        source = (audit.ROOT / row["target_source_port"]["fixture"]).read_text(encoding="utf-8")
        self.assertNotIn("StringBuilder", source)
        self.assertIn("return string.join(parts)", source)
        self.assertIn("parts.push(comp)", source)
        self.assertIn("parts.push(c)", source)
        self.assertEqual(len(row["named_test_migration"]["mapping"]), 7)

    def library_review_expected(self, name):
        return next(r["expected"] for r in self.oracles["cases"]
                    if r["id"] == "library_review_" + name)

    def test_all_original_differential_inputs_have_explicit_case_records(self):
        counts = self.validate_runner_routes(self.runner_routes)
        self.assertEqual(counts["unreviewed_differential_inputs"], 0)

    def test_base64_property_corpus_cannot_drop_lengths_or_wrong_error_channels(self):
        expected = self.library_review_expected("base64_property_direct")
        for observed in (dict(expected, singleton_count=16), dict(expected, generated_lengths=list(range(96))),
                         dict(expected, alphabet_roundtrip_counts=[97, 0]),
                         dict(expected, stdout_utf8="true\ntrue\n0\n0\n0\n0\n0\n")):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_utf8_utf16_full_property_dimensions_and_offsets_cannot_be_approximated(self):
        expected = self.library_review_expected("encoding_property_direct")
        for observed in (dict(expected, single_byte_utf8_cases=128),
                         dict(expected, valid_two_byte_utf8_pairs=64),
                         dict(expected, utf16_le_hex="41003dd800de"),
                         dict(expected, stdout_utf8=expected["stdout_utf8"].replace("4002", "4000"))):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_cluster_whole_wire_bytes_and_retired_kind_cannot_be_collapsed(self):
        expected = self.library_review_expected("cluster_wire_protocol_pure_direct")
        for field in ("transport_frame", "request_frame", "ack_frame", "done_frame", "heartbeat_frame"):
            observed = copy.deepcopy(expected)
            observed[field][-1] ^= 1
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, retired_kind_09_rejected=False))

    def test_cluster_phi_window_and_handshake_proof_do_not_accept_reduced_duties(self):
        expected = self.library_review_expected("cluster_wire_protocol_pure_direct")
        for observed in (dict(expected, phi_window_sample_count=16),
                         dict(expected, envelope_length=8),
                         dict(expected, handshake_proof=expected["handshake_proof"][:-1])):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_http_body_consumption_counts_bytes_instead_of_unicode_scalars(self):
        for name in ("http_request_message_pure_direct", "http_response_text_pure_direct"):
            expected = self.library_review_expected(name)
            for observed in (dict(expected, unicode_body_bytes=2),
                             dict(expected, unicode_consumed_bytes=expected["unicode_consumed_bytes"] - 1)):
                with self.assertRaises(ValueError):
                    audit.compare_required(expected, observed)

    def test_http_conflicting_framing_and_injection_cannot_publish_messages(self):
        expected = self.library_review_expected("http_pure_helpers_direct")
        for field in ("conflicting_content_length_rejected", "transfer_encoding_override_rejected", "header_and_content_type_injection_rejected"):
            observed = dict(expected)
            observed[field] = False
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_websocket_mask_length_and_utf8_control_checks_remain_exact(self):
        expected = self.library_review_expected("ws_pure_protocol_direct")
        for observed in (dict(expected, masked_bytes=[1, 2, 3, 4, 5]),
                         dict(expected, length300_header=[130, 126, 44, 1]),
                         dict(expected, invalid_utf8_close_rejected=False),
                         dict(expected, oversized_control_frame_rejected=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_csv_missing_delimiter_cannot_fall_back_to_original_source(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('/csv_pure_module_direct.xr'))))
        row.pop("target_source_port")
        with self.assertRaisesRegex(ValueError, "requires its frozen source port"):
            self.validate_runner_routes(bad)

    def test_multipart_rejected_append_and_temporary_cleanup_remain_required(self):
        expected = self.library_review_expected("http_formdata_pure_direct")
        for observed in (dict(expected, limit_rejected_field_total_size=4),
                         dict(expected, limit_rejected_file_total_size=4),
                         dict(expected, temporary_directory_removed=False),
                         dict(expected, failure_path_cleanup_registered=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_cipher_verdicts_do_not_prove_authentication_or_entropy(self):
        expected = self.library_review_expected("crypto_timing_safe_equal_direct")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, authentication_claim_from_padding_absence_accepted=True))
        entropy = self.library_review_expected("crypto_random_system_direct")
        with self.assertRaises(ValueError):
            audit.compare_required(entropy, dict(entropy, actual_entropy_provider_qualified=False))

    def test_datetime_negative_milliseconds_do_not_truncate_toward_zero(self):
        expected = self.library_review_expected("datetime_core_direct")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, negative_ms_timestamp_and_remainder=[[0, -1], [-1, -1]]))

    def test_url_port_does_not_borrow_the_cluster_integer_payload_type(self):
        expected = self.library_review_expected("url_core_direct")
        self.assertEqual(expected["parsed_port"], "8080")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, parsed_port=8080))

    def provider_review_expected(self, name):
        return next(r["expected"] for r in self.oracles["cases"]
                    if r["id"] == "provider_review_" + name)

    def test_historical_anchor_does_not_remove_the_explicit_review_gap(self):
        identities = ("tests/diff/cases/basic/arith.xr", "tests/diff/cases/basic/loops.xr")
        source_rows = [next(r for r in self.runner_routes["cases"] if r["case_id"] == p) for p in identities]
        small = copy.deepcopy(self.runner_routes)
        small["cases"] = [source_rows[0]]
        actual = {r["case_id"]: r["original_runner_entries"] for r in source_rows}
        counts = audit.validate_runner_routes(small, self.mapping, actual)
        self.assertEqual(counts["unreviewed_runner_inputs"], 1)
        self.assertEqual(counts["unreviewed_runner_entry_links"], 1)
        self.assertEqual(counts["unreviewed_differential_inputs"], 1)

    def test_process_target_cannot_drop_or_change_the_argument_context(self):
        for missing in (True, False):
            bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('/process_schema_shared_core.xr'))))
            if missing:
                row["target_source_port"].pop("differential_input_obligation")
            else:
                row["target_source_port"]["differential_input_obligation"]["arguments"] = []
            with self.assertRaisesRegex(ValueError, "arguments/stdin context"):
                self.validate_runner_routes(bad)

    def test_process_complete_argument_observation_cannot_have_empty_arguments(self):
        expected = self.provider_review_expected("process_schema_shared_core")
        for args in ([], ["alpha"]):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, arguments=args))

    def test_stdin_crlf_bytes_cannot_be_normalized_or_replaced_by_eof(self):
        expected = self.provider_review_expected("io_read_stdin_shared_core")
        for observed in (dict(expected, stdout_utf8="17\ntrue\ntrue\ntrue\n"),
                         dict(expected, stdin_hex=""),
                         dict(expected, stdin_hex="616c7068610a626574610a6f6d6567610a")):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_provider_true_fallback_cannot_qualify_symlink_effects(self):
        expected = self.provider_review_expected("io_path_result_shared_core")
        for observed in ({"exit_code": 0, "stdout_utf8": "true\ntrue\n"},
                         dict(expected, fake_true_fallback_accepted=True),
                         dict(expected, symlink_creation_observed=False)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_bidirectional_count_and_first_byte_cannot_hide_payload_corruption(self):
        expected = self.provider_review_expected("net_copy_bidirectional_transfer")
        for field in ("left_to_right_bytes", "right_to_left_bytes"):
            observed = copy.deepcopy(expected)
            observed[field][1] = 0
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_cancel_output_does_not_prove_provider_wait_or_drain(self):
        expected = self.provider_review_expected("net_copy_bidirectional_cancel")
        for field in ("provider_wait_registered", "cancel_wakes_provider_wait", "provider_jobs_drained", "owned_handles_released"):
            observed = dict(expected)
            observed[field] = False
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_physical_shell_reuse_cannot_be_inferred_from_clean_local_values(self):
        expected = self.provider_review_expected("coro_typed_local_shell_reuse")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, physical_shell_reuse_observed=False))

    def test_full_parallel_fault_dimensions_and_recovery_cannot_be_reduced(self):
        expected = self.provider_review_expected("parallel_plan_map_cleanup_after_panic")
        for observed in (dict(expected, dispatch_sizes=[16, 16]),
                         dict(expected, fault_indices=[8, 8]),
                         dict(expected, subsequent_completed_items=[4, 0]),
                         dict(expected, dispatch_leases_released=False),
                         dict(expected, failure_channel="TYPED_ERROR")):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_os_thread_results_cannot_be_qualified_by_coroutine_substitution(self):
        expected = self.provider_review_expected("sys_thread_os_primitives_cross_thread")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, actual_os_threads_observed=False))

    def test_native_vector_banner_cannot_skip_widths_or_mutable_store_contract(self):
        expected = self.provider_review_expected("simd_portable_integer_vectors")
        for observed in ({"exit_code": 0, "stdout_utf8": "OK simd portable integer vectors\n"},
                         dict(expected, fixed_vector_width_bits=[128]),
                         dict(expected, explicit_mutable_store_view_required=False),
                         dict(expected, acceleration_claim_from_tautology_accepted=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_recursive_initializer_stack_overflow_is_not_the_guard_error(self):
        expected = self.provider_review_expected("sys_threadlocal_recursive_init")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stack_overflow_accepted=True))

    def test_path_conversion_cache_does_not_accept_a_neighbor_repository(self):
        allowed = audit.ROOT / "tests" / "unit" / "xir" / "legacy_consumers" / "audit.py"
        self.assertEqual(audit.relative(allowed), "tests/unit/xir/legacy_consumers/audit.py")
        self.assertEqual(audit.relative(allowed), "tests/unit/xir/legacy_consumers/audit.py")
        neighbor = audit.ROOT.parent / (audit.ROOT.name + "-other") / "audit.py"
        for _ in range(2):
            with self.assertRaises(ValueError):
                audit.relative(neighbor)

    def test_cached_path_conversion_still_reads_changed_fixture_contents(self):
        with tempfile.TemporaryDirectory(dir=audit.HERE) as directory:
            source = Path(directory) / "fresh.xr"
            source.write_text("print(1)\n", encoding="utf-8")
            record = audit.file_record(source)
            row = copy.deepcopy(self.oracles["cases"][0])
            row["legacy_case"] = None
            row["expected"] = {"exit_code": 0, "stdout_utf8": "1\n"}
            row["inputs"] = [{"path": record["path"], "lf_sha256": record["lf_sha256"], "role": "target_source"}]
            row["execution_source"] = record["path"]
            row["id"] = "path_cache_freshness_probe"
            data = copy.deepcopy(self.oracles)
            data["cases"].append(row)
            audit.validate_oracles(data)
            source.write_text("print(2)\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "oracle input changed"):
                audit.validate_oracles(data)
            source.unlink()
            self.assertEqual(audit.file_record(source)["state"], "MISSING")
            with self.assertRaisesRegex(ValueError, "oracle input changed"):
                audit.validate_oracles(data)

    def oracle_freshness_probe(self, directory):
        source = Path(directory) / "fresh.xr"
        source.write_bytes(b"print(1)\n")
        record = audit.file_record(source)
        data = copy.deepcopy(self.oracles)
        for index in range(3):
            row = copy.deepcopy(self.oracles["cases"][0])
            row.update(id=f"audit_freshness_probe_{index}", legacy_case=None,
                       expected={"exit_code": 0, "stdout_utf8": "1\n"},
                       inputs=[{"path": record["path"], "lf_sha256": record["lf_sha256"], "role": "target_source"}],
                       execution_source=record["path"])
            data["cases"].append(row)
        return source, data

    def test_shared_oracle_input_is_read_once_per_audit_and_again_on_next_call(self):
        from unittest import mock
        with tempfile.TemporaryDirectory(dir=audit.HERE) as directory:
            source, data = self.oracle_freshness_probe(directory)
            original = Path.read_bytes
            reads = []

            def read(path):
                if path == source:
                    reads.append(path)
                return original(path)

            with mock.patch.object(Path, "read_bytes", read):
                for expected_reads in (1, 2):
                    self.assertEqual(audit.validate_oracles(data)["oracle_cases"], 627)
                    self.assertEqual(len(reads), expected_reads)

    def test_same_length_same_timestamp_source_change_is_seen_on_next_audit(self):
        import os
        with tempfile.TemporaryDirectory(dir=audit.HERE) as directory:
            source, data = self.oracle_freshness_probe(directory)
            audit.validate_oracles(data)
            before = source.stat()
            source.write_bytes(b"print(2)\n")
            os.utime(source, ns=(before.st_atime_ns, before.st_mtime_ns))
            self.assertEqual(source.stat().st_size, before.st_size)
            self.assertEqual(source.stat().st_mtime_ns, before.st_mtime_ns)
            with self.assertRaisesRegex(ValueError, "oracle input changed"):
                audit.validate_oracles(data)

    def test_deleted_and_recreated_source_does_not_reuse_a_prior_audit_record(self):
        with tempfile.TemporaryDirectory(dir=audit.HERE) as directory:
            source, data = self.oracle_freshness_probe(directory)
            audit.validate_oracles(data)
            source.unlink()
            with self.assertRaisesRegex(ValueError, "oracle input changed"):
                audit.validate_oracles(data)
            source.write_bytes(b"print(1)\n")
            self.assertEqual(audit.validate_oracles(data)["oracle_cases"], 627)

    def test_reused_input_record_does_not_reuse_another_oracles_hash_claim(self):
        with tempfile.TemporaryDirectory(dir=audit.HERE) as directory:
            source, data = self.oracle_freshness_probe(directory)
            data["cases"][-2]["inputs"][0]["lf_sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "oracle input changed"):
                audit.validate_oracles(data)

    def test_file_record_keeps_raw_and_lf_identity_for_lf_crlf_and_lone_cr(self):
        import hashlib
        with tempfile.TemporaryDirectory(dir=audit.HERE) as directory:
            source = Path(directory) / "bytes.xr"
            for raw, normalized in ((b"", b""), (b"print(1)\n", b"print(1)\n"),
                                    (b"print(1)\r\n", b"print(1)\n"), (b"print(1)\r", b"print(1)\r")):
                source.write_bytes(raw)
                record = audit.file_record(source)
                self.assertEqual(record["bytes"], len(raw))
                self.assertEqual(record["sha256"], hashlib.sha256(raw).hexdigest())
                self.assertEqual(record["lf_sha256"], hashlib.sha256(normalized).hexdigest())

    def test_runner_audits_rediscover_created_and_deleted_expected_args_and_stdin_sidecars(self):
        with tempfile.TemporaryDirectory(dir=audit.HERE) as directory:
            source = Path(directory) / "sidecars.xr"
            source.write_bytes(b"print(1)\n")
            identity = audit.relative(source)
            row = copy.deepcopy(next(r for r in self.runner_routes["cases"]
                                     if r["case_id"] == "tests/diff/cases/basic/arith.xr"))
            entries = [{"suite": "differential", "entry": "run"}]
            runner = audit.load_module("audit_sidecar_freshness_runner", "tests/diff/run_backend_diff.py")
            row.update(case_id=identity, source_lf_sha256=audit.file_record(source)["lf_sha256"],
                       original_runner_entries=entries,
                       original_differential_obligation=audit.differential_input_obligation(source, runner),
                       original_stdout_oracle={"path": identity + ".expected", "state": "MISSING", "lf_sha256": None})
            data = copy.deepcopy(self.runner_routes)
            data["cases"].append(row)
            actual = dict(self.actual_runner_entries)
            actual[identity] = entries

            def validate():
                return audit.validate_runner_routes(data, self.mapping, actual)

            self.assertEqual(validate()["reviewed_runner_inputs"], 2051)
            for path, content, message in ((Path(str(source) + ".expected"), b"1\n", "stdout oracle changed"),
                                           (source.with_suffix(".args"), b"changed\n", "lost arguments"),
                                           (source.with_suffix(".stdin"), b"alpha\r\n", "lost arguments")):
                path.write_bytes(content)
                with self.assertRaisesRegex(ValueError, message):
                    validate()
                path.unlink()
                self.assertEqual(validate()["reviewed_runner_inputs"], 2051)

    def test_mutated_control_rows_keep_the_full_corpus_and_cannot_mutate_shared_fixtures(self):
        import json
        for original, key, helper in ((self.oracles, "id", self.mutated_oracle),
                                      (self.runner_routes, "case_id", self.mutated_runner_route)):
            before = json.dumps(original, sort_keys=True)
            identity = original["cases"][-1][key]
            data, row = helper(identity)
            self.assertEqual(len(data["cases"]), len(original["cases"]))
            self.assertEqual({r[key] for r in data["cases"]}, {r[key] for r in original["cases"]})
            self.assertIs(data["cases"][0], row)
            if key == "id":
                row["inputs"][0]["lf_sha256"] = "0" * 64
            else:
                row["original_runner_entries"][0]["entry"] = "wrong"
            self.assertEqual(json.dumps(original, sort_keys=True), before)

    def execution_review_expected(self, name):
        return next(r["expected"] for r in self.oracles["cases"]
                    if r["id"] == "execution_review_" + name)

    def test_optimized_loop_exit_cannot_trust_the_header_bound(self):
        expected = self.execution_review_expected("known_loop_exit_dropped_by_full_pipeline")
        for backend in ("vm", "native"):
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8="4950\n100\n"))

    def test_optimized_continue_filter_cannot_be_dropped_by_both_backends(self):
        expected = self.execution_review_expected("known_continue_filter_dropped_by_full_pipeline")
        for backend in ("vm", "native"):
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8="45\n"))

    def test_loop_latch_zero_trip_and_select_effects_remain_observable(self):
        for name, wrong in (("pass_loop_rotate_guarded_body", "0\n1\n45\n"),
                            ("continue_labeled_outer", "3\n3\n"),
                            ("pass_ifconv_select_shape", "9\n9\n100\n200\n\n1\n100\n200\n")):
            expected = self.execution_review_expected(name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_large_positive_dimensions_cannot_be_shrunk_while_retaining_answer(self):
        for name, field, wrong in (("reg_pressure_over_254", "parameters_and_arguments", 255),
                                   ("upval_capture_over_254", "parameters_and_arguments", 255),
                                   ("var_count", "locals", 299),
                                   ("string_parts_over_64", "parts_per_construction", 64)):
            expected = self.execution_review_expected(name)
            observed = dict(expected)
            observed[field] = wrong
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_float_relation_does_not_copy_the_wrong_f32_comment(self):
        expected = self.execution_review_expected("relation_owner")
        lines = expected["stdout_utf8"].splitlines()
        self.assertEqual(lines[43], "true")
        lines[43] = "false"
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="\n".join(lines) + "\n"))

    def test_absent_callee_and_explicit_default_values_skip_side_effects(self):
        for name, wrong in (("optional_call", "42\n1\n-1\n101\n4.5\n102\n"),
                            ("default_evaluated_per_call", "1\n2\n99\n4\n4\n"),
                            ("explicit_null_does_not_use_default", "true\ntrue\ntrue\nlocal\nlocal\nremote\nJohn Unknown\nJohn Doe\n")):
            expected = self.execution_review_expected(name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_bound_method_retains_receiver_and_does_not_reevaluate_it(self):
        expected = self.execution_review_expected("method_as_value")
        for wrong in ("7\n7\n12\n12\n20\n1\n12\n2\n", "7\n7\n12\n12\n0\n1\n12\n1\n"):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_receiver_cleanup_occurs_after_result_materialization(self):
        expected = self.execution_review_expected("class_native_receiver_as_value")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="1\n101\n201\n201\n348\n"))

    def test_formatter_fatal_cannot_publish_prefix_or_pose_as_compile_failure(self):
        expected = self.execution_review_expected("print_group_atomic_on_fatal")
        for observed in (dict(expected, stdout_utf8="-- before --\nhead middle "),
                         dict(expected, failure_phase="CHECK"),
                         dict(expected, failure_domain="SUCCESS"),
                         dict(expected, unreachable_executed=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_print_zero_and_empty_positions_keep_exact_bytes(self):
        for name, wrong in (("print_zero_args", ""),
                            ("print_zero_and_value", "a\n"),
                            ("print_separator_exact", "x\n\na\nb\na 1 true\np q r s\na b c\n1 2\n")):
            expected = self.execution_review_expected(name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_field_and_read_parameter_values_do_not_alias_the_caller(self):
        for name, wrong in (("class_native_ref_field_ownership", "6\n30\n6\n6\n99\n99\n88\n88\n88\n"),
                            ("repr_c_struct_aggregate_param", "42\n45\n37\n37\n26\n9\n")):
            expected = self.execution_review_expected(name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_unit_null_and_raw_store_replacements_cannot_claim_execution_sources(self):
        for name in ("known_empty_call_result_is_null", "tbaa_raw_pointer_store_kills_array_load"):
            bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'execution_review_' + name)))
            row["execution_source"] = row["inputs"][0]["path"]
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "executable target source"):
                audit.validate_oracles(bad)

    def test_type_positive_named_manifest_cannot_be_omitted(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('1206_generic_functions.xr'))))
        row.pop("original_named_test_obligation")
        with self.assertRaisesRegex(ValueError, "falsely qualified discovery"):
            self.validate_runner_routes(bad)

    def test_generic_equal_migration_cannot_keep_future_instance_proof(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('1437_generic_method_param_contract.xr'))))
        row["target_disposition"]["kind"] = "INSTANCE_ONLY_EQUAL_PROOF"
        with self.assertRaisesRegex(ValueError, "required disposition"):
            self.validate_runner_routes(bad)

    def test_generic_method_storage_domain_is_not_narrowed_by_one_equal_method(self):
        source = (audit.HERE / "fixtures/method_owned_equal_constraint/root.xr").read_text(encoding="utf-8")
        self.assertIn("class Box<T>", source)
        self.assertIn("touch(value: ref T) where T: Equal", source)
        self.assertNotIn("class Box<T: Equal>", source)

    def test_read_retained_array_copies_cannot_alias_source_after_return_or_store(self):
        for prefix in ("1454", "1455", "1456", "1457", "1458", "1459", "1460", "1461", "1462", "1463", "1466", "1467", "1468", "1469", "1470"):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"].startswith("read_retention_" + prefix))
            lines = expected["stdout_utf8"].splitlines()
            first = lines.index("1")
            lines[first] = "9"
            for backend in ("vm", "native"):
                with self.subTest(prefix=prefix, backend=backend), self.assertRaises(ValueError):
                    audit.compare_required(expected, dict(expected, stdout_utf8="\n".join(lines) + "\n"))

    def test_read_captured_lengths_do_not_follow_caller_growth(self):
        for prefix, wrong in (("1464", "3\n4\n4\n"), ("1465", "4\n5\n5\n")):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"].startswith("read_retention_" + prefix))
            with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=wrong))

    def test_native_provider_early_return_cannot_qualify_execution(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "native_type_net_test_cannot_succeed_by_early_return")
        for observed in ({"exit_code": 0, "all_required_tests_passed": True},
                         dict(expected, normal_listener_and_connection_observed=False),
                         dict(expected, accepted_connections=0), dict(expected, provider_skip_accepted=True)):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_json_extracted_map_must_be_a_named_independent_value(self):
        source = (audit.HERE / "fixtures/json_as_object_logical_value/root.xr").read_text(encoding="utf-8")
        self.assertIn("var extracted = obj!", source)
        self.assertIn('JSON.get<i64>(value, ["z"]), null', source)
        self.assertIn("value = extracted", source)
        self.assertNotIn("obj!.set", source)

    def test_full_generic_function_port_keeps_all_twenty_six_original_tests(self):
        row = next(r for r in self.runner_routes["cases"] if r["case_id"].endswith("1206_generic_functions.xr"))
        self.assertEqual(len(row["original_named_test_obligation"]["declarations"]), 26)
        self.assertEqual(len(row["named_test_migration"]["mapping"]), 26)
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "generic_functions_require_equal_at_definition")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, required_test_names=expected["required_test_names"][:-1]))

    def test_nested_observations_keep_exact_numeric_and_boolean_types(self):
        expected = {"bytes": [0, 1], "outcome": {"value": 1, "valid": True}}
        audit.compare_required(expected, {"bytes": [0, 1], "outcome": {"value": 1, "valid": True, "detail": "extra"}})
        for observed in ({"bytes": [False, True], "outcome": {"value": 1, "valid": True}},
                         {"bytes": [0, 1], "outcome": {"value": True, "valid": True}},
                         {"bytes": [0, 1], "outcome": {"value": 1, "valid": 1}},
                         {"bytes": [0.0, 1], "outcome": {"value": 1, "valid": True}}):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, observed)

    def test_nested_observations_cannot_lose_fields_or_reorder_values(self):
        expected = {"events": [{"kind": "store", "value": 1}, {"kind": "load", "value": 2}]}
        for events in ([{"kind": "store"}, {"kind": "load", "value": 2}],
                       list(reversed(expected["events"])), expected["events"][:-1]):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, {"events": events})

    def test_target_borrow_return_helper_cannot_be_dropped_or_stale(self):
        for change in ("drop", "stale"):
            bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('slice_borrow_return_cross_module.xr'))))
            if change == "drop":
                row["target_source_port"]["companion_inputs"] = []
            else:
                row["target_source_port"]["companion_inputs"][0]["lf_sha256"] = "0" * 64
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "companion"):
                self.validate_runner_routes(bad)

    def test_pending_view_member_replacement_is_not_an_executable_port(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'byte_explicit_endian_float_observations')))
        row["execution_source"] = row["inputs"][0]["path"]
        with self.assertRaisesRegex(ValueError, "cannot pose as an executable"):
            audit.validate_oracles(bad)

    def test_negative_array_ranges_cannot_be_clamped_into_success(self):
        for identity in ("array_negative_capacity_resize_no_publication", "array_typed_fill_strict_bounds_and_return_copy"):
            expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == identity)
            for backend in ("vm", "native"):
                with self.subTest(identity=identity, backend=backend), self.assertRaises(ValueError):
                    audit.compare_required(expected, dict(expected, stdout_utf8=expected["stdout_utf8"].replace("422\n", "0\n")))

    def test_view_materialization_keeps_an_independent_owned_result(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "view_shared_then_exclusive_then_owned_copy")
        with self.assertRaises(ValueError):
            audit.compare_required(expected, dict(expected, stdout_utf8="21\n31\n22\n32\n"))

    def test_assignment_selectors_must_precede_rhs_and_execute_once(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "evaluation_order_e5_e6_assignment_order")
        for output in (expected["stdout_utf8"].replace("index\nvalue\n", "value\nindex\n"),
                       expected["stdout_utf8"].replace("receiver\ndelta\n", "receiver\nreceiver\ndelta\n")):
            with self.assertRaises(ValueError):
                audit.compare_required(expected, dict(expected, stdout_utf8=output))

    def test_required_byte_observations_cannot_accept_boolean_aliases(self):
        expected = next(r["expected"] for r in self.oracles["cases"] if r["id"] == "byte_explicit_endian_float_observations")
        wrong = copy.deepcopy(expected)
        wrong["printed_bytes"][0] = False
        for backend in ("vm", "native"):
            with self.subTest(backend=backend), self.assertRaises(ValueError):
                audit.compare_required(expected, wrong)

    def test_dual_entry_input_cannot_lose_either_original_entry(self):
        for suite in ("regression", "differential"):
            bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if len(r['original_runner_entries']) == 2)))
            row["original_runner_entries"] = [e for e in row["original_runner_entries"] if e["suite"] != suite]
            with self.subTest(suite=suite), self.assertRaisesRegex(ValueError, "original runner entry"):
                self.validate_runner_routes(bad)

    def test_unique_program_counts_do_not_replace_entry_responsibilities(self):
        counts = self.validate_runner_routes(self.runner_routes)
        self.assertEqual(counts["reviewed_dual_runner_inputs"], 66)
        self.assertEqual(counts["reviewed_runner_entry_links"], counts["reviewed_runner_inputs"] + 66)

    def test_differential_arguments_and_exact_stdin_cannot_be_lost(self):
        for field in ("arguments", "stdin_hex", "stdin_sidecar", "arguments_sidecar", "execution"):
            bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'original_differential_obligation' in r)))
            if field == "arguments":
                row["original_differential_obligation"][field] = ["unreviewed-argument"]
            elif field == "stdin_hex":
                row["original_differential_obligation"][field] = "00"
            elif field == "execution":
                row["original_differential_obligation"][field] = "PASS"
            else:
                row["original_differential_obligation"].pop(field)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "stdin bytes"):
                self.validate_runner_routes(bad)

    def test_coroutine_textual_tests_cannot_be_removed_from_test_obligation(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('1164_channel_recv_raw_match_nullable_payload.xr'))))
        row.pop("original_named_test_obligation")
        with self.assertRaisesRegex(ValueError, "falsely qualified discovery"):
            self.validate_runner_routes(bad)

    def test_sticky_array_result_cannot_keep_already_taken_target(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('080_transfer_task_second_await_rejected.xr'))))
        self.assertEqual(row["original_rejection_oracle"]["assertions"][0]["code"], "E0356")
        row["target_disposition"]["kind"] = "REJECTION_FOR_ALREADY_TAKEN"
        with self.assertRaisesRegex(ValueError, "required disposition"):
            self.validate_runner_routes(bad)

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
            bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'original_named_test_obligation' in r)))
            if change == "drop":
                row["original_named_test_obligation"]["declarations"].pop()
            else:
                row["original_named_test_obligation"]["execution"] = change
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "falsely qualified discovery"):
                self.validate_runner_routes(bad)

    def test_target_named_test_migration_must_cover_every_original_identity(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'named_test_migration' in r)))
        row["named_test_migration"]["mapping"].pop(next(iter(row["named_test_migration"]["mapping"])))
        with self.assertRaisesRegex(ValueError, "omitted"):
            self.validate_runner_routes(bad)

    def test_partial_reverse_program_cannot_replace_the_thirteen_test_suite(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'partial_target_program' in r)))
        row["partial_target_program"]["complete_file_replacement"] = True
        with self.assertRaisesRegex(ValueError, "remaining original tests"):
            self.validate_runner_routes(bad)

    def test_partial_reverse_program_cannot_delete_stringbuilder_obligations(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'partial_target_program' in r)))
        row["partial_target_program"]["remaining_original_tests"] = []
        with self.assertRaisesRegex(ValueError, "remaining original tests"):
            self.validate_runner_routes(bad)

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
        counts = self.validate_runner_routes(self.runner_routes)
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
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('/span_array_literal_escape.xr'))))
        row["target_disposition"] = {"kind": "POSITIVE_LOCAL_VIEW_TUPLE_AFTER_FROZEN_TASK313_PORT", "owner_package": "P10", "qualification": "NOT_RUN", "reason": "invalid container exemption"}
        with self.assertRaisesRegex(ValueError, "required disposition"):
            self.validate_runner_routes(bad)

    def test_imported_diagnostics_cannot_omit_companion_inputs(self):
        for identity in audit.IMPORTED_ORACLE_INPUTS:
            with self.subTest(identity=identity):
                bad, row = self.mutated_runner_route(identity)
                del row["original_imported_inputs"]
                with self.assertRaisesRegex(ValueError, "required companion inputs"):
                    self.validate_runner_routes(bad)

    def test_imported_effect_diagnostic_cannot_bind_changed_companion(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'original_imported_inputs' in r)))
        row["original_imported_inputs"][0]["lf_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "companion changed"):
            self.validate_runner_routes(bad)

    def test_oracle_cannot_execute_a_different_unbound_source(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'atomic_var_handle_root_parameter')))
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
                bad, row = self.mutated_runner_route(identity)
                del row["target_disposition"]
                with self.assertRaisesRegex(ValueError, "required disposition"):
                    self.validate_runner_routes(bad)

    def test_read_class_positive_requires_the_ported_source(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('/007_in_instance_mutating_method.xr'))))
        del row["target_source_port"]
        with self.assertRaisesRegex(ValueError, "requires its frozen source port"):
            self.validate_runner_routes(bad)

    def test_read_struct_cannot_inherit_read_class_mutation_permission(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('/read_struct_mutating_receiver.xr'))))
        row["target_disposition"] = {"kind": "POSITIVE_AFTER_FROZEN_CLASS_SURFACE_PORT",
                                     "owner_package": "P10", "qualification": "NOT_RUN", "reason": "invalid class-only permission"}
        with self.assertRaisesRegex(ValueError, "required disposition"):
            self.validate_runner_routes(bad)

    def test_incomplete_enum_witness_is_not_an_implemented_positive(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r['case_id'].endswith('/020_enum_implements_rejected.xr'))))
        row["target_disposition"]["kind"] = "POSITIVE_AFTER_FROZEN_CLASS_SURFACE_PORT"
        with self.assertRaisesRegex(ValueError, "required disposition"):
            self.validate_runner_routes(bad)

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
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'original_regression_obligation' in r)))
        row["original_regression_obligation"]["expected_exit"] = 1
        with self.assertRaisesRegex(ValueError, "regression positive"):
            self.validate_runner_routes(bad)

    def test_build_only_cannot_claim_test_discovery(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'original_regression_obligation' in r)))
        row["original_regression_obligation"]["test_discovery"] = "PASS"
        with self.assertRaisesRegex(ValueError, "test discovery"):
            self.validate_runner_routes(bad)

    def test_class_source_port_preserves_named_tests_and_frozen_surface(self):
        row = next(r for r in self.runner_routes["cases"]
                   if r["case_id"] == "tests/regression/05_functions/0512_defer_around_return_call.xr")
        source = (audit.ROOT / row["target_source_port"]["fixture"]).read_text(encoding="utf-8")
        self.assertIn("init()", source)
        self.assertNotIn("constructor()", source)
        self.assertNotIn("ref useWith", source)
        self.assertNotIn("ref useHandler", source)
        for name in ("defer_runs_after_a_returned_parameter_call", "defer_runs_after_a_returned_indirect_call"):
            self.assertIn("@test fn " + name, source)
        self.assertEqual(source.count("assertEqual(g.held, true)"), 4)

    def test_target_source_port_is_not_a_qualified_product(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'target_source_port' in r)))
        row["target_source_port"]["qualification"] = "PASS"
        with self.assertRaisesRegex(ValueError, "incorrectly qualified"):
            self.validate_runner_routes(bad)

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
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if 'target_disposition' in r)))
        row["target_disposition"]["qualification"] = "PASS"
        with self.assertRaisesRegex(ValueError, "cannot publish qualification"):
            self.validate_runner_routes(bad)

    def test_unknown_task_responsibility_is_not_a_known_family(self):
        bad = copy.deepcopy(self.runner_routes)
        bad["cases"][0]["primary_family"] = "task318:Q99"
        with self.assertRaisesRegex(ValueError, "unknown family"):
            self.validate_runner_routes(bad)

    def test_imported_program_companion_is_part_of_oracle_identity(self):
        bad, row = self.mutated_oracle(next((r['id'] for r in self.oracles['cases'] if r['id'] == 'module_diamond_initializes_once')))
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
        self.validate_runner_routes(self.runner_routes)

    def test_runner_source_change_requires_semantic_review(self):
        bad = copy.deepcopy(self.runner_routes)
        bad["cases"][0]["source_lf_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "source changed"):
            self.validate_runner_routes(bad)

    def test_runtime_trap_cannot_replace_compile_entry(self):
        bad = copy.deepcopy(self.runner_routes)
        bad["cases"][0]["original_runner_entries"][0]["entry"] = "run"
        with self.assertRaisesRegex(ValueError, "runner entry"):
            self.validate_runner_routes(bad)

    def test_wrong_diagnostic_location_is_not_preserved_oracle(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r.get('original_rejection_oracle', {}).get('state') == 'PRESENT')))
        row["original_rejection_oracle"]["assertions"][0]["where"]["col"] += 1
        with self.assertRaisesRegex(ValueError, "position, code or text"):
            self.validate_runner_routes(bad)

    def test_position_gap_annotation_cannot_be_removed_to_pass(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if any((a['misplaced'] for a in r.get('original_rejection_oracle', {}).get('assertions') or [])))))
        next(a for a in row["original_rejection_oracle"]["assertions"] if a["misplaced"])["misplaced"] = None
        with self.assertRaisesRegex(ValueError, "position, code or text"):
            self.validate_runner_routes(bad)

    def test_missing_expected_file_stays_an_open_gap(self):
        bad, row = self.mutated_runner_route(next((r['case_id'] for r in self.runner_routes['cases'] if r.get('original_rejection_oracle', {}).get('state') == 'MISSING')))
        row["original_rejection_oracle"]["assertions"] = []
        with self.assertRaisesRegex(ValueError, "cannot be fabricated"):
            self.validate_runner_routes(bad)

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
