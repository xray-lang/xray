"""Verify full positive JSON boundaries without refreshing the frozen typed codec."""
from provenance_test_support import provenance_input_path
import unittest
import complete_json_boundary_model as model
import complete_typed_json_model as codec


class JSONBoundaryTraces(unittest.TestCase):
    def test_complete_original_traces(self):
        expected={'json_encode_boundary':['Map','Bob',20,True,'Map',1,'bee','seven',3,1,3],
            'json_kind_and_object_type':['object','Map','JSON.Value',True,False,'null','bool','int','float','string','array','object',True,True,True,True,True,True,True]}
        actual=model.boundary_observations();self.assertEqual(actual,expected);self.assertEqual(sum(map(len,actual.values())),30)
        rows={row['source']:row for row in model.expected_rows()}
        for name,path in model._PROGRAMS.items():self.assertEqual(rows[path]['expected']['stdout_utf8'],''.join((str(value).lower() if type(value) is bool else str(value))+'\n' for value in expected[name]))

    def test_explicit_encoding_preserves_container_identity_and_independent_values(self):
        inner=model.ExactObject([('flag',True)]);source=model.ExactObject([('items',[1,inner])])
        encoded=model.encode_boundary(source);self.assertEqual(model.runtime_family(source),'object')
        self.assertEqual(model.runtime_family(encoded),'Map');self.assertEqual(model.json_arm(encoded),'object')
        encoded['items'][0]=99;encoded['items'][1]['flag']=False
        self.assertEqual(model.encode_boundary(source),{'items':[1,{'flag':True}]})
        for invalid in ({7:'bad'},lambda:7,1<<63):
            with self.assertRaises(ValueError):model.encode_boundary(invalid)
        with self.assertRaises(ValueError):model.ExactObject([('x',1),('x',2)])
        with self.assertRaises(ValueError):model.json_arm(source)

    def test_json_paths_distinguish_full_keys_indices_boolean_indices_and_null(self):
        root={'7':{'tag':'seven'},'a.b':None,'a':{'b':[0,None,True]}}
        self.assertEqual(model.positive_path_value(root,['7','tag'],'string'),'seven')
        self.assertIsNone(model.positive_path_value(root,['a.b'],codec.nullable('i64')))
        self.assertEqual(model.positive_path_value(root,['a','b',0],'i64'),0)
        self.assertIsNone(model.positive_path_value(root,['a','b',1],codec.nullable('i64')))
        self.assertTrue(model.positive_path_value(root,['a','b',2],'bool'))
        for path,schema in [([7],'string'),(['a','b',True],'i64'),(['a','b',-1],'i64'),(['a','b',3],'i64'),(['a','missing'],'i64'),(['a','b',2],'i64')]:
            with self.subTest(path=path),self.assertRaises(ValueError):model.positive_path_value(root,path,schema)

    def test_kind_predicates_are_disjoint_and_Map_arm_differs_from_structural_type(self):
        values=[None,False,0,0.0,'',[],{}];want=['null','bool','int','float','string','array','object']
        self.assertEqual([model.json_arm(value) for value in values],want)
        matrix=[[model.json_arm(value)==name for name in want] for value in values]
        self.assertEqual(matrix,[[row==column for column in range(7)] for row in range(7)])
        self.assertEqual(model.runtime_family({}),'Map');self.assertEqual(model.runtime_family(model.ExactObject([])),'object')
        self.assertEqual(model.boundary_semantic_traces()['json_kind_and_object_type']['typeOf_object_comparisons'],[True,False])
        self.assertFalse(model.boundary_semantic_traces()['json_kind_and_object_type']['public_full_type_queries_runtime_tags_and_provider_qualified'])

    def test_Set_boundary_keeps_first_insertion_order_without_sorting(self):
        values=model.InsertionSet([3,1,3,2,1]);self.assertEqual(model.encode_boundary(values),[3,1,2])
        self.assertEqual(model.encode_boundary(model.InsertionSet([])),[])
        self.assertEqual(model.encode_boundary(model.InsertionSet([0,False,0])),[0,False])
        with self.assertRaises(ValueError):model.InsertionSet([[]])

    def test_all_reviewed_inputs_and_new_complete_paths_are_exact_and_fail_closed(self):
        from pathlib import Path
        from unittest import mock
        saved=Path.read_bytes
        for name in model._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==provenance_input_path(model.ROOT, name) else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
        for name in ('json_parse_typed_error_path','json_parse_value_invalid_panic','json_typed_root_values','json_with_rest_and_path'):self.assertNotIn(name,model._NAMES)
        with mock.patch.object(model,'_PROGRAMS',{}),self.assertRaises(ValueError):model.expected_rows()
        rows=[row for row in model.expected_rows() if row['source'] in model._BOUNDARY_PROGRAMS.values()]
        self.assertEqual(len(rows),2);self.assertEqual(sum(row['observation_count'] for row in rows),30)
        self.assertTrue(all(row['source']==row['original_source'] and 'required_semantic_trace' in row for row in rows))

    def test_existing_collection_factory_binds_new_inputs_in_both_real_drivers(self):
        import ast,hashlib
        import complete_collection_values_model as collections
        directory=model.ROOT/'tests/unit/xir/legacy_consumers'
        for name in ('run_product_driver.py','run_full_diff_driver.py'):
            source=(directory/name).read_text(encoding='utf-8');tree=ast.parse(source)
            self.assertTrue(any(isinstance(node,ast.Import) and any(alias.name=='complete_collection_values_model' for alias in node.names) for node in ast.walk(tree)))
            self.assertTrue(any(isinstance(node,ast.Call) and isinstance(node.func,ast.Attribute) and isinstance(node.func.value,ast.Name) and node.func.value.id=='complete_collection_values_model' and node.func.attr=='expected_rows' for node in ast.walk(tree)))
            self.assertIn('complete_collection_values_model.py',[node.value for node in ast.walk(tree) if isinstance(node,ast.Constant) and isinstance(node.value,str)])
        source=provenance_input_path(collections.ROOT, collections._JSON_BOUNDARY_INPUT);self.assertEqual(collections._INPUTS[collections._JSON_BOUNDARY_INPUT],hashlib.sha256(source.read_bytes().replace(b'\r\n',b'\n')).hexdigest())
        rows=collections.expected_rows();identities=[row['id'] for row in rows]
        for row in model.expected_rows():self.assertEqual(identities.count(row['id']),1);self.assertIn(row,rows)
        self.assertIn('legacy_consumer_complete_json_boundary_model_controls',(directory/'CMakeLists.txt').read_text(encoding='utf-8'))

    def test_real_rejection_context_still_validates_every_frozen_input(self):
        import task_rejection_model as rejection
        required=rejection.expected();self.assertEqual(required['id'],rejection.IDENTITY)
        self.assertEqual(len(required['controls']),2)
        self.assertTrue(all(row['expected']['stdout_utf8']=='42\n' for row in required['controls']))

if __name__=='__main__':unittest.main()
