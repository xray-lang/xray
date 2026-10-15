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

if __name__=='__main__':unittest.main()
