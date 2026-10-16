"""Pin complete imported observations and initialization failure boundaries."""
import unittest
import complete_module_model as model


class ModuleTraces(unittest.TestCase):
    def test_complete_module_observations(self):
        expected={name:[] for name in ['_classlib','_default_arg_class_lib','_exported_struct_methods_lib','_inherit_lib','_ptr_offset_const_lib','_value_struct_arg_lib']}
        expected.update({'_dead_net_helper':[],'xmod_dead_net_helper':['dead-net-ok'],'xmod_ptr_offset_imported_const':[4096],'_cleanup_init_error':['error dependency body','error dependency cleanup'],
            'cleanup_init_error':['error dependency body','error dependency cleanup'],
            '_cleanup_init_panic':['panic dependency body','panic dependency cleanup'],
            'cleanup_init_panic':['panic dependency body','panic dependency cleanup'],
            'cross_module_borrowed_arg_release':[9,17,15,60,'Hi','a-b-c'],'exported_struct_methods':[24,23,30],
            'native_module_scalar_call_target':[7,3,9],'value_struct_arg_alias_import':[9],
            'value_struct_arg_without_type_import':[15,21,12,24],'value_struct_copy_without_type_import':[1,99],
            'value_struct_return_release':[49995000,True,True],'xmod_class':[15,18,101,141],
            'xmod_default_arg_class':[7002,7102,7202,7309,'def','def',8025,1,9002,9005,8026],
            'xmod_inherit':['dog:Rex',4,'Rex',4],'xmod_inherit_lib_subclass':[1,2,2,'sub',1],
            'xmod_namespace_generic':[10,20,'hi','hi!'],'xmod_nested_class_field':[42],
            'xmod_same_name_classes':[3,7,10,20],'xmod_same_name_methods':[100,200,3,30]})
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),69)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_required_termination_is_execution_not_compilation_refusal(self):
        for name in ['_cleanup_init_error','cleanup_init_error','_cleanup_init_panic','cleanup_init_panic','xmod_class']:
            with self.subTest(program=name):
                code,stderr=model.termination(name)
                if name.endswith('_error'):self.assertEqual((code,stderr),(1,'[Uncaught Error] InitError.Failed\n'))
                elif name.endswith('_panic'):self.assertEqual((code,stderr),(1,'[Uncaught Panic] E0420: division by zero\n'))
                else:self.assertEqual((code,stderr),(0,''))


    def test_unreachable_network_body_and_imported_pointer_constant_trace(self):
        traces=model.reachability_traces()
        self.assertEqual(traces['_dead_net_helper']['entered_function_bodies'],[])
        self.assertEqual(traces['xmod_dead_net_helper']['entered_function_bodies'],['label'])
        self.assertEqual(traces['_dead_net_helper']['unreachable_net_calls_executed'],0)
        self.assertEqual(traces['xmod_dead_net_helper']['unreachable_net_calls_executed'],0)
        pointer=traces['xmod_ptr_offset_imported_const'];self.assertEqual(pointer['outputs'],[4096])
        self.assertEqual((pointer['allocation_request_bytes'],pointer['foreign_malloc_calls'],pointer['foreign_free_calls'],pointer['pointer_offset_calls'],pointer['pointer_dereferences']),(8192,1,0,1,0))
        self.assertFalse(pointer['actual_foreign_allocation_or_pointer_provenance_qualified'])

    def test_new_complete_modules_keep_companions_and_reject_changed_original(self):
        from unittest.mock import patch
        rows={row['original_source']:row for row in model.expected_rows()}
        for name,path in model._REACHABILITY_PROGRAMS.items():
            with self.subTest(program=name):
                self.assertEqual(rows[path]['source'],path);self.assertIn('required_semantic_trace',rows[path])
                self.assertEqual(rows[path]['companions'],model._COMPANIONS[name])
                with patch.dict(model._INPUTS,{path:'0'*64}):
                    with self.assertRaisesRegex(ValueError,'source or authority changed'):model.expected_rows()

    def test_complete_module_product_companions_carry_exact_path_and_hash(self):
        import json
        cases={case['id']:case for case in json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_text(encoding='utf-8'))['cases']}
        for name in model._REACHABILITY_PROGRAMS:
            with self.subTest(program=name):
                case=cases['complete_module_'+name]
                self.assertEqual(case['companions'],[{'path':path,'lf_sha256':model._INPUTS[path]} for path in model._COMPANIONS[name]])
                self.assertEqual(case['source'],model._PROGRAMS[name]);self.assertEqual(case['source_context'],'original_absolute')

if __name__=='__main__':unittest.main()
