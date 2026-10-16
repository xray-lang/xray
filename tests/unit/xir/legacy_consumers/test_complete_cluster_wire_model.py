"""Whole original cluster traces, packet known answers and source tamper controls."""
from pathlib import Path
from fractions import Fraction
import hashlib,json,unittest
from unittest import mock
import complete_cluster_wire_model as m

PACKETS={
 'raw':'0000000404aabbcc',
 'transport':'0000005007030d62656e63682e7061796c6f6164000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f',
 'request':'0000002101060a6e6f64652d616c706861101112131415161718191a1b1c1d1e1f01020304',
 'ack':'000000400206096e6f64652d62657461202122232425262728292a2b2c2d2e2f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f0a0b0c0d',
 'done':'0000002103c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf',
 'heartbeat':'00000009050102030405060708',
 'monitor':'0000000e080c776f726b65722e616c706861',
 'exit':'000000150a0c776f726b65722e616c706861066e6f726d616c'}


class CompleteClusterWire(unittest.TestCase):
    def test_all54_original_observations_and_complete_packets(self):
        p={name:bytes.fromhex(value) for name,value in PACKETS.items()}
        expected=[p['raw'],4,bytes.fromhex('aabbcc'),True,True,True,True,
            p['transport'],3,'bench.payload',64,0,63,True,True,
            p['request'],'node-alpha',bytes.fromhex('101112131415161718191a1b1c1d1e1f'),16909060,True,True,
            p['ack'],'node-beta',bytes.fromhex('202122232425262728292a2b2c2d2e2f'),bytes(range(128,160)),168496141,True,True,
            p['done'],bytes(range(192,224)),True,p['heartbeat'],72623859790382856,True,
            p['monitor'],'worker.alpha',True,p['exit'],'worker.alpha','normal',True,
            bytes.fromhex('deb1d34e704143aea767d3481e679477edd8260b8013ba095f8e72686ed70fb0'),True,False,
            2,True,True,100,22,False,True,True,False]
        self.assertEqual(len(expected),53);trace=m.wire_trace()
        self.assertEqual(trace['observations'],expected);self.assertEqual(trace['full_packet_hex'],PACKETS)
        rows=m.expected_rows();self.assertEqual([r['observation_count'] for r in rows],[1,53])
        self.assertEqual(rows[0]['expected']['stdout_utf8'],'true\n')
        self.assertEqual(rows[1]['expected']['stdout_utf8'],''.join(m.display(x)+'\n' for x in expected))

    def test_every_original_assertion_and_record_loop_is_retained(self):
        trace=m.wire_trace();checks=trace['original_assertions']
        self.assertEqual(len(checks),59);self.assertEqual(len({x['label'] for x in checks}),59)
        self.assertTrue(all(x['passed'] and x['actual']==x['expected'] for x in checks))
        self.assertEqual((trace['detector_record_calls'],trace['detector_interval_updates']),(123,122))
        self.assertEqual(len([x for x in checks if x['label'].startswith('heartbeat')]),11)

    def test_original_assertions_catch_changed_encoded_bytes(self):
        saved=m.encode_frame
        def altered(kind,payload):
            value=saved(kind,payload)
            return value[:-1]+bytes([value[-1]^1]) if kind==4 and value else value
        with mock.patch.object(m,'encode_frame',altered),self.assertRaisesRegex(ValueError,'raw'):
            m.expected_rows()

    def test_frame_lengths_exact_consumption_and_retired_kind(self):
        wire=bytes.fromhex(PACKETS['raw'])
        for end in range(len(wire)):self.assertIsNone(m.decode_frame(wire[:end]))
        for data in [wire+b'next',bytes.fromhex('0000000004'),bytes.fromhex('0100000104'),bytes.fromhex('0000000109')]:
            self.assertIsNone(m.decode_frame(data))
        for kind in [1,2,3,4,5,6,7,8,10]:self.assertEqual(m.decode_frame(m.encode_frame(kind,b'')),(kind,b''))
        for kind in [-1,0,9,11,255,256]:self.assertIsNone(m.encode_frame(kind,b''))

    def test_transport_topic_hop_and_full_opaque_envelope(self):
        for hop in [0,255]:
            wire=m.encode_transport(hop,'a.b',bytes(range(64)))
            self.assertEqual(m.decode_transport(wire),(hop,'a.b',bytes(range(64))))
        for topic in ['', '.', 'a.', '.a', 'a..b', 'a.*', 'a.>', 'a b', '中文', 'a'*128]:
            self.assertIsNone(m.encode_transport(3,topic,bytes(64)))
        for hop in [-1,256]:self.assertIsNone(m.encode_transport(hop,'a',bytes(64)))
        self.assertIsNone(m.encode_transport(3,'a',bytes(63)))
        for body in [b'',b'\x03\x00'+bytes(64),b'\x03\x01\xff'+bytes(64),b'\x03\x02a'+bytes(64)]:
            self.assertIsNone(m.decode_transport(m.encode_frame(7,body)))
        self.assertIsNone(m.decode_transport(m.encode_frame(8,b'a'+bytes(64))))

    def test_handshake_width_version_flags_and_minimum_complete_names(self):
        for ack in [False,True]:
            fixed=54 if ack else 22;minimum=bytes([6,1,97])+bytes(fixed-2)
            self.assertIsNotNone(m.decode_handshake(minimum,ack))
            for data in [minimum[:-1],minimum+b'\x00',bytes([5])+minimum[1:],bytes([6,0])+bytes(fixed-2),bytes([6,1,255])+minimum[3:]]:
                self.assertIsNone(m.decode_handshake(data,ack))
            for flags in [0,0xffffffff]:
                wire=m.encode_handshake('a'*63,bytes(16),flags,bytes(32) if ack else None)
                self.assertEqual(m.decode_handshake(m.decode_frame(wire)[1],ack)[-1],flags)
        for name in ['', 'a'*64, 'a\n', '中文']:
            self.assertIsNone(m.encode_handshake(name,bytes(16),0))
        for nonce in [bytes(15),bytes(17)]:self.assertIsNone(m.encode_handshake('a',nonce,0))
        for flags in [-1,0x100000000]:self.assertIsNone(m.encode_handshake('a',bytes(16),flags))
        for proof in [bytes(31),bytes(33)]:self.assertIsNone(m.encode_handshake('a',bytes(16),0,proof))

    def test_done_and_heartbeat_width_kind_and_big_endian(self):
        for size in [0,31,33]:
            self.assertIsNone(m.encode_done(bytes(size)));self.assertIsNone(m.decode_done(bytes(size)))
        self.assertEqual(m.decode_done(bytes(32)),bytes(32))
        for kind in [5,6]:self.assertEqual(m.decode_heartbeat(m.decode_frame(m.encode_heartbeat(kind,72623859790382856))[1]),72623859790382856)
        self.assertIsNone(m.encode_heartbeat(4,1))
        for size in [0,7,9]:self.assertIsNone(m.decode_heartbeat(bytes(size)))
        self.assertEqual(m.encode_heartbeat(5,72623859790382856).hex(),PACKETS['heartbeat'])

    def test_monitor_and_exit_name_policy_strict_utf8_and_byte_length(self):
        self.assertEqual(m.decode_monitor(m.decode_frame(m.encode_monitor('a'*127))[1]),'a'*127)
        for name in ['', 'a'*128, 'a\n', '中文']:self.assertIsNone(m.encode_monitor(name))
        for body in [b'',b'\x01',b'\x01\xff',b'\x01ab']:self.assertIsNone(m.decode_monitor(body))
        for reason in ['', '中文🙂', '\x00', 'a'*255]:
            self.assertEqual(m.decode_exit(m.decode_frame(m.encode_exit('a',reason))[1]),('a',reason))
        self.assertIsNone(m.encode_exit('a','中'*86))
        for body in [b'\x01a\x01',b'\x01a\x01\xff',b'\x01a\x00x',b'\x00\x00']:
            self.assertIsNone(m.decode_exit(body))

    def test_hmac_known_answer_and_proof_comparison_is_value_only(self):
        digest=m.proof('projection-secret',bytes(range(16,32)))
        self.assertEqual(digest.hex(),'deb1d34e704143aea767d3481e679477edd8260b8013ba095f8e72686ed70fb0')
        self.assertTrue(m.proof_equal(digest,bytes(digest)))
        self.assertFalse(m.proof_equal(digest,bytes(32)))
        for size in [0,15,17]:self.assertIsNone(m.proof('projection-secret',bytes(size)))
        for size in [0,31,33]:self.assertFalse(m.proof_equal(bytes(size),bytes(size)))
        # RFC4231 case1 independently checks the selected standard primitive.
        self.assertEqual(m.hmac.digest(bytes([11])*20,b'Hi There','sha256').hex(),'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7')

    def test_phi_window_recomputes_rational_stats_and_wrap_count(self):
        detector=m.Detector();self.assertEqual(detector.stats,(Fraction(5000),Fraction(100)))
        for now in [1000,6000,11000]:detector.record(now)
        self.assertEqual(detector.stats,(Fraction(5000),Fraction(1)))
        self.assertLess(detector.value(11000),.001);self.assertAlmostEqual(detector.value(16000),math_log10_two(),14)
        for i in range(120):detector.record(16000+i*5000)
        self.assertEqual((len(detector.window),detector.updates,detector.calls),(100,122,123))
        self.assertEqual(detector.stats,(Fraction(5000),Fraction(1)))
        # A nonconstant window proves the oracle does not rely on constant inputs.
        detector=m.Detector()
        for now in [1,2,5,10]:detector.record(now)
        self.assertEqual(detector.stats,(Fraction(3),Fraction(8,3)))
        detector.record(8);self.assertEqual(list(detector.window),[1,3,5,0])

    def test_phi_and_warmup_policy_preserve_strict_thresholds(self):
        self.assertFalse(m.heartbeat_dead(8,3,999999,99,15000,3))
        self.assertTrue(m.heartbeat_dead(8.000001,3,0,0,15000,3))
        self.assertFalse(m.heartbeat_dead(99,2,15000,99,15000,3))
        self.assertFalse(m.heartbeat_dead(99,2,15001,1,15000,3))
        self.assertTrue(m.heartbeat_dead(0,2,15001,2,15000,3))
        detector=m.Detector();self.assertEqual(detector.value(99999),0)
        detector.record(1);detector.record(2);self.assertEqual(detector.value(99999),0)
        detector.record(3);self.assertEqual(detector.value(99999),15)

    def test_array_format_cap_preserves_full_packets_separately(self):
        self.assertEqual(m.display(b''),'[]')
        self.assertEqual(m.display(bytes(range(32))),'['+', '.join(str(x) for x in range(32))+']')
        self.assertEqual(m.display(bytes(range(33))),'['+', '.join(str(x) for x in range(32))+', ...(1 more)]')
        self.assertTrue(m.display(bytes.fromhex(PACKETS['transport'])).endswith(', ...(52 more)]'))
        self.assertEqual(len(bytes.fromhex(m.wire_trace()['full_packet_hex']['transport'])),84)

    def test_all_source_and_authority_inputs_are_pinned(self):
        saved=Path.read_bytes
        for name in m._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()

    def test_typed_nullable_call_retains_four_declarations(self):
        row=m.expected_rows()[0]
        self.assertEqual(row['required_nullable_parameter_types'],['ClusterConfig?','ClusterInfo?','ClusterNodeInfo?','ClusterTlsOptions?'])
        self.assertTrue(m.typed_accepts(None,None,None,None))
        with self.assertRaises(ValueError):m.typed_accepts({},None,None,None)
        with mock.patch.object(m,'_NAMES',()),self.assertRaises(ValueError):m.expected_rows()

    def test_real_run_routes_and_products_retain_whole_original_identity(self):
        folder=Path(__file__).parent
        cases={r['id']:r for r in json.loads((folder/'product_driver_cases.json').read_text())['cases']}
        routes={r['case_id']:r for r in json.loads((folder/'runner_routes.json').read_text())['cases']}
        for row in m.expected_rows():
            case=cases[row['id']];route=routes[row['source']]
            self.assertEqual(case['source'],case['preserved_original_identity']['source'])
            self.assertEqual(case['source_context'],'original_absolute');self.assertEqual(case['entry'],'run')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8'])
            self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')
            self.assertEqual(case['owners'],route['producer_packages'])
            self.assertNotIn('target_source_port',route)


def math_log10_two():
    from decimal import Decimal,localcontext
    with localcontext() as context:
        context.prec=50
        return float(Decimal(2).log10())


if __name__=='__main__':unittest.main()
