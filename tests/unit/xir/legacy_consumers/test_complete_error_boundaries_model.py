from decimal import Decimal
import unittest
import complete_error_boundaries_model as model

class ErrorBoundaries(unittest.TestCase):
    def test_complete_observations(self):
        expected={'array_index_set_append_oob':['[Uncaught Panic] E0430: array index out of range: 3 (length 3)'],
         'array_index_set_gap_oob':['[Uncaught Panic] E0430: array index out of range: 5 (length 3)'],
         'array_index_set_negative_oob':['[Uncaught Panic] E0430: array index out of range: -1 (length 3)'],
         'fixed_array_index_oob_panic_shape':['fixed array index out of range: -1 (length 4)',430,'fixed array index out of range: 4 (length 4)',430,'fixed array index out of range: 5 (length 4)',430],
         'string_number_conversion_panic_shape':[-7,'i64.InvalidSyntax','overflow.OutOfRange','1500.0','inf','f64.InvalidSyntax',True,True]}
        self.assertEqual(sum(map(len,expected.values())),17)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(model.observations()[name],values)

    def test_strict_integer_syntax_and_range(self):
        for text,variant in [('12abc','InvalidSyntax'),('0x12','InvalidSyntax'),('','InvalidSyntax'),('1.0','InvalidSyntax'),('9223372036854775808','OutOfRange'),('-9223372036854775809','OutOfRange')]:
            with self.subTest(text=text):
                with self.assertRaises(model.ParseFailure) as error:model.parse_integer(text)
                self.assertEqual(error.exception.variant,variant)
        self.assertEqual(model.parse_integer('-9223372036854775808'),-(1<<63));self.assertEqual(model.parse_integer('+9223372036854775807'),(1<<63)-1)

    def test_strict_float_grammar_and_unambiguous_values(self):
        for text in ['1e+','nan','inf','0x1p2','1.2.3','']:
            with self.subTest(text=text):
                with self.assertRaises(model.ParseFailure):model.parse_float(text)
        self.assertEqual(model.parse_float('.5'),Decimal('0.5'));self.assertEqual(model.parse_float('1.'),Decimal(1))
        self.assertEqual(model.parse_float('1.5e3'),Decimal(1500));self.assertEqual(model.render_float(model.parse_float('1e400')),'inf')

    def test_bounds_are_checked_before_success_continuations(self):
        for length,index,valid in [(3,-1,False),(3,2,True),(3,3,False),(3,5,False),(4,4,False)]:
            with self.subTest(length=length,index=index):self.assertEqual(model.checked_index(length,index) is None,valid)



