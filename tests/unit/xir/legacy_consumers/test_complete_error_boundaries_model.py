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

if __name__=='__main__':unittest.main()
