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
        self.assertEqual({k:v['observations'] for k,v in m.observations().items()},expected)
        rows=m.expected_rows();self.assertEqual([x['observation_count'] for x in rows],[1,9,5,0,0])
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


if __name__=='__main__':unittest.main()
