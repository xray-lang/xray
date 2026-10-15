import unittest
import complete_http_framing_model as model

class HttpTextTraces(unittest.TestCase):
    def test_complete_traces(self):
        expected={'http2_surface_direct':['bool'],
         'http_chunked_body_pure_direct':['Wikipedia',77,'ignored','soon','one|two','one','Wikipedia',True,5,*([True]*7)],
         'http_request_head_pure_direct':['POST','/submit?q=1','HTTP/1.1',-1,True,True,'example.com','one|two','one','4\r\ndata\r\n0\r\n\r\n','http://example.com/a',True,*([True]*9),'HEAD'],
         'http_request_message_pure_direct':['POST','/submit','hello',63,'one',0,'extra','Wikipedia',True,'ok','ok|newer',108,True,41,'éx',3,45,'é',2,*([True]*8)],
         'http_response_text_pure_direct':[200,'hello',57,'one',0,'extra','Wikipedia',True,'ok','ok|newer',102,'until-close',30,'éx',3,41,'é',2,True,27,*([True]*8)],
         'http_response_head_pure_direct':['HTTP/1.1',302,'Found',5,False,False,'/next?q=1','a=1|b=2','5',200,-1,True,True,True,*([True]*10),'https://a.test/next','https://a.test/done','GET',False,True,'https://b.test/next','POST',True,False,False,True]}
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),129)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_head_policy_rejections(self):
        for response in [False,True]:
            for raw in model.invalid_head_inputs(response):
                with self.subTest(response=response,raw=raw):self.assertIsNone(model.parse_head(raw,response))
        self.assertIsNone(model.parse_head('GET / HTTP/1.1\r\n folded: no\r\n\r\n'))
        accepted=model.parse_head('GET / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\nX: first\r\nx: second\r\n\r\n')
        self.assertEqual(accepted['contentLength'],5);self.assertEqual(model.first_header(accepted['headers'],'X'),'first')
        self.assertEqual(model.all_headers(accepted['headers'],'X'),['first','second'])

    def test_chunk_rejections_and_limits(self):
        cases=[('3\r\nab\r\n0\r\n\r\n',99,50),('z\r\nbad\r\n0\r\n\r\n',99,50),('2\r\nab\n0\r\n\r\n',99,50),
         ('2\r\nab\r\n0\r\n folded: nope\r\n\r\n',99,50),('4\r\nWiki\r\n0\r\nA: 1\r\nB: 2\r\n\r\n',99,1),('4\r\nWiki\r\n0\r\n\r\n',3,50)]
        for raw,body_limit,trailer_limit in cases:
            with self.subTest(raw=raw,body_limit=body_limit,trailer_limit=trailer_limit):self.assertIsNone(model.parse_chunks(raw,body_limit,trailer_limit))

    def test_utf8_byte_boundaries_and_wire_counts(self):
        self.assertIsNone(model.parse_chunks('1\r\né\r\n0\r\n\r\n'))
        parsed=model.parse_chunks('2\r\né\r\n0\r\n\r\n')
        self.assertEqual((parsed['body'],parsed['consumedBytes']),('é',12))
        raw='POST /utf8 HTTP/1.1\r\nContent-Length: 3\r\n\r\néxtrail';parsed=model.parse_message(raw)
        self.assertEqual((parsed['body'],parsed['consumedBytes']),('éx',45));self.assertEqual(raw.encode()[45:],b'trail')
        self.assertIsNone(model.parse_message('POST / HTTP/1.1\r\nContent-Length: 1\r\n\r\né'))

    def test_redirect_policy_is_explicit(self):
        cases=[(303,'https://a.test/form','/done','POST',('https://a.test/done','GET',False,True)),
         (302,'http://a.test:80/start','https://b.test/next','POST',('https://b.test/next','POST',True,False)),
         (308,'http://a.test/start','http://a.test:80/next','POST',('http://a.test:80/next','POST',False,False))]
        for status,current,location,method,expected in cases:
            with self.subTest(status=status):
                actual=model.redirect(status,current,location,method)
                self.assertEqual(tuple(actual[k] for k in ['url','method','stripSensitiveHeaders','dropBody']),expected)
        self.assertIsNone(model.redirect(200,'http://a.test','/next'))

if __name__=='__main__':unittest.main()
