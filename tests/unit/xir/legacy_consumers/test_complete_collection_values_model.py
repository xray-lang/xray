from provenance_test_support import provenance_input_path
import unittest
import complete_collection_values_model as model

class CollectionValues(unittest.TestCase):
    def test_all_complete_traces(self):
        expected={'array_member_scalar_target':[0,4,3,-1,True,False,4,0,4,2,9,9,6,9,7,4,4,1,8,8,4,1,8],
         'array_push_scalar_target':[5,4,-5,2,1.25,False,7],'array_literal_index_target':[1,3,99,4,99,103],
         'array_join_shared_core':['12|-3|0|99','false/true/false','1.5,-0.0,2.25','65-0-255','a:null:true:42:2.5','empty='],
         'array_sort_shared_core':[1,2,3,4,'a','aa','ab','b',2,2,5,9,False,False,True,True,True,True,True,True,'a','z','é','中'],
         'stringbuilder_append_shared_core':['bool=true; null=null; range=1..4; array=[1, 2, 3]'],
         'method_self_return_ownership':['b0 2','a0 2','a0-again 3','sb ','cleared ','t abcd','u abcd','v abcd','s abcd','a0 1','f 5 5','r0 3','b 104 105','rows 1','rows0-after-filter 3','s a,b,c 1','map-value 1','kept 3','rows0 1','y 6','y 4','y 2','y 1','y 4','y 4'],
         'stringbuilder_scalar_known_append_shared_core':['[true|false|中|A|null]'],
         'user_hashable_map_set_key':[2,'ONE','two',True,False,True,1,2,True,False]}
        self.assertEqual(sum(map(len,expected.values())),103)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(model.observations()[name],values)

    def test_array_owned_returns_are_independent_logical_values(self):
        for operation,argument in [('reverse',None),('sort',None),('fill',7)]:
            with self.subTest(operation=operation):
                array=model.Array([3,1,2]);old=model.Array(array.items);result=getattr(array,operation)(*(() if argument is None else (argument,)));result.items[0]=99
                self.assertNotEqual(array.items[0],99);self.assertEqual(old.items,[3,1,2])
        array=model.Array([[1],[2]]);snapshot=array.iterator();copy_=next(snapshot);copy_[0]=99;self.assertEqual(array.items,[[1],[2]])

    def test_fill_strict_bounds_and_empty_ranges(self):
        for start,end in [(-1,2),(2,1),(0,4)]:
            with self.subTest(start=start,end=end):
                array=model.Array([1,2,3])
                with self.assertRaisesRegex(ValueError,'E0422'):array.fill(7,start,end)
                self.assertEqual(array.items,[1,2,3])
        array=model.Array([1,2,3]);array.fill(7,3,3);self.assertEqual(array.items,[1,2,3])

    def test_owned_string_accumulation_and_scalar_rendering(self):
        self.assertNotIn('sb world',model.self_return_trace());self.assertIn('sb ',model.self_return_trace());self.assertIn('a0 1',model.self_return_trace())
        for value,text in [(-0.0,'-0.0'),(1.5,'1.5'),(False,'false'),(None,'null')]:
            with self.subTest(value=value):self.assertEqual(model.render(value),text)


    def test_four_whole_display_traces(self):
        expected={
         'container_print':['[1, 2, 3]','[1.5, 0.1, 100.0]','["a", "b", "c"]','[[1, 2], [3, 4]]','[true, false, true]','(1, "x", 2.5)','(42,)','[10, 20, 30]','[["x"], ["y", "z"]]','#{1: 10, 2: 20, 3: 30}','#[1, 2, 3]','#{"a": 1, "b": 2}', '#{1: [1, 2], 2: [3, 4]}','#{}','#["x", "y"]'],
         'slice_value_to_string':['[20, 30]','[20, 30]','["ab", "cd"]','[]','[10, 20, 30, 40]','v=[20, 30]','n=["ab", "cd"]',8,12,2],
         'function_value_to_string':['<fn>','<fn>','<fn>','<fn>',4,4,'f=<fn>',5,8,True],
         'json_forin_shared_core':['static keys=11','static pairs=11','dynamic keys=11','dynamic pairs=11','keys length=2','values length=2','entries length=2']}
        self.assertEqual(sum(map(len,expected.values())),42)
        self.assertEqual(model.display_observations(),expected)

    def test_recursive_display_and_insertion_order(self):
        mapping=model.DisplayMap([('z',[True,False]),('a',(1,))]);mapping.put('z',[3])
        self.assertEqual(model.display(mapping),'#{"z": [3], "a": (1,)}')
        self.assertEqual(mapping.keys(),['z','a']);self.assertEqual(mapping.values(),[[3],(1,)])
        self.assertEqual(model.display(model.DisplaySet(['z','a','z'])),'#["z", "a"]')
        self.assertEqual(model.display([['x'],['y','z']]),'[["x"], ["y", "z"]]')
        self.assertEqual(model.display('x'),'x');self.assertEqual(model.display(['x']),'["x"]')

    def test_tuple_arity_and_delimiters(self):
        for value,text in [((),'()'),((42,),'(42,)'),((1,'x',2.5),'(1, "x", 2.5)'),([],'[]'),(model.DisplayMap(),'#{}'),(model.DisplaySet([]),'#[]')]:
            with self.subTest(text=text):self.assertEqual(model.display(value),text)

    def test_f64_identity_in_container_display(self):
        self.assertEqual(model.display([100.0,100,-0.0,0.1]),'[100.0, 100, -0.0, 0.1]')
        self.assertEqual(model.display([True,1,False,0]),'[true, 1, false, 0]')

    def test_typed_borrowed_slice_range_and_owner(self):
        owner=model.Array([10,20,30,40]);view=model.BorrowedDisplaySlice(owner,1,3)
        self.assertIs(view.owner,owner);self.assertEqual(view.items(),[20,30]);self.assertEqual(len(view),2)
        self.assertEqual(model.display(view),'[20, 30]');self.assertEqual(model.display(model.BorrowedDisplaySlice(owner,2,2)),'[]')
        for lo,hi in [(-1,2),(2,1),(0,5),(True,2)]:
            with self.subTest(lo=lo,hi=hi),self.assertRaises(ValueError):model.BorrowedDisplaySlice(owner,lo,hi)
        with self.assertRaises(ValueError):model.BorrowedDisplaySlice([1,2],0,1)
        self.assertEqual(owner.items,[10,20,30,40])

    def test_callable_display_keeps_calls_and_hides_debug_names(self):
        named=model.DisplayCallable('private_helper',lambda n:n+1);other=model.DisplayCallable('different_lambda',lambda n:n*2)
        self.assertEqual(model.display(named),model.display(other));self.assertEqual(model.display(named),'<fn>')
        self.assertEqual((named.calls,other.calls),(0,0));self.assertNotIn('private_helper',model.display(named))
        self.assertEqual((named.apply(4),other.apply(4)),(5,8));self.assertEqual((named.calls,other.calls),(1,1))
        self.assertEqual(model.display(named),'<fn>');self.assertEqual(named.calls,1)

    def test_json_pair_association_and_member_presence(self):
        scalar=model.JsonDisplayScalar
        mapping=model.DisplayMap([('name',scalar('Alice')),('age',scalar(30))]);expected={'name':(1,'Alice'),'age':(10,'30')}
        self.assertEqual(model.json_iteration_scores(mapping,expected),(11,11))
        swapped=model.DisplayMap([('name',scalar(30)),('age',scalar('Alice'))]);self.assertEqual(model.json_iteration_scores(swapped,expected),(11,0))
        missing=model.DisplayMap([('name',scalar('Alice'))]);self.assertEqual(model.json_iteration_scores(missing,expected),(1,1))
        mapping.put('name',scalar('Bob'));self.assertEqual(mapping.keys(),['name','age']);self.assertEqual(model.json_iteration_scores(mapping,expected),(11,10))
        for value in [None,True,1.5,[],{}]:
            with self.subTest(value=value),self.assertRaises(ValueError):scalar(value)

    def test_complete_display_bindings_and_changed_input_rejection(self):
        from unittest.mock import patch
        rows=[row for row in model.expected_rows() if row['original_source'] in model._DISPLAY_PROGRAMS.values()]
        self.assertEqual(len(rows),4);self.assertEqual(sum(row['observation_count'] for row in rows),42)
        for row in rows:
            self.assertEqual(row['source'],row['original_source']);self.assertIsNone(row['required_target']);self.assertEqual(row['expected']['exit_code'],0)
        source=model._DISPLAY_PROGRAMS['slice_value_to_string']
        with patch.dict(model._INPUTS,{source:'0'*64}),self.assertRaisesRegex(ValueError,'source/authority changed'):model.expected_rows()


    def test_all_complete_explicit_bool_and_range_join_observations(self):
        facts=model.value_boundary_observations()
        truth=[('null',False),('false',False),('true',True),('zero-int',False),('nonzero-int',True),('zero-float',False),('nonzero-float',True),('empty-string',False),('nonempty-string',True),
            ('empty-array',False),('full-array',True),('empty-map',False),('full-map',True),('empty-boolmap',False),('full-boolmap',True),('empty-set',False),('full-set',True),('empty-typed-set',False),('full-typed-set',True)]
        self.assertEqual(facts['bool_conversion_truthy_shared_core'],[label+'='+str(value).lower() for label,value in truth])
        self.assertEqual(facts['array_join_special_object_shared_core'],['1..3,4..4,9..12','tag:2..5:null'])
        self.assertEqual(len(facts['value_format_truncation_shared_core']),3)
        self.assertEqual(sum(map(len,facts.values())),24)

    def test_explicit_bool_is_not_python_container_identity_or_implicit_condition(self):
        for value in (None,False,0,0.0,-0.0,'',model.Array([]),model.DisplayMap(),model.DisplaySet([])):self.assertFalse(model.explicit_bool(value))
        for value in (True,-1,0.25,'x',model.Array([False]),model.DisplayMap([(False,0)]),model.DisplaySet([0]),float('nan')):self.assertTrue(model.explicit_bool(value))
        for value in ([],{},lambda:7,object()):
            with self.subTest(value=type(value)),self.assertRaises(ValueError):model.explicit_bool(value)
        self.assertTrue(model.value_boundary_semantic_traces()['bool_conversion_truthy_shared_core']['implicit_condition_truthiness_not_granted'])

    def test_public_element_limit_covers_empty_exact_boundary_and_all_retained_tails(self):
        for count in (0,1,31,32,33,35):
            values,trace=model.complete_truncation_values(count);shown=min(count,32);suffix=(', ...('+str(count-32)+' more)') if count>32 else ''
            self.assertEqual(values[0],'['+', '.join(str(value) for value in range(shown))+suffix+']')
            self.assertEqual(values[1],'#{'+', '.join(str(value)+': '+str(value+100) for value in range(shown))+suffix+'}')
            self.assertEqual(values[2],'#['+', '.join(str(value) for value in range(shown))+suffix+']')
            self.assertEqual(trace['original_mutation_operations'],3*count);self.assertEqual(trace['complete_container_lengths'],[count]*3)
        array=model.Array(range(35));mapping=model.DisplayMap((value,value+100) for value in range(35));set_=model.DisplaySet(range(35))
        before=(list(array.items),mapping.entries(),list(set_.values))
        for value in (array,mapping,set_):self.assertIn(', ...(3 more)',model.limited_container_display(value))
        self.assertEqual(before,(array.items,mapping.entries(),set_.values));self.assertEqual(array.items[-1],34);self.assertEqual(mapping.entries()[-1],(34,134))
        self.assertFalse(model.complete_truncation_values()[1]['physical_storage_and_output_ownership_qualified'])

    def test_join_formats_range_values_including_empty_range_and_nullable_value(self):
        self.assertEqual(model.join_selected_values([model.RangeText(4,4)],','),'4..4')
        self.assertEqual(model.join_selected_values([model.RangeText(-2,5),None,False],'|'),'-2..5|null|false')
        self.assertEqual(model.join_selected_values([],','),'')
        with self.assertRaises(ValueError):model.RangeText(True,3)
        with self.assertRaises(ValueError):model.join_selected_values([model.RangeText(1,3)],None)
        self.assertFalse(model.value_boundary_semantic_traces()['array_join_special_object_shared_core']['real_join_ToString_capabilities_and_runtime_qualified'])

    def test_new_complete_value_sources_and_authorities_cannot_be_changed_or_dropped(self):
        from pathlib import Path
        from unittest import mock
        saved=Path.read_bytes
        for name in [*model._VALUE_BOUNDARY_PROGRAMS.values(),'src/shared/xr_truthy_core.h','src/shared/xr_range_core.h']:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==provenance_input_path(model.ROOT, name) else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
        rows=[row for row in model.expected_rows() if row['source'] in model._VALUE_BOUNDARY_PROGRAMS.values()]
        self.assertEqual(len(rows),3);self.assertEqual(sum(row['observation_count'] for row in rows),24)
        for row in rows:self.assertEqual(row['source'],row['original_source']);self.assertIn('required_semantic_trace',row)

    def test_complete_type_names_keep_all_ten_operands_and_exact_top_names(self):
        trace=model.type_names_trace()
        self.assertEqual(trace['observations'],['i64','f64','bool','null','string','Array','Map','Set','Range','string'])
        self.assertEqual((trace['original_typeName_calls'],trace['original_print_expressions']),(10,10))
        self.assertEqual(trace['all_original_operand_values'][5:9],[[1,2,3],{'a':1},[1,2,3],{'start':2,'end':6,'step':1,'inclusive':False}])
        self.assertFalse(trace['Range_Source_admission_and_result_string_ownership_FI_provider_qualified'])

    def test_type_name_builder_port_preserves_every_other_original_byte(self):
        original=(model.ROOT/model._TYPE_NAMES_ORIGINAL).read_text();port=(model.ROOT/model._TYPE_NAMES_PORT).read_text()
        self.assertEqual(model.port_text(model._TYPE_NAMES_NAME,original),port)
        self.assertEqual(port.replace('var sb = ""','var sb = StringBuilder()'),original)
        self.assertEqual(port.count('print(typeName('),10)

    def test_type_name_public_reference_rejects_changed_blob_and_unreviewed_operand(self):
        from pathlib import Path
        from unittest import mock
        import json
        ref=json.loads((model.ROOT/model._TYPE_NAMES_REFERENCE).read_text());name=ref['source_snapshots'][0]['snapshot'];saved=Path.read_bytes
        def changed(path):return saved(path)+b'\nchanged\n' if path==provenance_input_path(model.ROOT, name) else saved(path)
        with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.type_name_reference()
        for shape in (None,0,True,'StringBuilder',('Nullable','i64'),('Map','i64','i64')):
            with self.subTest(shape=shape),self.assertRaises(ValueError):model.reviewed_type_name(shape)
        self.assertEqual(model.reviewed_type_name(('Map','string','i64')),'Map')

    def test_type_name_oracle_follows_derived_names_not_product_output(self):
        from unittest import mock
        with mock.patch.object(model,'reviewed_type_name',lambda shape:'changed'):
            row=next(x for x in model.expected_rows() if x['original_source']==model._TYPE_NAMES_ORIGINAL)
            self.assertEqual(row['expected']['stdout_utf8'],'changed\n'*10)

    def test_complete_net_reference_keeps_all_three_outputs_without_calling_the_copier(self):
        trace=model.net_reference_trace()
        self.assertEqual(trace['observations'],[True,'NetError','NetError'])
        self.assertEqual(trace['original_imports'],['CopyBidirectionalResult','NetConn','NetError','copyBidirectional'])
        self.assertEqual(trace['original_helper_bodies'],['counts','accepts'])
        self.assertEqual((trace['counts_helper_invocations'],trace['accepts_helper_invocations'],trace['copier_argument_invocations']),(0,1,0))
        self.assertEqual(trace['actual_socket_or_copy_operations'],0)
        self.assertFalse(trace['network_copy_cancellation_ABI_Source_lifetime_FI_provider_qualified'])

    def test_net_callable_signature_is_proved_but_its_body_is_never_invoked(self):
        from unittest import mock
        with mock.patch.object(model.NetCallableReference,'invoke',side_effect=AssertionError('copier called')):
            self.assertIs(model.accept_net_copier(model.NetCallableReference(('NetConn','NetConn'),'CopyBidirectionalResult')),True)
            self.assertEqual(model.net_reference_trace()['observations'],[True,'NetError','NetError'])
        for parameters,result,symbol in [(('NetConn',),'CopyBidirectionalResult','copyBidirectional'),(('NetConn','NetConn'),'i64','copyBidirectional'),(('NetConn','NetConn'),'CopyBidirectionalResult','counts')]:
            with self.subTest(parameters=parameters,result=result,symbol=symbol),self.assertRaises(ValueError):
                model.accept_net_copier(model.NetCallableReference(parameters,result,symbol))

    def test_net_reference_retains_the_entire_original_program_and_exact_registered_source(self):
        import hashlib,json
        from pathlib import Path
        original=(model.ROOT/model._NET_REFERENCE_ORIGINAL).read_text(encoding='utf-8')
        self.assertEqual(hashlib.sha256(original.replace('\r\n','\n').encode()).hexdigest(),'cf115225bc75cd1aaaccbd2b9cd24f1fcc6a0592625caf4a48d86eb928fbc3cd')
        self.assertEqual(original.count('print('),3)
        self.assertEqual(original.count('copyBidirectional'),2)
        self.assertEqual(original.count('counts('),1);self.assertEqual(original.count('accepts('),2)
        row=next(x for x in model.expected_rows() if x['original_source']==model._NET_REFERENCE_ORIGINAL)
        self.assertEqual(row['source'],model._NET_REFERENCE_ORIGINAL);self.assertEqual(row['observation_count'],3)
        self.assertEqual(row['expected'],{'exit_code':0,'stdout_utf8':'true\nNetError\nNetError\n','stderr_utf8':''})
        case=next(x for x in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases'] if x['id']==row['id'])
        self.assertEqual(case['source'],row['source']);self.assertEqual(case['source_context'],'original_absolute')
        self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'');self.assertEqual(case['companions'],[])

    def test_net_reference_declarations_and_nominal_names_are_independent_of_product_output(self):
        from pathlib import Path
        from unittest import mock
        saved=Path.read_text
        for old,new in [('Timeout,','TimeoutChanged,'),('b: NetConn','b: i64')]:
            def changed(path,*args,**kwargs):
                value=saved(path,*args,**kwargs)
                return value.replace(old,new) if path==model.ROOT/model._NET_REFERENCE_DECLARATIONS else value
            with mock.patch.object(Path,'read_text',changed),self.assertRaises(ValueError):model.net_reference_declarations()
        with mock.patch.object(model,'accept_net_copier',lambda reference:False):
            row=next(x for x in model.expected_rows() if x['original_source']==model._NET_REFERENCE_ORIGINAL)
            self.assertEqual(row['expected']['stdout_utf8'],'false\nNetError\nNetError\n')

if __name__=='__main__':unittest.main()
