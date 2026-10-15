import hashlib,unittest
from pathlib import Path
from unittest import mock
import complete_collection_protocols_model as model

class CollectionProtocols(unittest.TestCase):
    def test_full_callback_and_ref_programs(self):
        self.assertEqual(model.indexed_callback_trace(),[
            '[1000, 2000, 3000]','[1000, 2001, 3002]','[10, 20]','0:10','1:20'])
        self.assertEqual(model.ref_branch_trace(),[3,1,4,5,3,0])
    def test_complete_bool_map_order_and_class_field_program(self):
        expected='''-- insert false then true --
size=2
has true=true has false=true
pair false=34
pair true=21
key false
key true
val 34
val 21
get true=21 get false=34
-- insert true then false --
size=2
has true=true has false=true
pair true=7
pair false=9
key true
key false
val 7
val 9
-- update keeps position --
size=2
has true=true has false=true
pair true=70
pair false=9
key true
key false
val 70
val 9
-- delete + reinsert moves to end --
size=2
has true=true has false=true
pair false=9
pair true=700
key false
key true
val 9
val 700
-- delete to empty --
size=0 has true=false
-- float32 narrowing --
csize=2
cget true=1.5 cget false=2.25
cf true=1.5
cf false=2.25
-- class field --
fill=2
probe=55'''
        self.assertEqual(model.bool_map_trace(),expected.splitlines())
    def test_exact_bool_keys_and_f32_value_narrowing(self):
        mapping=model.BoolMap('f32');mapping.put(True,16777217);mapping.put(False,2.25)
        self.assertEqual(mapping.get(True),16777216.0)
        for key in [0,1,'true',None]:
            with self.subTest(key=key):
                for action in [lambda:mapping.put(key,1),lambda:mapping.get(key),lambda:mapping.has(key),lambda:mapping.delete(key)]:
                    with self.assertRaises(ValueError):action()
        saved=mapping.snapshot();mapping.put(True,1.5)
        self.assertEqual(list(mapping.items),[True,False]);self.assertEqual(saved[True],16777216.0)
        mapping.delete(True);mapping.put(True,3.0);self.assertEqual(list(mapping.items),[False,True])
    def test_cross_module_ref_aliases_and_independent_owned_snapshot(self):
        stack=[1,2,3,4,5];kinds=['I64']*5;alias=stack;saved=stack.copy();model.take_branch(stack,kinds,2,1)
        self.assertIs(alias,stack);self.assertEqual(alias,[1,4,5]);self.assertEqual(saved,[1,2,3,4,5]);self.assertEqual(kinds,['I64']*3)
        with self.assertRaises(ValueError):model.take_branch(stack,kinds,4,0)
        self.assertEqual(stack,[1,4,5])
    def test_integer_endpoint_and_explicit_text_program(self):
        rows=model.observations();minimum=-(1<<63);maximum=(1<<63)-1
        self.assertEqual(rows['generic_int_format_shared_core'],[minimum,maximum,
            'left=-9223372036854775808','9223372036854775807=right',
            '-9223372036854775808|9223372036854775807|-42',
            '[-9223372036854775808, 9223372036854775807, -42]',
            'n:-9223372036854775808:null:true:9223372036854775807'])
        self.assertEqual(sum(map(len,rows.values())),65)
    def test_all_delivered_sources_and_authorities_are_frozen(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            with self.subTest(source=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==model.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
    def test_exact_program_port_and_required_input_identities(self):
        swapped=dict(model._PROGRAMS);a,b='hof_index_param','map_bool_key';swapped[a],swapped[b]=swapped[b],swapped[a]
        for name,bad in [('_PROGRAMS',swapped),('_PROGRAMS',{}),('_PORTS',{}),('_INPUTS',{}),('_REQUIRED_INPUTS',())]:
            with self.subTest(attribute=name),mock.patch.object(model,name,bad),self.assertRaises(ValueError):model.expected_rows()
    def test_matching_digest_cannot_hide_an_extra_statement_in_the_port(self):
        saved=Path.read_bytes
        for name in model._PORTS.values():
            with self.subTest(source=name):
                raw=saved(model.ROOT/name)+b'\nprint("extra")\n';inputs=dict(model._INPUTS)
                inputs[name]=hashlib.sha256(raw.replace(b'\r\n',b'\n')).hexdigest()
                def changed(path):return raw if path==model.ROOT/name else saved(path)
                with mock.patch.object(model,'_INPUTS',inputs),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()

if __name__=='__main__':unittest.main()
