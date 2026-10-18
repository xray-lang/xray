"""Independent full WebSocket helper trace, exact source port and packet controls."""
from provenance_test_support import provenance_input_path
from pathlib import Path
import hashlib,json
import unittest
from unittest import mock
import complete_websocket_protocol_model as m


class CompleteWebSocketProtocol(unittest.TestCase):
    def test_every_complete_original_observation(self):
        expected=[
            's3pPLMBiTxaQ9kYGzzhZRbK+xOo=',True,
            '/chat?room=1',True,'chat',True,'https://app.example','superchat',False,
            True,True,True,True,
            True,'example.com',443,'/chat?room=1',False,'::1',8080,'/','/?token=a',
            True,True,True,True,True,False,True,True,
            True,True,True,True,True,True,
            101,'chat',True,True,True,True,True,True,
            1000,'OK',1005,0,True,False,True,False,True,True,True,True,True,
            0,3,2,5,4,
            4,129,2,72,105,130,126,0,126,1,44,300,90,
            True,False,1,False,'Hi',4,True,'Hi','A',3,
            True,True,True,True,True,True,True,True,True]
        self.assertEqual(len(expected),94);self.assertEqual(m.observations(),expected)
        self.assertEqual(m.expected_rows()[0]['observation_count'],94)

    def test_rfc_accept_digest_and_key_width_are_independent_known_answers(self):
        self.assertEqual(m.accept_key(m.KEY),'s3pPLMBiTxaQ9kYGzzhZRbK+xOo=')
        self.assertTrue(m.valid_key(m.KEY))
        for key in ['bad','',m.KEY+'\n',m.base64.b64encode(b'A'*15).decode(),m.base64.b64encode(b'A'*17).decode()]:
            self.assertFalse(m.valid_key(key))

    def test_upgrade_request_checks_protocol_priority_origin_and_extension_permission(self):
        request=m.upgrade_request_text()
        self.assertEqual(m.parse_upgrade_request(request,['chat','superchat'])['protocol'],'chat')
        self.assertEqual(m.parse_upgrade_request(request,['superchat','chat'])['protocol'],'superchat')
        self.assertFalse(m.parse_upgrade_request(request,['chat'],False,['*'])['deflate'])
        self.assertIsNone(m.parse_upgrade_request(request,['chat'],True,['https://other.example']))
        for old,new in [('GET ','get '),('HTTP/1.1','HTTP/1.0'),('Sec-WebSocket-Version: 13','Sec-WebSocket-Version: 12'),('Connection: keep-alive, Upgrade','Connection: keep-alive')]:
            self.assertIsNone(m.parse_upgrade_request(request.replace(old,new)))

    def test_uri_authority_and_request_target_keep_ports_ipv6_and_query(self):
        self.assertEqual(m.parse_url('wss://example.com/chat?room=1'),m.Url(True,'example.com',443,'/chat?room=1'))
        self.assertEqual(m.parse_url('ws://[::1]:8080'),m.Url(False,'::1',8080,'/'))
        self.assertEqual(m.parse_url('ws://Example.com?token=a').path,'/?token=a')
        self.assertEqual(m.parse_url('ws://Example.com').host,'Example.com')
        for port in [1,65535]:self.assertEqual(m.parse_url('ws://example.com:'+str(port)).port,port)
        for url in ['http://example.com','ws://','ws://example.com:abc','ws://example.com:0','ws://example.com:65536','ws://[::1','ws://example.com/#fragment']:
            self.assertIsNone(m.parse_url(url))

    def test_client_header_construction_refuses_injection_without_entropy(self):
        url=m.Url(True,'example.com',443,'/chat');text=m.client_handshake(url,m.KEY,['chat'],True)
        self.assertIn('Host: example.com\r\n',text);self.assertIn('Sec-WebSocket-Protocol: chat\r\n',text)
        self.assertIn('Host: [::1]:8080\r\n',m.client_handshake(m.Url(False,'::1',8080,'/'),m.KEY,None,False))
        for bad in [m.Url(True,'a\nInjected',443,'/'),m.Url(True,'example.com',443,'/x\r\nInjected')]:
            self.assertIsNone(m.client_handshake(bad,m.KEY))
        self.assertIsNone(m.client_handshake(url,m.KEY,['chat\nbad']))
        self.assertIsNone(m.client_handshake(url,'bad'))

    def test_response_checks_accept_status_protocol_and_extension_negotiation(self):
        response=m.upgrade_response_text();self.assertEqual(m.parse_upgrade_response(response,m.KEY,['chat']),{'statusCode':101,'protocol':'chat','deflate':True})
        for text,protocols,allow in [(response.replace('101','200'),['chat'],True),(response.replace(m.accept_key(m.KEY),'bad'),['chat'],True),
                (response.replace('Upgrade: websocket\r\n',''),['chat'],True),(response,['other'],True),(response,['chat'],False)]:
            self.assertIsNone(m.parse_upgrade_response(text,m.KEY,protocols,allow))

    def test_close_policy_and_strict_utf8_preserve_valid_reasons(self):
        self.assertEqual(m.parse_close(b''),(1005,''))
        for text in ['OK','中文','🙂','A'*123]:self.assertEqual(m.parse_close(b'\x03\xe8'+text.encode()),(1000,text))
        for data in [b'\x03',b'\x03\xee',b'\x03\xe8\xc0\xaf',b'\x03\xe8\xe2\x82',b'\x03\xe8'+b'A'*124]:self.assertIsNone(m.parse_close(data))
        for code in [1000,1001,1011,3000,4999]:self.assertTrue(m.valid_close_code(code))
        for code in [999,1005,1006,1015,5000]:self.assertFalse(m.valid_close_code(code))

    def test_mask_xor_period_is_reversible_and_matches_exact_packet_bytes(self):
        key=bytes([1,2,3,4]);payload=bytes(range(256))
        self.assertEqual(m.mask_payload(m.mask_payload(payload,key),key),payload)
        self.assertEqual(m.frame(b'Hi',mask_key=key).hex(),'818201020304496b')
        self.assertEqual(m.mask_payload(b'abc',b''),bytes(3))

    def test_length_encodings_roundtrip_at_short_extended_and_64bit_boundaries(self):
        for length in [0,1,125,126,300,65535,65536]:
            wire=m.frame(bytes(length),2);parsed=m.parse_frame(wire)
            self.assertEqual(len(parsed.payload),length);self.assertEqual(parsed.consumed,len(wire))
            tag=wire[1]&127;self.assertEqual(tag,length if length<126 else 126 if length<65536 else 127)
        self.assertEqual(m.frame(b'Hi').hex(),'81024869')
        with self.assertRaises(ValueError):m.frame(b'Hi',mask_key=b'bad')

    def test_mask_role_enforcement_and_trailing_consumption_are_separate(self):
        masked=m.frame(b'Hi',mask_key=bytes([1,2,3,4]));unmasked=m.frame(b'A')
        self.assertTrue(m.parse_frame(masked,False,False).masked)
        self.assertIsNone(m.parse_frame(masked,False,True));self.assertIsNone(m.parse_frame(unmasked,True,True))
        parsed=m.parse_frame(unmasked+b'next');self.assertEqual(parsed.payload,b'A');self.assertEqual(parsed.consumed,3)

    def test_reserved_bits_control_rules_nonminimal_lengths_and_incomplete_data(self):
        for raw in [bytes([131,0]),bytes([9,0]),bytes([137,126,0,126]),bytes([129,126,0,1,65]),
                bytes([129,127,128,0,0,0,0,0,0,1,65]),bytes([129,2,65]),bytes([161,0])]:
            self.assertIsNone(m.parse_frame(raw))
        self.assertIsNone(m.parse_frame(bytes([193,0])))
        self.assertTrue(m.parse_frame(bytes([193,0]),allow_rsv1=True).rsv1)
        masked=m.frame(b'Hi',mask_key=bytes(4))
        for end in range(len(masked)):self.assertIsNone(m.parse_frame(masked[:end]))
        self.assertIsNone(m.parse_frame(m.frame(bytes(126)),max_payload=125))
        self.assertEqual(m.parse_frame(b'\x81\x01\xff').payload,b'\xff')

    def test_every_source_and_authority_is_pinned(self):
        saved=Path.read_bytes
        for name in m._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==provenance_input_path(m.ROOT, name) else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()

    def test_full_source_port_refuses_extra_changes_even_with_matching_new_digest(self):
        saved=Path.read_bytes;port=m.ROOT/m.PORT
        changed=saved(port).replace(b'string.fromUtf8(parsed.payload[:])',b'"Hi"')
        pins=dict(m._INPUTS);pins[m.PORT]=hashlib.sha256(changed.replace(b'\r\n',b'\n')).hexdigest()
        def read(path):return changed if path==port else saved(path)
        with mock.patch.object(Path,'read_bytes',read),mock.patch.object(m,'_INPUTS',pins),self.assertRaises(ValueError):m.expected_rows()
        original=(m.ROOT/m.ORIGINAL).read_text(encoding='utf-8')
        migrated=m.port_text(original)
        self.assertEqual(migrated.count('string.fromUtf8('),3);self.assertEqual(migrated.count('print('),94)

    def test_real_route_and_one_canonical_port_keep_the_original_identity(self):
        cases={x['id']:x for x in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']}
        route=next(x for x in json.loads((Path(__file__).parent/'runner_routes.json').read_text())['cases'] if x['case_id']==m.ORIGINAL)
        case=cases[m.IDENTITY];expected=m.expected_rows()[0]
        self.assertEqual(case['source'],m.PORT);self.assertEqual(case['preserved_original_identity']['source'],m.ORIGINAL)
        self.assertEqual(case['stdout_utf8'],expected['expected']['stdout_utf8']);self.assertEqual(case['entry'],'run')
        self.assertEqual(route['target_source_port']['fixture'],m.PORT);self.assertEqual(route['target_source_port']['qualification'],'NOT_RUN')
        self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')


if __name__=='__main__':unittest.main()
