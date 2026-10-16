"""Full original SIMD assertions, independent bit/lane controls and input guards."""
from pathlib import Path
import itertools,json,unittest
from unittest import mock
import complete_simd_vectors_model as m


class CompleteSimdVectors(unittest.TestCase):
    def test_complete_original_assertion_and_output_denominators(self):
        rows=m.expected_rows();self.assertEqual([r['observation_count'] for r in rows],[1,1])
        self.assertEqual([r['required_original_assertion_count'] for r in rows],[83,24])
        self.assertEqual([r['expected']['stdout_utf8'] for r in rows],['OK simd portable integer vectors\n','OK simd top-level shared vectors\n'])
        for row in rows:
            facts=row['required_original_assertions'];self.assertTrue(all(x['passed'] and x['actual']==x['expected'] for x in facts))
            self.assertEqual(len({x['label'] for x in facts}),len(facts))
        self.assertEqual(rows[0]['required_semantic_trace']['roundtrip_lane_assertions'],32)
        self.assertEqual(rows[0]['required_semantic_trace']['initialization_loop_iterations'],{'bytes32':32,'bytes512':64,'nativeBytesBuffer':64})

    def test_every_original_store_has_full_known_answer_and_preserved_tail(self):
        stores={x['label']:x for x in m.portable_trace()['original_stores']}
        self.assertEqual(len(stores),7)
        self.assertEqual(stores['out']['after'],[None,18446744073709551615,9,None])
        self.assertEqual(stores['wideOut']['after'],[32,42,100,126])
        self.assertEqual(stores['wideOut zero']['after'],[0]*4)
        self.assertEqual(stores['bytes512']['before'],[(i*13+5)&255 for i in range(64)])
        self.assertEqual(stores['bytes512']['after'],[0]*64)
        self.assertEqual(stores['out512']['after'],[25769803798,51539607574,77309411398,103079215190,128849019030,154618822838,180388626694,206158430518])
        self.assertEqual(stores['out512 roundtrip']['after'],[10,20,30,40,50,60,70,80])
        self.assertEqual(stores['nativeOut']['after'],[7,7,99,99,99,99,99,99])

    def test_all_native_prefix_and_boolean_branches_preserve_original_observation(self):
        for width,accelerated,scalable in itertools.product([16,32,64],[False,True],[False,True]):
            with self.subTest(width=width,accelerated=accelerated,scalable=scalable):
                trace=m.portable_trace(width,accelerated,scalable)
                self.assertEqual(trace['stdout_utf8'],'OK simd portable integer vectors\n')
                self.assertEqual(len(trace['original_assertions']),81+width//8)
                self.assertEqual(trace['native_out'],[7]*(width//8)+[99]*(8-width//8))
        for width in [0,8,24,128]:
            with self.assertRaises(ValueError):m.portable_trace(width)

    def test_global_fixed_layout_lanes_and_complete_roundtrip(self):
        trace=m.top_level_trace()
        self.assertEqual(trace['global64_lanes'],[18446744073709551615,9])
        self.assertEqual(trace['global32_lanes'],[4294967295,4294967295,9,0])
        self.assertEqual(trace['global_byte_roundtrip'],list(range(1,17)))
        vector=m.Vector(8,tuple(range(1,17)))
        self.assertEqual(vector.reinterpret(32).lanes,(67305985,134678021,202050057,269422093))
        self.assertEqual(vector.reinterpret(64).lanes,(578437695752307201,1157159078456920585))

    def test_lane_arithmetic_wraps_at_exact_unsigned_width(self):
        for width in [8,32,64]:
            maximum=(1<<width)-1;a=m.Vector(width,(maximum,0,3,9));b=m.Vector.splat(width,4,2)
            self.assertEqual(a.add(b).lanes,(1,2,5,11))
            self.assertEqual(a.mul(b).lanes,(maximum-1,0,6,18))
            self.assertEqual(a.xor(a).lanes,(0,0,0,0))
            self.assertEqual(a.lanes,(maximum,0,3,9))
        self.assertEqual(m.Vector(64,(18446744073709551615,1)).reduce(),0)

    def test_shifts_use_modulo64_then_exact_lane_width(self):
        for width in [8,32,64]:
            v=m.Vector(width,(1,(1<<width)-1))
            for count in [-65,-64,-1,0,1,8,31,32,63,64,65,127]:
                effective=count%64
                self.assertEqual(v.shift(count).lanes,tuple(x//(1<<effective) for x in v.lanes))
                self.assertEqual(v.shift(count,True).lanes,tuple((x*(1<<effective))%(1<<width) for x in v.lanes))
        self.assertEqual(m.Vector(32,(1,2)).shift(32,True).lanes,(0,0))
        self.assertEqual(m.Vector(8,(1,2)).shift(64,True).lanes,(1,2))

    def test_shuffle_unzip_replace_and_swap_are_value_operations(self):
        a=m.Vector(32,(2,3,4,5));b=m.Vector.splat(32,4,7)
        self.assertEqual(a.shuffle([3,2,1,0]).lanes,(5,4,3,2))
        self.assertEqual(a.swap().lanes,(3,2,5,4));self.assertEqual(a.swap().swap(),a)
        self.assertEqual(a.unzip(b).lanes,(2,4,7,7));self.assertEqual(a.unzip(b,True).lanes,(3,5,7,7))
        self.assertEqual(a.replace(1,21).lanes,(2,21,4,5));self.assertEqual(a.lanes,(2,3,4,5))

    def test_widen_uses_selected_source_lanes_and_full64bit_products(self):
        a=m.Vector(32,(2,3,4,5));b=m.Vector.splat(32,4,7)
        self.assertEqual(a.widen(b,[0,2]).lanes,(14,28));self.assertEqual(a.widen(b,[1,3]).lanes,(21,35))
        self.assertEqual(a.widen(b,[0,1]).lanes,(14,21));self.assertEqual(a.widen(b,[2,3]).lanes,(28,35))
        maximum=m.Vector.splat(32,4,4294967295)
        self.assertEqual(maximum.widen(maximum,[0,2]).lanes,(18446744065119617025,)*2)

    def test_little_byte_reinterpretation_is_host_independent(self):
        for size in [16,32,64]:
            data=bytes((i*17+3)&255 for i in range(size));byte_vector=m.Vector(8,tuple(data))
            for width in [32,64]:
                lanes=byte_vector.reinterpret(width)
                self.assertEqual(lanes.reinterpret(8),byte_vector)
                self.assertEqual(b''.join(x.to_bytes(width//8,'little') for x in lanes.lanes),data)
        self.assertEqual(m.Vector(64,(0x0102030405060708,0)).reinterpret(8).lanes[:8],(8,7,6,5,4,3,2,1))

    def test_vector_load_store_are_checked_and_preserve_other_slots(self):
        source=[0,1,2,3,4,5];vector=m.Vector.load(32,4,source,1);destination=[99]*6
        vector.store(destination,1);self.assertEqual(destination,[99,1,2,3,4,99]);self.assertEqual(source,[0,1,2,3,4,5])
        for offset in [-1,3]:
            with self.assertRaises(ValueError):m.Vector.load(32,4,source,offset)
            before=destination.copy()
            with self.assertRaises(ValueError):vector.store(destination,offset)
            self.assertEqual(destination,before)

    def test_changed_wrap_or_lane_order_breaks_original_assertions(self):
        with mock.patch.object(m.Vector,'add',lambda a,b:a),self.assertRaisesRegex(ValueError,'Original SIMD assertion'):
            m.expected_rows()
        saved=m.Vector.reinterpret
        def wrong_order(vector,width):
            result=saved(vector,width)
            return m.Vector(result.width,tuple(reversed(result.lanes)))
        with mock.patch.object(m.Vector,'reinterpret',wrong_order),self.assertRaisesRegex(ValueError,'Original SIMD assertion'):
            m.expected_rows()

    def test_invalid_vector_shapes_and_noncanonical_lanes_fail_closed(self):
        for width,lanes in [(16,(0,)),(32,()),(8,(256,)),(64,(-1,)),(8,(True,))]:
            with self.assertRaises(ValueError):m.Vector(width,lanes)
        a=m.Vector(32,(1,2,3,4))
        with self.assertRaises(ValueError):a.add(m.Vector(64,(1,2)))
        with self.assertRaises(ValueError):a.shuffle([0,1,2,4])
        with self.assertRaises(ValueError):a.widen(a,[0])
        with self.assertRaises(ValueError):a.replace(4,0)
        with self.assertRaises(ValueError):a.reinterpret(16)

    def test_every_complete_source_and_normative_input_is_pinned(self):
        saved=Path.read_bytes
        for name in m._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_NAMES',()),self.assertRaises(ValueError):m.expected_rows()

    def test_actual_product_registrations_keep_all_original_source_and_obligations(self):
        folder=Path(__file__).parent;cases={x['id']:x for x in json.loads((folder/'product_driver_cases.json').read_text())['cases']}
        routes={x['case_id']:x for x in json.loads((folder/'runner_routes.json').read_text())['cases']}
        for row in m.expected_rows():
            case=cases[row['id']];route=routes[row['source']]
            self.assertEqual(case['source'],case['preserved_original_identity']['source']);self.assertEqual(case['source'],row['source'])
            self.assertEqual(case['source_context'],'original_absolute');self.assertEqual(case['entry'],'run')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8']);self.assertEqual(case['owners'],route['producer_packages'])
            self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'');self.assertNotIn('target_source_port',route)
            self.assertIn('Legacy ref Slice',case['independent_execution_model'])


if __name__=='__main__':unittest.main()
