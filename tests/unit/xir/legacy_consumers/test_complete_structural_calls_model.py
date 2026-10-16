"""Complete structural calls and declaration-only execution boundaries."""
from pathlib import Path
import hashlib,itertools,json,unittest
from unittest import mock
import complete_structural_calls_model as m


class CompleteStructuralCalls(unittest.TestCase):
    def test_five_complete_programs_and_all15_original_observations(self):
        expected={'generic_width_direct_ordinal':['adabobcatdot'],
            'object_shape_test_and_narrow':['ok decodes User=true','missing decodes User=false','extra rejects by default=true',
                'extra ignores explicitly=true','wrongKind decodes User=false','narrowed=Alice/30','scalar decodes User=false','tagged:9','user:Eve'],
            'hashable_contract':[True,99,99,True,7],'1471_default_read_rest_param':[],'1472_default_read_destructure_param':[]}
        self.assertEqual({k:v['observations'] for k,v in m.observations().items() if k!=m._DECLARED_NAME},expected)
        rows=[row for row in m.expected_rows() if Path(row['source']).stem!=m._DECLARED_NAME];self.assertEqual([x['observation_count'] for x in rows],[1,9,5,0,0])
        self.assertEqual(sum(x['observation_count'] for x in rows),15)
        for row in rows:
            name=Path(row['source']).stem
            self.assertEqual(row['expected']['stdout_utf8'],''.join(m.render({'expected':x})+'\n' for x in expected[name]))

    def test_width_calls_keep_every_concrete_field_and_original_projection(self):
        trace=m.observations()['generic_width_direct_ordinal']
        self.assertEqual(trace['all_original_field_values'],[{'name':'ada','age':1},{'name':'bob','zzz':2},{'age':3,'name':'cat'},{'id':4,'name':'dot'}])
        self.assertEqual(trace['concrete_fields_in_written_order'],[['name','age'],['name','zzz'],['age','name'],['id','name']])
        self.assertEqual(trace['original_projection_calls'],4);self.assertTrue(trace['no_physical_ordinal_assumed'])

    def test_logical_name_projection_is_independent_of_written_field_order(self):
        for fields in itertools.permutations([('id',4),('name','dot'),('extra',True)]):
            self.assertEqual(m.project(dict(fields),'name',{'name':'string'}),'dot')
        with mock.patch.object(m,'project',lambda value,name,required:'wrong'):
            row=m.expected_rows()[0];self.assertNotEqual(row['expected']['stdout_utf8'],'adabobcatdot\n')

    def test_definition_site_constraint_exposes_only_declared_fields(self):
        value={'name':'ada','age':1}
        with self.assertRaisesRegex(ValueError,'unconstrained'):m.project(value,'age',{'name':'string'})
        for value in [{},{'name':1},{'name':True},{'name':None}]:
            with self.assertRaises(ValueError):m.project(value,'name',{'name':'string'})

    def test_exact_shape_and_width_constraint_are_distinct_relations(self):
        value={'name':'ada','age':1}
        self.assertFalse(m.satisfies(value,{'name':'string'}));self.assertTrue(m.satisfies(value,{'name':'string'},exact=False))
        self.assertTrue(m.satisfies(value,{'name':'string','age':'i64'}))
        self.assertFalse(m.satisfies({'name':'ada','age':True},{'name':'string','age':'i64'}))

    def test_json_boundary_requires_explicit_unknown_field_permission(self):
        schema={'name':'string','age':'i64'};extra={'name':'Carol','age':7,'extra':True}
        self.assertIs(m.codec.decode(extra,schema),m.codec.INVALID)
        self.assertEqual(m.codec.decode(extra,schema,True),{'name':'Carol','age':7})
        for value in [{'name':'Bob'},{'name':'Dan','age':'not an int'},{'name':'Dan','age':True},42,None]:
            self.assertIs(m.codec.decode(value,schema),m.codec.INVALID)

    def test_static_union_narrowing_uses_exact_fields_and_types(self):
        self.assertEqual(m.describe({'kind':'t','n':9}),'tagged:9');self.assertEqual(m.describe({'name':'Eve','age':1}),'user:Eve')
        for value in [42,{'kind':'t','n':True},{'kind':'t','n':1<<63},{'kind':'t','n':9,'extra':1},{'name':'Eve'},{'name':'Eve','age':'1'}]:
            self.assertEqual(m.describe(value),'neither')

    def test_hashable_key_observations_follow_original_hash_and_equality(self):
        a=m.Token('a',7);b=m.Token('b',7);c=m.Token('c',8);table=m.KeyTable();table.set(a,99)
        self.assertEqual(a.hash_value(),7);self.assertTrue(a.equal(b));self.assertFalse(a.equal(c))
        self.assertTrue(table.contains(b));self.assertEqual(table.get(b),99);self.assertFalse(table.contains(c));self.assertIsNone(table.get(c))

    def test_hash_collision_still_checks_declared_value_equality(self):
        table=m.KeyTable(lambda value:0);a=m.Token('a',7);b=m.Token('b',8)
        table.set(a,99);table.set(b,100);self.assertEqual(table.get(a),99);self.assertEqual(table.get(b),100)
        table.set(m.Token('new-owner',7),42);self.assertEqual(table.get(a),42);self.assertEqual(len(table.buckets[0]),2)

    def test_explicit_class_copy_keeps_identity_without_moving_escaped_root(self):
        original=m.Token('root',7);temporary=original.owned_copy()
        self.assertIsNot(temporary,original);self.assertEqual(temporary.identity,original.identity);self.assertEqual(temporary.value,7)
        trace=m.observations()['hashable_contract'];self.assertFalse(trace['original_escaped_root_moved'])
        self.assertTrue(trace['temporary_owner_moved_into_generic']);self.assertTrue(trace['copy_stops_at_class_identity'])
        self.assertEqual(trace['Map_and_Set_key_identity'],trace['explicit_copy_temporary_identity'])

    def test_unused_read_signatures_never_claim_argument_execution(self):
        traces=m.observations();rest=traces['1471_default_read_rest_param']['declaration'];destructure=traces['1472_default_read_destructure_param']['declaration']
        for value in [rest,destructure]:self.assertEqual(value['parameter_mode'],'READ');self.assertEqual(value['invocations'],0);self.assertTrue(value['body_empty'])
        self.assertTrue(rest['variadic']);self.assertEqual(rest['element_type'],'i64');self.assertEqual(rest['parameter_pattern'],'xs')
        self.assertFalse(destructure['variadic']);self.assertEqual(destructure['declared_type'],'[i64;1]');self.assertEqual(destructure['parameter_pattern'],'[x]')
        for row in m.expected_rows()[-2:]:self.assertEqual(row['expected'],{'exit_code':0,'stdout_utf8':'','stderr_utf8':''})

    def test_preserved_original_golden_cannot_be_refreshed_to_hide_wrong_output(self):
        saved=Path.read_bytes;golden=m.ROOT/m.GOLDEN;changed=saved(golden).replace(b'Alice',b'Mary')
        pins=dict(m._INPUTS);pins[m.GOLDEN]=hashlib.sha256(changed.replace(b'\r\n',b'\n')).hexdigest()
        def read(path):return changed if path==golden else saved(path)
        with mock.patch.object(Path,'read_bytes',read),mock.patch.object(m,'_INPUTS',pins),self.assertRaisesRegex(ValueError,'preserved original golden'):
            m.expected_rows()

    def test_every_complete_source_and_normative_input_is_pinned(self):
        saved=Path.read_bytes
        for name in m._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_PROGRAMS',dict(reversed(list(m._PROGRAMS.items())))),self.assertRaises(ValueError):m.expected_rows()

    def test_product_registrations_keep_all_original_sources_and_arguments(self):
        cases={x['id']:x for x in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']}
        for row in m.expected_rows():
            case=cases[row['id']];self.assertEqual(case['source'],row['source']);self.assertEqual(case['source'],case['preserved_original_identity']['source'])
            self.assertEqual(case['entry'],'run');self.assertEqual(case['source_context'],'original_absolute');self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8'])


    def test_declared_selector_methods_keep_all_three_original_calls(self):
        trace=m.declared_selector_trace()
        self.assertEqual(trace['observations'],[10,False,True])
        self.assertEqual(trace['nominal_receiver'],'Box');self.assertEqual(trace['initial_total'],4)
        self.assertEqual(trace['helper_calls'],['spread(Box)','holds(Box)'])
        self.assertEqual(trace['declared_method_calls'],[
            {'caller':'spread','method':'Box.fill','arguments':[1,2,3]},
            {'caller':'holds','method':'Box.contains','arguments':[7]},
            {'caller':'main','method':'Box.contains','arguments':[4]}])
        self.assertEqual(trace['total_after_calls'],4);self.assertEqual(trace['receiver_writes'],0)
        self.assertEqual(trace['container_operations'],0);self.assertEqual(trace['original_main_calls'],1)

    def test_declared_receiver_arithmetic_and_equality_do_not_use_container_semantics(self):
        receiver=m.DeclaredBoxRead(4)
        self.assertEqual(receiver.fill(1,2,3),10)
        self.assertFalse(receiver.contains(7));self.assertTrue(receiver.contains(4))
        self.assertEqual(m.DeclaredBoxRead(8).fill(1,2,3),14)
        self.assertTrue(m.DeclaredBoxRead(7).contains(7));self.assertEqual(receiver.total,4)
        for bad in (True,False,None,4.0,'4',1<<63,-(1<<63)-1):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):m.DeclaredBoxRead(bad)
                with self.assertRaises(ValueError):receiver.contains(bad)
                with self.assertRaises(ValueError):receiver.fill(bad,2,3)
        with self.assertRaises(ValueError):m.DeclaredBoxRead((1<<63)-1).fill(1,0,0)

    def test_declared_selector_whole_original_remains_unchanged(self):
        source=m._PROGRAMS[m._DECLARED_NAME];text=(m.ROOT/source).read_text(encoding='utf-8')
        self.assertEqual(hashlib.sha256(text.replace('\r\n','\n').encode()).hexdigest(),'883b0955fe6a6e138a11e2219bdebec065a5640ab49cdb821d2f39d403d94d08')
        self.assertEqual(text.count('print('),3)
        for body in ('return this.total + value + start + end','return this.total == value','return box.fill(1, 2, 3)','return box.contains(7)','print(b.contains(4))','main()'):
            self.assertIn(body,text)
        row=next(x for x in m.expected_rows() if Path(x['source']).stem==m._DECLARED_NAME)
        self.assertEqual(row['source'],source);self.assertEqual(row['observation_count'],3)
        self.assertEqual(row['expected'],{'exit_code':0,'stdout_utf8':'10\nfalse\ntrue\n','stderr_utf8':''})

    def test_declared_selector_registered_output_changes_with_the_independent_method_trace(self):
        with mock.patch.object(m.DeclaredBoxRead,'fill',lambda self,*args:11):
            row=next(x for x in m.expected_rows() if Path(x['source']).stem==m._DECLARED_NAME)
            self.assertEqual(row['expected']['stdout_utf8'],'11\nfalse\ntrue\n')
        trace=m.declared_selector_trace()
        self.assertEqual(trace['internal_intrinsic_or_ABI_identity'],'NOT_INFERRED_FROM_THREE_OUTPUTS')


if __name__=='__main__':unittest.main()
