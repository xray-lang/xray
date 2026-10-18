"""Whole generic programs, exact type identities and protocol boundaries."""
import itertools,unittest
from unittest.mock import patch
import complete_generic_program_model as m


class CompleteGenericPrograms(unittest.TestCase):
    def test_all32_original_observations(self):
        values=m.traces()
        self.assertEqual(values[m._NAMES[0]]['outputs'],[9,12,9,11,6,4,2,2,5,5,1,'ab',30,21,29,19,3,3])
        self.assertEqual(values[m._NAMES[1]]['outputs'],[6,6,10,14,10,5,3,31,3,3,2,2,3,4])
        self.assertEqual(sum(len(x['outputs']) for x in values.values()),32)

    def test_all_original_display_texts_and_nested_quotes(self):
        calls=m.container_trace()['calls']
        self.assertEqual([x['facts']['complete_text'] for x in calls[:8]],
            ['[1, 2, 3]','["ab", "cd"]','#{"k": 1}','#["a", "b"]','{a: 1}','1..5','42','xy'])
        self.assertEqual(calls[14]['facts']['complete_text'],'(1, "z", true)')

    def test_every_call_and_exact_partition_is_retained(self):
        a,b=m.container_trace(),m.iterable_trace()
        self.assertEqual((len(a['calls']),len(b['calls'])),(18,14))
        self.assertEqual(len({x['instance'] for x in a['calls']}),18)
        self.assertEqual(len({x['instance'] for x in b['calls']}),12)
        with self.assertRaisesRegex(ValueError,'concrete type arguments'):
            m.instance_key('module','f',(m.array(('Parameter','T')),))

    def test_container_element_types_cannot_share_semantic_instance(self):
        key=lambda t:m.instance_key('module','f',(t,))
        self.assertNotEqual(key(m.array(m.I64)),key(m.array(m.STRING)))
        self.assertNotEqual(key(m.array(m.I64)),key(m.slice_type(m.I64)))
        self.assertNotEqual(key(m.mapping(m.STRING,m.I64)),key(m.mapping(m.I64,m.STRING)))

    def test_tuple_arity_and_element_types_are_exact(self):
        key=lambda t:m.instance_key('module','f',(t,))
        self.assertNotEqual(key(('Tuple',m.I64,m.I64)),key(('Tuple',m.I64,m.STRING,m.BOOL)))

    def test_definition_module_is_part_of_instance_identity(self):
        self.assertNotEqual(m.instance_key('one','same',(m.I64,)),m.instance_key('two','same',(m.I64,)))
        self.assertNotEqual(m.instance_key('one','first',(m.I64,)),m.instance_key('one','second',(m.I64,)))

    def test_full_function_signature_keeps_modes_effects_and_suspend(self):
        key=lambda t:m.instance_key('module','f',(t,))
        basic=m.function_type((m.I64,),m.I64)
        variants=[m.function_type((m.STRING,),m.I64),m.function_type((m.I64,),m.STRING),
            m.function_type((m.I64,),m.I64,modes=('MOVE',)),m.function_type((m.I64,),m.I64,throws=('Error.Invalid',)),
            m.function_type((m.I64,),m.I64,suspends=True)]
        self.assertEqual(len({key(basic),*[key(x) for x in variants]}),6)

    def test_function_argument_does_not_invoke_helper_or_allow_erased_fallback(self):
        trace=m.container_trace()
        self.assertEqual(trace['helper_invocations'],0)
        self.assertFalse(trace['current_erased_origin_fallback_allowed'])
        self.assertNotEqual(trace['calls'][-2]['instance'],trace['calls'][-1]['instance'])

    def test_first_or_element_binding_and_preserved_empty_branch(self):
        calls=m.container_trace()['calls']
        self.assertEqual([x['type_argument'] for x in calls[10:12]],[m.I64,m.STRING])
        self.assertEqual(m.first_or([],7),7)
        self.assertEqual(m.first_or([1,2],7),1)

    def test_all_original_arithmetic_loop_steps_and_guard_are_retained(self):
        self.assertEqual(m.container_trace()['original_arithmetic_loop_iterations'],16)
        self.assertEqual(m.guarded_sum([100000])[0],100000)
        self.assertEqual(m.guarded_sum([100001])[0],0)
        self.assertTrue(all(x['facts'].get('guard',100000)==100000 for x in m.container_trace()['calls']))

    def test_current_closed_display_does_not_accept_open_parameter_or_extra_domain(self):
        self.assertTrue(m.current_closed_display(m.array(m.STRING)))
        for t in [('Parameter','T'),m.mapping(m.STRING,m.I64),m.set_type(m.STRING),('Object',(('a',m.I64),)),
                  m.function_type((m.I64,),m.I64),m.RANGE,('Tuple',m.I64,m.I64)]:
            self.assertFalse(m.current_closed_display(t))
        self.assertFalse(m.container_trace()['open_parameter_display_admitted'])

    def test_map_iterates_keys_with_exact_element_type(self):
        self.assertEqual(m.iterable_element(m.mapping(m.STRING,m.I64)),m.STRING)
        self.assertNotEqual(m.iterable_element(m.mapping(m.STRING,m.I64)),m.I64)
        for order in itertools.permutations(['aa','bbb']):
            self.assertEqual(m.reduce_items(order,keys=True)[0],5)

    def test_JSON_Object_alias_does_not_erase_map_value_type(self):
        trace=m.iterable_trace()
        self.assertEqual(trace['JSON_Object_canonical_type'],m.mapping(m.STRING,m.JSON_VALUE))
        a,b=trace['calls'][5:7]
        self.assertNotEqual(a['instance'],b['instance'])
        self.assertEqual([a['result'],b['result']],[5,3])

    def test_indexed_loop_keeps_both_original_indices_and_values(self):
        row=m.iterable_trace()['calls'][7]
        self.assertEqual(row['facts']['loop_steps'],[(0,10,0,10),(1,20,10,31)])

    def test_generator_is_single_pass_and_has_one_terminal_next(self):
        values,conditions=m.up_to(4);cursor=m.Cursor(values)
        self.assertEqual(list(cursor),[1,2,3,4]);self.assertEqual(cursor.next_calls,5)
        self.assertEqual(list(cursor),[]);self.assertEqual(cursor.next_calls,6)
        self.assertEqual(conditions,[True,True,True,True,False])

    def test_complete_iterable_work_and_generator_body_are_retained(self):
        trace=m.iterable_trace()
        self.assertEqual(trace['original_for_body_iterations'],24)
        self.assertEqual(trace['generator_arithmetic_iterations'],4)
        self.assertEqual(trace['generator_consumed_values'],4)
        self.assertEqual(trace['generator_next_calls'],5)

    def test_iterator_JSON_Value_and_Channel_are_not_lengthable(self):
        self.assertEqual(m.iterable_element(m.iterator(m.I64)),m.I64)
        for t in [m.iterator(m.I64),m.JSON_VALUE,('Channel',m.I64)]:
            self.assertFalse(m.lengthable(t))
        self.assertIsNone(m.iterable_element(m.JSON_VALUE))
        self.assertIsNone(m.iterable_element(('Channel',m.I64)))

    def test_byte_length_does_not_count_Unicode_scalars(self):
        self.assertEqual(m.byte_length('é😀'),6)

    def test_complete_source_or_authority_tamper_is_rejected(self):
        pins=dict(m._INPUTS);pins[m._PROGRAMS[m._NAMES[0]]]='0'*64
        with patch.object(m,'_INPUTS',pins):
            with self.assertRaisesRegex(ValueError,'original or authority'):m.expected_rows()

    def test_registered_rows_have_all18_and14_observations(self):
        rows=m.expected_rows()
        self.assertEqual([x['observation_count'] for x in rows],[18,14])
        self.assertTrue(all(x['expected']['exit_code']==0 and not x['expected']['stderr_utf8'] for x in rows))
        self.assertTrue(all('OPEN' in x['model'] for x in rows))


if __name__=='__main__':unittest.main()
