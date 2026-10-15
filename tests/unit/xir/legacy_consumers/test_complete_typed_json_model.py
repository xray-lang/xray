"""Pin independent typed JSON distinctions and complete program traces."""
import unittest
import complete_typed_json_model as model


class TypedJSONTraces(unittest.TestCase):
    def test_complete_original_traces(self):
        expected={
            'json_decode_nullable_nested_object':[False,False,'Hangzhou',310000,False,True,False,True,True],
            'json_decode_object_loop_reuse':[21],
            'json_decode_type_matrix':[False,1,2.5,'hi',True,True,True,True,True,True,True,True,True,False,1,True,True,False,4,True,False,False,False,False,False,False,False,False,3,2,2,2,True,True,True,True],
            'json_decode_typed_fields':[False,'Ada',37,True,9.5,True,True,True,True,False,True,False,4,True],
            'json_derive_decode':['Ada',37,False,'Grace',42,'Lin','vm',9,True,3,4,8,'nested',8,'nested','{"point":{"x":8,"y":9},"label":"nested"}',False,12,'decoded',True,'Ready',True,'Ready','{"state":"Ready","note":null}'],
            'json_derive_struct_encode':['{"i":42,"f":1.5,"b":true,"s":"hi"}','{"a":7,"inner":{"v":9}}','[{"v":1},{"v":2}]'],
            'json_namespace_value_scalar_target':[1,2.5,True,-7],
            'json_parse_typed_direct':['Ada',37,'Hangzhou',30.25,True,3,3,1,1,2],
            'object_json_codec_bridge':['Ada',37,'Hangzhou',310000,False,'Ada',37,'Hangzhou',310000,'{"name":"Ada","age":37,"address":{"city":"Hangzhou","zip":310000}}']}
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),111)
        for name,values in expected.items():
            with self.subTest(program=name):
                self.assertEqual([model.render(v) for v in actual[name]],[model.render(v) for v in values])

    def test_bool_integer_float_and_range_are_distinct(self):
        for value,schema in [(True,'i64'),(False,'i64'),(1,'bool'),(2,'f64'),(2.0,'i64'),(1<<63,'i64'),(-(1<<63)-1,'i64'),('2','i64')]:
            with self.subTest(value=value,schema=schema):self.assertIs(model.decode(value,schema),model.INVALID)
        self.assertEqual(model.decode(-(1<<63),'i64'),-(1<<63))
        self.assertEqual(model.decode(2.5,'f64'),2.5)

    def test_nullable_missing_and_json_null_are_not_failure(self):
        schema={'name':'string','note':model.nullable('string')}
        self.assertEqual(model.decode({'name':'x'},schema),{'name':'x','note':None})
        self.assertEqual(model.decode({'name':'x','note':None},schema),{'name':'x','note':None})
        self.assertEqual(model.decode({'payload':None},{'payload':'any'}),{'payload':None})
        self.assertIs(model.decode({},{'payload':'any'}),model.INVALID)
        self.assertIs(model.decode({'name':'x','note':3},schema),model.INVALID)

    def test_recursive_containers_and_unknown_field_policy(self):
        cases=[([1,True],model.array('i64')),({'k':False},model.mapping('i64')),
            ({},{'id':'i64'}),({'id':1,'extra':2},{'id':'i64'}),
            ([{'id':'bad'}],model.array({'id':'i64'})),({7:1},model.mapping('i64'))]
        for value,schema in cases:
            with self.subTest(value=value):self.assertIs(model.decode(value,schema),model.INVALID)
        self.assertEqual(model.decode({'id':1,'extra':2},{'id':'i64'},True),{'id':1})
        self.assertEqual(model.observations(),model.observations())


if __name__=='__main__':unittest.main()
