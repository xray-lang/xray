"""Pin complete imported observations and initialization failure boundaries."""
import unittest
import complete_module_model as model


class ModuleTraces(unittest.TestCase):
    def test_complete_module_observations(self):
        expected={name:[] for name in ['_classlib','_default_arg_class_lib','_exported_struct_methods_lib','_inherit_lib','_ptr_offset_const_lib','_value_struct_arg_lib']}
        expected.update({'_cleanup_init_error':['error dependency body','error dependency cleanup'],
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
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),67)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_required_termination_is_execution_not_compilation_refusal(self):
        for name in ['_cleanup_init_error','cleanup_init_error','_cleanup_init_panic','cleanup_init_panic','xmod_class']:
            with self.subTest(program=name):
                code,stderr=model.termination(name)
                if name.endswith('_error'):self.assertEqual((code,stderr),(1,'[Uncaught Error] InitError.Failed\n'))
                elif name.endswith('_panic'):self.assertEqual((code,stderr),(1,'[Uncaught Panic] E0420: division by zero\n'))
                else:self.assertEqual((code,stderr),(0,''))


if __name__=='__main__':unittest.main()
