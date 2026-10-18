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
        expected.update({'enum_type_name':['Color','Color','Shape'],
          'enum_value_aot_identity':['Color.Red|Color.Green|Color.Blue','Red',1,2,'Color.Red',True,'Green:1','N','Direction.North',404],
          'adt_enum_payload_tostring':['x=E.A(7)','E.A(7)','i=E.A(7)','c=E.A(7)','E.B','b=E.B',
             'P.Pair(3, "hi")','p=P.Pair(3, "hi")','W.Wrap(E.A(9))','w=W.Wrap(E.A(9))',
             'g=E.A(7)','g=P.Pair(3, "hi")','L=P.Pair(42, "'+'a'*80+'")','M=P.Pair(42, "'+'a'*80+'")']})
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),115)
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

    def test_enum_value_names_do_not_become_variant_names_or_constructor_values(self):
        decl=model.EnumType('m','Shape',(('Circle',(('radius','i64'),)),('Point',())))
        self.assertEqual('Shape',model.enum_type_name(decl.construct('Point')))
        self.assertEqual('Shape',model.enum_type_name(decl.construct('Circle',radius=7)))
        for value in (decl,decl.construct):
            with self.assertRaises(ValueError):model.enum_type_name(value)

    def test_enum_name_text_never_grants_nominal_identity(self):
        a=model.EnumType('a','E',(('A',()),))
        b=model.EnumType('b','E',(('A',()),))
        local=model.EnumType('a','E',(('A',()),),1)
        self.assertEqual(model.enum_type_name(a.construct('A')),model.enum_type_name(b.construct('A')))
        self.assertNotEqual(a.construct('A'),b.construct('A'))
        self.assertNotEqual(a.construct('A'),local.construct('A'))
        self.assertEqual(a.construct('A'),a.construct('A'))

    def test_payload_declared_order_and_exact_types_survive_construction(self):
        p=model.EnumType('m','P',(('Pair',(('first','i64'),('second','string'))),('Solo',())))
        self.assertEqual('P.Pair(3, "hi")',model.enum_text(p.construct('Pair',second='hi',first=3)))
        for kwargs in ({'first':3},{'first':3,'second':'hi','extra':1},{'first':True,'second':'hi'},
                       {'first':3,'second':1},{'first':1<<63,'second':'hi'}):
            with self.assertRaises(ValueError):p.construct('Pair',**kwargs)
        self.assertEqual('P.Solo',model.enum_text(p.construct('Solo')))

    def test_nested_enum_keeps_the_exact_payload_declaration(self):
        e=model.EnumType('m','E',(('A',(('value','i64'),)),))
        foreign=model.EnumType('other','E',(('A',(('value','i64'),)),))
        w=model.EnumType('m','W',(('Wrap',(('value',e),)),))
        self.assertEqual('W.Wrap(E.A(9))',model.enum_text(w.construct('Wrap',value=e.construct('A',value=9))))
        with self.assertRaises(ValueError):w.construct('Wrap',value=foreign.construct('A',value=9))

    def test_nested_strings_escape_quotes_and_slashes_without_becoming_top_level_strings(self):
        p=model.EnumType('m','P',(('Pair',(('first','i64'),('second','string'))),))
        value=p.construct('Pair',first=2,second='a'+chr(34)+chr(92)+'b')
        expected_bytes=[80,46,80,97,105,114,40,50,44,32,34,97,92,34,92,92,98,34,41]
        self.assertEqual(expected_bytes,list(model.enum_text(value).encode('utf-8')))
        self.assertEqual('hi',model.enum_text('hi'))
        self.assertEqual('"hi"',model.enum_text('hi',True))

    def test_complete_payload_surfaces_keep_original_generic_and_long_payload_work(self):
        trace=model.enum_surface_traces()['adt_enum_payload_tostring']
        self.assertEqual((14,7,1,6,2,80),(len(trace['outputs']),trace['explicit_toString_runtime_calls'],
                         trace['string_conversion_calls'],trace['interpolation_calls'],trace['generic_describe_calls'],trace['long_payload_UTF8_bytes']))
        self.assertFalse(trace['generic_definition_display_permission_proved'])
        self.assertTrue(trace['full_original_generic_body_retained'])
        self.assertIn('a'*80,trace['outputs'][-1]);self.assertEqual(trace['outputs'][-2][2:],trace['outputs'][-1][2:])
        self.assertEqual(['g=E.A(7)','g=P.Pair(3, "hi")'],trace['outputs'][10:12])

    def test_malformed_nominal_variant_or_payload_is_rejected(self):
        decl=model.EnumType('m','E',(('A',(('value','i64'),)),('B',())))
        for name,ordinal,payload in [('B',0,()),('A',1,(1,)),('A',0,()),('A',0,(True,))]:
            with self.assertRaises(ValueError):model.EnumValue(decl,name,ordinal,payload)
        with self.assertRaises(ValueError):model.EnumType('m','E',(('A',()),('A',())))

    def test_all_three_whole_original_enum_programs_registered(self):
        import json
        cases={row['id']:row for row in json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_text())['cases']}
        rows=[row for row in model.expected_rows() if row['original_source'] in model._ENUM_PROGRAMS.values()]
        self.assertEqual(3,len(rows));self.assertEqual(27,sum(row['observation_count'] for row in rows))
        for row in rows:
            self.assertEqual(row['source'],cases[row['id']]['source'])
            self.assertEqual(row['expected']['stdout_utf8'],cases[row['id']]['stdout_utf8'])

if __name__=='__main__':unittest.main()
