"""Pin complete independent traces and state distinctions before product runs."""
import math
import unittest
import complete_callable_model as model


class CallableTraces(unittest.TestCase):
    def test_complete_program_traces(self):
        expected = {
            'unproven_callee_targets':'-10\n14\n0\n2\n',
            'call_through_function_parameter':'42\n6\n',
            'capture_forms':'hi!\n1\n2\n3\n1\n4\n15\n104\n12\n',
            'nested_closure_transitive_capture':'21\n',
            'bare_lambda_expression_positions':'42\n310\n42\n',
            'default_param_as_function_value':'a:8080\nb:9\n',
            'function_type_surface':'42\n42\n20\n-10\n12\n-6\n36\n1001\n[100, 200, 300]\n',
            'lambda_forms':'42\n42\n7\n12\n99\n50\n42\n7\n20\n10\n30\n101\n',
            'lambda_inference_sources':'3\n12\n8\n9\n1.0\n10\n20\n10\n11\n11\n',
            'named_function_forms':'7\nlog explicit-unit\npong\nlocalhost:8080 tls=false\nlocalhost:443 tls=false\nlocalhost:443 tls=true\n3\n2\n42\nxyz\n15\n',
            'parenthesized_function_type':'1\n42\ntrue\n2\n',
            'string_result_direct_call':'pos\nneg\npos\npos\n',
            'unit_return_expression':'inner\ninner\n1\n2\n',
            'unit_value_rendering':'()\n(1, 2)\n',
            'known_empty_call_result_is_null':'()\n()\n',
        }
        traces = model.observations()
        self.assertEqual(set(traces),set(expected))
        self.assertEqual(sum(map(len,traces.values())),79)
        for name,text in expected.items():
            with self.subTest(program=name):
                self.assertEqual(''.join(model.render(value)+'\n' for value in traces[name]),text)

    def test_factory_cells_are_independent(self):
        first=model.counter();second=model.counter()
        self.assertEqual([first(),first(),second(),first(),second()],[1,2,1,3,2])

    def test_unit_and_singleton_keep_tuple_identity(self):
        self.assertEqual(model.render(()),'()')
        self.assertEqual(model.render((1,)),'(1,)')
        self.assertEqual(model.render([1,2]),'[1, 2]')
        for value in [None,object(),math.inf,math.nan]:
            with self.subTest(value_type=type(value).__name__):
                with self.assertRaises(ValueError):model.render(value)

    def test_repeated_derivation_keeps_fresh_cells(self):
        self.assertEqual(model.observations(),model.observations())


    def test_unit_call_trace_keeps_each_call_query_assert_print_in_order(self):
        trace=model.unit_call_trace()
        self.assertEqual(['run','empty','local_empty'],trace['calls'])
        self.assertEqual(['()','()'],trace['type_names'])
        self.assertEqual(2,trace['assertions'])
        self.assertEqual(['call:run','call:empty','typeName:result','assert:result','print:result',
                          'call:local_empty','typeName:nested','assert:nested','print:nested'],trace['events'])

    def test_unit_reflection_never_accepts_null_or_scalar_sentinels(self):
        self.assertEqual('()',model.unit_type_name(()))
        for value in (None,0,False,[], '()',(1,)):
            with self.subTest(value_type=type(value).__name__),self.assertRaises(ValueError):
                model.unit_type_name(value)

    def test_unit_port_changes_only_two_labels_and_inaccurate_comment(self):
        original=(model.ROOT/model._UNIT_ORIGINAL).read_text()
        selected=(model.ROOT/model._UNIT_PORT).read_text()
        self.assertEqual(model.unit_call_port(original),selected)
        self.assertEqual(3,selected.count('fn '))
        self.assertEqual(2,selected.count('assert(typeName('))
        self.assertEqual(2,selected.count('    print('))
        self.assertIn('var result = empty()',selected)
        self.assertIn('var nested = local_empty()',selected)
        self.assertNotIn('== "null"',selected)
        self.assertEqual(original,selected.replace('// value the caller may bind. It is Unit (), distinct from null. Inlining the',
                         '// value the caller may bind, and that value compares equal to null. Inlining the').replace('== "()"','== "null"'))

    def test_unit_port_rejects_lost_or_already_migrated_labels(self):
        original=(model.ROOT/model._UNIT_ORIGINAL).read_text()
        changed_sources=(original.replace('== "null"','== "()"',1),
                         original.replace('// value the caller may bind,','// unrelated comment,'),
                         model.unit_call_port(original))
        for changed in changed_sources:
            with self.assertRaises(ValueError):model.unit_call_port(changed)

    def test_unit_preservation_guard_rejects_synthetic_print_even_with_new_digest(self):
        import hashlib,tempfile
        from pathlib import Path
        from unittest.mock import patch
        original_root=model.ROOT
        with tempfile.TemporaryDirectory() as temporary:
            damaged=Path(temporary)/'damaged.xr'
            data=(model.ROOT/model._UNIT_PORT).read_bytes().replace(b'print(nested)',b'print(())')
            damaged.write_bytes(data)
            pins=dict(model._INPUTS);pins[model._UNIT_PORT]=hashlib.sha256(data.replace(b'\r\n',b'\n')).hexdigest()
            class SourceView:
                def __truediv__(self,name):return damaged if name==model._UNIT_PORT else original_root/name
            with patch.object(model,'ROOT',SourceView()),patch.object(model,'_INPUTS',pins):
                with self.assertRaisesRegex(ValueError,'changed more than'):model.expected_rows()

    def test_unit_selected_oracle_cannot_qualify_obsolete_original(self):
        import complete_target_selection as select
        import json
        preserved=json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/oracles.json').read_text())['cases']
        selected={'case_id':model._UNIT_ORIGINAL,'selected_source':model._UNIT_PORT,
                  'selected_source_lf_sha256':model._INPUTS[model._UNIT_PORT],
                  'canonical_port_selected':True,'rejection_oracle_required':False}
        row=select.expected_for(selected,preserved,model.expected_rows())
        self.assertEqual(model._UNIT_PORT,row['source'])
        self.assertEqual('()\n()\n',row['expected']['stdout_utf8'])
        selected.update(selected_source=model._UNIT_ORIGINAL,canonical_port_selected=False,
                        selected_source_lf_sha256=model._INPUTS[model._UNIT_ORIGINAL])
        self.assertIsNone(select.expected_for(selected,preserved,model.expected_rows()))

    def test_unit_router_manifest_and_frozen_transition_name_same_complete_source(self):
        import audit,json
        routes=json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/runner_routes.json').read_text())['cases']
        route=next(x for x in routes if x['case_id']==model._UNIT_ORIGINAL)
        self.assertEqual(model._UNIT_PORT,route['target_source_port']['fixture'])
        self.assertEqual(model._INPUTS[model._UNIT_PORT],route['target_source_port']['lf_sha256'])
        self.assertEqual((route['target_disposition']['kind'],'P10'),audit.FROZEN_TARGETS[model._UNIT_ORIGINAL])
        manifest=json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_text())['cases']
        row=next(x for x in manifest if x['id']=='complete_callable_'+model._UNIT_NAME)
        self.assertEqual(model._UNIT_PORT,row['source'])
        self.assertEqual(model._UNIT_ORIGINAL,row['preserved_original_identity']['source'])
        self.assertEqual('()\n()\n',row['stdout_utf8'])


if __name__=='__main__':unittest.main()
