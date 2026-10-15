import unittest
import complete_aggregate_model as model

class AggregateTraces(unittest.TestCase):
    def test_complete_traces(self):
        expected={'span_active_borrow_branch_nll_shared_core':[1,2,2,2,2],
         'span_active_borrow_disjoint_owner_shared_core':[1,1,1,10,1,3,1,8],
         'span_active_borrow_nested_owner_shared_core':[1,7,2,1,3,2],
         'object_rename':['Alice',30,'Alice','Shanghai'],
         'tuple_fixed_width_unsigned':[4689139246482086,1,10000000000000000,100000000000000000,True,7],
         'object_field_invariance':['5/<none>','-1/abc','-1/<none>','Alice/-1','Bob/3'],
         'object_optional_sparse_fields':['late',True,True,25,True,7,True],
         'union_as_conversion':['a=7 b=q','c=3'],
         'union_member_selection':['a=1 is i64=true is f64=false','b=1.0 is i64=false is f64=true','c=7 is i32=true is string=false','d=1.5 is f32=true is string=false','e=s is i32=false is string=true','widened=3 is i32=true is string=false','switched=2.5 is i64=false is f64=true','m1 i64','m2 f64'],
         'map_build_in_fn':[6,0,9,25,55,3,4],
         'map_float_key_identity':['nan set size=1','nan set contains=true','nan map size=1','nan map containsKey=true','nan map get=second','nan map size after delete=0','zero map size=1','zero get=neg','neg zero get=neg','0.0 == -0.0 -> true','nan == nan -> false'],
         'map_key_contextual_literal':['size after int literal=1','size after float literal=1','get via int literal=float-literal','get via float literal=float-literal','key=1.0','wide size=1','wide get=one','set size=1','set contains int literal=true'],
         'enum_payload_equality':[True,False,True,False,False,2,'one','two',2]}
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),88)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_borrow_path_and_last_use(self):
        ledger=model.BorrowLedger();view=ledger.begin(('box','left'))
        for path in [('box',),('box','left'),('box','left',0)]:
            with self.subTest(path=path):
                with self.assertRaises(ValueError):ledger.mutate(path)
        ledger.mutate(('box','right'));ledger.end(view);ledger.mutate(('box','left'))

    def test_structural_fields_union_and_float_keys(self):
        with self.assertRaises(ValueError):model.materialize({'required':False},{})
        with self.assertRaises(ValueError):model.materialize({'known':True},{'extra':1})
        self.assertEqual(model.materialize({'known':True},{}),{'known':None})
        self.assertEqual(model.select_union(['f64','i64'],'i64',3),('i64',3))
        self.assertEqual(model.select_union(['string','i32'],'i16',3),('i32',3))
        self.assertEqual(model.float_key(float('nan')),model.float_key(float('nan')))
        self.assertEqual(model.float_key(-0.0),model.float_key(0.0))

if __name__=='__main__':unittest.main()