class StrictMapBoundaries(unittest.TestCase):
    def test_two_full_original_programs_and_all12_observations(self):
        traces=model.map_boundary_traces()
        self.assertEqual(traces['map_index_missing_key_panic']['outputs'],[7,'map key not found',431,True,True,True])
        self.assertEqual(traces['map_set_static_missing_key_prehashed']['outputs'],[True,False,7,True,431,'map key not found'])
        self.assertEqual(sum(len(x['outputs']) for x in traces.values()),12)
        self.assertTrue(all(not x['bad_index_continuation_executed'] for x in traces.values()))

    def test_nullable_present_and_absent_are_different_key_states(self):
        mapping=model.StringKeyMap({'present-null':None,'present-value':5})
        self.assertTrue(mapping.contains('present-null'));self.assertIsNone(mapping.strict('present-null'))
        self.assertFalse(mapping.contains('absent'));self.assertIsNone(mapping.optional('absent'))
        with self.assertRaises(model.MissingMapKey):mapping.strict('absent')
        self.assertEqual(mapping.strict('present-value'),5)

    def test_missing_index_interrupts_print_argument_and_does_not_mutate(self):
        mapping=model.StringKeyMap({'ada':7});before=dict(mapping.items)
        with self.assertRaises(model.MissingMapKey) as panic:mapping.strict('missing')
        self.assertEqual((panic.exception.code,panic.exception.message),(431,'map key not found'))
        self.assertEqual(mapping.items,before)
        self.assertEqual(model._caught_missing(mapping),(['map key not found',431],False))
        self.assertEqual(model._caught_missing(mapping,True),([431,'map key not found'],False))

    def test_returning_null_for_strict_absence_fails_the_whole_program(self):
        from unittest import mock
        with mock.patch.object(model.StringKeyMap,'strict',lambda mapping,key:mapping.items.get(key)),self.assertRaisesRegex(ValueError,'did not interrupt'):
            model.expected_rows()

    def test_readonly_candidates_keep_all_original_entries_and_no_storage_claim(self):
        trace=model.map_boundary_traces()['map_set_static_missing_key_prehashed']
        self.assertEqual(trace['scores_after_caught_panic'],{'ada':7,'lin':9})
        self.assertEqual(trace['seen_after_reads'],['ada','lin'])
        self.assertEqual((trace['original_top_level_const_map_entries'],trace['original_top_level_const_set_entries']),(2,2))
        self.assertIn('OPEN',trace['actual_static_prehashed_emission_or_native_readonly_storage'])
        with self.assertRaises(ValueError):model.StringKeyMap({1:5})

    def test_full_input_and_diagnostic_authorities_are_pinned(self):
        from pathlib import Path
        from unittest import mock
        saved=Path.read_bytes
        for name in model._MAP_INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'changed' if path==model.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
        with mock.patch.dict(model._MAP_PROGRAMS,{},clear=True),self.assertRaises(ValueError):model.expected_rows()

    def test_product_rows_keep_complete_original_run_and_independent_bytes(self):
        import json
        from pathlib import Path
        cases={row['id']:row for row in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']}
        for row in model.expected_rows():
            if row['source'] not in model._MAP_PROGRAMS.values():continue
            case=cases[row['id']];self.assertEqual(case['source'],row['source'])
            self.assertEqual(case['source'],case['preserved_original_identity']['source'])
            self.assertEqual(case['entry'],'run');self.assertEqual(case['source_context'],'original_absolute')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8'])
            self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')


    def test_reference_is_committed_target_source_not_retired_backend_or_product_output(self):
        reference=model.map_diagnostic_reference()
        self.assertEqual(reference['target_code'],431);self.assertEqual(reference['target_message'],'map key not found')
        self.assertEqual(reference['retired_message_for_history'],'Map key not found')
        self.assertEqual(reference['fixed_source_commit'],'460622cf45c82ab0bc93be14d2f0ac3fce21bef6')
        self.assertNotIn('src/aot/xrt_coll.h',model._INPUTS)
        self.assertEqual(len(model._MAP_REFERENCE_FILES),3)
        self.assertFalse(reference['SDK_FI_lifetime_provider_or_readonly_storage_qualified'])

    def test_default_message_is_exact_and_cannot_accept_legacy_case_or_whitespace(self):
        from unittest import mock
        for message in ('Map key not found','map key not found ','MAP KEY NOT FOUND'):
            with self.subTest(message=message),mock.patch.object(model.MissingMapKey,'message',message),self.assertRaises(ValueError):model.expected_rows()
        self.assertEqual(model.MissingMapKey.message.encode('utf-8'),b'map key not found')

    def test_reference_metadata_rejects_wrong_commit_code_message_or_snapshot_identity(self):
        from pathlib import Path
        from unittest import mock
        import copy,json
        original=json.loads((model.ROOT/model._MAP_REFERENCE_PATH).read_text(encoding='utf-8'));saved=Path.read_text
        variants=[]
        for key,value in [('fixed_source_commit','0'*40),('canonical_fault',{'code':430,'message':'map key not found'}),('canonical_fault',{'code':431,'message':'Map key not found'})]:
            changed=copy.deepcopy(original);changed[key]=value;variants.append(changed)
        changed=copy.deepcopy(original);changed['source_snapshots'][0]['git_blob_oid']='0'*40;variants.append(changed)
        changed=copy.deepcopy(original);changed['source_snapshots']=changed['source_snapshots'][:-1];variants.append(changed)
        changed=copy.deepcopy(original);changed['source_snapshots'][0]['snapshot_path']='../outside';variants.append(changed)
        for value in variants:
            def altered(path,*args,**kwargs):return json.dumps(value) if path==model.ROOT/model._MAP_REFERENCE_PATH else saved(path,*args,**kwargs)
            with mock.patch.object(Path,'read_text',altered),self.assertRaises(ValueError):model.expected_rows()

    def test_target_acceptance_keeps_full_sources_and_storage_gate_open(self):
        rows=[row for row in model.expected_rows() if row['source'] in model._MAP_PROGRAMS.values()]
        self.assertEqual(len(rows),2);self.assertEqual(sum(row['observation_count'] for row in rows),12)
        for row in rows:
            self.assertEqual(row['source'],row['original_source']);self.assertEqual(row['diagnostic_acceptance_basis']['target_message'],'map key not found')
            self.assertFalse(row['required_semantic_trace']['bad_index_continuation_executed'])
        trace=model.map_boundary_traces()['map_set_static_missing_key_prehashed']
        self.assertIn('OPEN',trace['actual_static_prehashed_emission_or_native_readonly_storage'])

class NullUnwrapBoundary(unittest.TestCase):
    def test_whole_port_changes_only_the_obsolete_comment_and_catch_channel(self):
        original=(model.ROOT/model._NULL_ORIGINAL).read_text(encoding='utf-8')
        target=(model.ROOT/model._NULL_PORT).read_text(encoding='utf-8')
        body=original[original.index('var value: i64? = null'):]
        self.assertEqual(target[target.index('var value: i64? = null'):],body.replace('} catch (e) {','} catch panic (e) {'))
        self.assertEqual(original.count('print('),3);self.assertEqual(target.count('print('),3)
        self.assertIn('print(value!)',target);self.assertIn('print(e.message)\n    print(e.code)',target)

    def test_optional_i64_absence_panics_while_present_zero_is_a_value(self):
        for value in (0,7,-(1<<63),(1<<63)-1):self.assertEqual(model.unwrap_i64(value),value)
        with self.assertRaises(model.NullUnwrapPanic) as panic:model.unwrap_i64(None)
        self.assertEqual((panic.exception.code,panic.exception.message),(413,'cannot unwrap a null value'))
        for value in (False,True,'0',1<<63):
            with self.subTest(value=value),self.assertRaises(ValueError):model.unwrap_i64(value)
        self.assertFalse(issubclass(model.NullUnwrapPanic,Exception))

    def test_failed_argument_emits_no_value_and_only_the_two_panic_observations(self):
        trace=model.null_unwrap_trace()
        self.assertEqual(trace['outputs'],['cannot unwrap a null value',413])
        self.assertEqual(trace['optional_reads'],1);self.assertFalse(trace['normal_print_argument_emitted'])
        self.assertFalse(trace['ordinary_value_error_catch_granted'])
        from unittest import mock
        for code,message in ((410,'cannot unwrap a null value'),(413,'Cannot unwrap a null value'),(413,'cannot unwrap a null value ')):
            with self.subTest(code=code,message=message),mock.patch.object(model.NullUnwrapPanic,'code',code),mock.patch.object(model.NullUnwrapPanic,'message',message),self.assertRaises(ValueError):model.expected_rows()

    def test_committed_reference_keeps_code_message_and_legacy_spec_conflict_explicit(self):
        reference=model.null_unwrap_reference()
        self.assertEqual((reference['code'],reference['message_UTF8_bytes'],reference['channel']),(413,26,'PANIC'))
        self.assertEqual(reference['legacy_spec004_example_reconciliation'],'OPEN')
        self.assertFalse(reference['SDK_FI_lifetime_provider_qualified'])
        from pathlib import Path
        from unittest import mock
        import json,copy
        original=json.loads((model.ROOT/model._NULL_REFERENCE).read_text(encoding='utf-8'));saved=Path.read_text
        variants=[]
        changed=copy.deepcopy(original);changed['canonical_fault']['code']=410;variants.append(changed)
        changed=copy.deepcopy(original);changed['fixed_source_commit']='0'*40;variants.append(changed)
        changed=copy.deepcopy(original);changed['source_snapshots'][-1]['git_blob_oid']='0'*40;variants.append(changed)
        changed=copy.deepcopy(original);changed['source_snapshots'][-1]['snapshot_path']='../outside';variants.append(changed)
        changed=copy.deepcopy(original);changed['legacy_example_conflict']['state']='PASS';variants.append(changed)
        for value in variants:
            def altered(path,*args,**kwargs):return json.dumps(value) if path==model.ROOT/model._NULL_REFERENCE else saved(path,*args,**kwargs)
            with mock.patch.object(Path,'read_text',altered),self.assertRaises(ValueError):model.expected_rows()

    def test_single_canonical_selection_and_product_keep_original_identity_and_bytes(self):
        import json,audit,complete_target_selection as selection
        rows=json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/runner_routes.json').read_text(encoding='utf-8'))['cases']
        original=next(row for row in rows if row['case_id']==model._NULL_ORIGINAL)
        runner=audit.load_module('null_unwrap_selection_runner','tests/diff/run_backend_diff.py')
        selected=selection.select(original,model.ROOT,runner)
        self.assertTrue(selected['canonical_port_selected']);self.assertFalse(selected['rejection_oracle_required'])
        self.assertEqual(selected['selected_source'],model._NULL_PORT);self.assertEqual(selected['arguments'],[]);self.assertEqual(selected['stdin_hex'],'')
        row=selection.expected_for(selected,[],model.expected_rows())
        self.assertEqual(row['original_source'],model._NULL_ORIGINAL);self.assertEqual(row['observation_count'],2)
        self.assertEqual(row['expected'],{'exit_code':0,'stdout_utf8':'cannot unwrap a null value\n413\n','stderr_utf8':''})
        products=json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_text(encoding='utf-8'))['cases']
        product=next(case for case in products if case['id']==row['id'])
        self.assertEqual(product['source'],model._NULL_PORT);self.assertEqual(product['preserved_original_identity']['source'],model._NULL_ORIGINAL)
        self.assertEqual(product['stdout_utf8'],row['expected']['stdout_utf8']);self.assertEqual(product['entry'],'run')

    def test_complete_json_require_value_channel_keeps_all_five_failures(self):
        trace=model.json_value_path_trace()
        self.assertEqual(trace['outputs'],['map=7','caught-map-key','caught-map-value','caught-fn','caught-var','caught-return'])
        self.assertEqual(trace['ordinary_error_catches'],['map-key','map-value','fn','var','return'])
        self.assertEqual(trace['panic_catches'],0);self.assertEqual(trace['final_map_entries'],[(True,7)])
        self.assertEqual(trace['takes_bool_body_calls'],0);self.assertEqual(trace['original_print_expressions'],14)
        self.assertEqual(trace['typed_path_reads'],[('goodBool','bool'),('goodInt','i64'),('badBool','bool'),('goodBool','bool'),('badInt','i64'),('badBool','bool'),('badInt','i64'),('badInt','i64')])

    def test_json_require_exact_types_and_ordinary_failure_are_separate_from_panic(self):
        self.assertEqual(model.json_require_exact({'x':7},'x','i64'),7)
        self.assertIs(model.json_require_exact({'x':True},'x','bool'),True)
        for value,schema in ((1,'bool'),(True,'i64'),(3.5,'i64'),(None,'bool')):
            with self.subTest(value=value),self.assertRaises(model.JsonPathValueFailure):model.json_require_exact({'x':value},'x',schema)
        self.assertTrue(issubclass(model.JsonPathValueFailure,Exception));self.assertFalse(issubclass(model.NullUnwrapPanic,Exception))
        with self.assertRaises(model.JsonPathValueFailure):model.json_require_exact({},'x','i64')

    def test_json_require_port_only_changes_five_catch_qualifiers(self):
        original=(model.ROOT/model._JSON_PATH_ORIGINAL).read_text();port=(model.ROOT/model._JSON_PATH_PORT).read_text()
        self.assertEqual(original.count('catch panic(e)'),5);self.assertEqual(port.count('catch(e)'),5)
        self.assertEqual(original.replace('catch panic(e)','catch(e)'),port);self.assertEqual(original.count('print('),14);self.assertEqual(port.count('print('),14)
        self.assertIn('fn takes_bool(x: bool)',port);self.assertIn('var m: Map<bool, i64> = #{}',port)

    def test_json_require_exact_positive_oracle_has_no_payload_or_rejection_substitute(self):
        row=next(x for x in model.expected_rows() if x['original_source']==model._JSON_PATH_ORIGINAL)
        self.assertEqual(row['expected'],{'exit_code':0,'stdout_utf8':'map=7\ncaught-map-key\ncaught-map-value\ncaught-fn\ncaught-var\ncaught-return\n','stderr_utf8':''})
        self.assertEqual(row['observation_count'],6);self.assertEqual(row['required_semantic_trace']['unobserved_PathError_payload_message_code'],'NOT_INVENTED')
        import hashlib
        from pathlib import Path
        from unittest import mock
        saved=Path.read_bytes;path=model.ROOT/model._JSON_PATH_PORT;changed=saved(path)+b'\nprint(1)\n';pins=dict(model._INPUTS);pins[model._JSON_PATH_PORT]=hashlib.sha256(changed.replace(b'\r\n',b'\n')).hexdigest()
        def read(pathValue):return changed if pathValue==path else saved(pathValue)
        with mock.patch.object(model,'_INPUTS',pins),mock.patch.object(Path,'read_bytes',read),self.assertRaisesRegex(ValueError,'more than five catch'):
            model.expected_rows()

if __name__=='__main__':unittest.main()
