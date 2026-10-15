import datetime,unittest
import complete_http_policy_model as model

class HttpPolicyTraces(unittest.TestCase):
    def test_complete_traces(self):
        expected={'http_cookie_pure_direct':['session','abc123','example.com','/account',True,True,'session=abc123',True,True,'None',True,True,True,True,2,'session=abc123',None,False,'domain=d',None,3,'session=def',3,4,3,0],
         'http_pure_helpers_direct':['a%20b%2B%E5%BC%A0','%3A%2F%3F%23%5B%5D%40%21%24%26%27%28%29%2A%2B%2C%3B%3D','a b+c%ZZ%','张三',*([True]*22)],
         'http_request_text_pure_direct':['basic','GET /api?q=1 HTTP/1.1|X-Test: ok|Host: example.com|User-Agent: xray-http/1.0|Accept: */*|Connection: keep-alive|Accept-Encoding: gzip, deflate||',
          'body','POST /submit HTTP/1.1|Content-Type: text/plain|Host: example.com|User-Agent: xray-http/1.0|Accept: */*|Connection: keep-alive|Accept-Encoding: gzip, deflate|Content-Length: 5||hello',
          'custom-host','GET / HTTP/1.1|Host: override.test|User-Agent: xray-http/1.0|Accept: */*|Connection: keep-alive|Accept-Encoding: gzip, deflate||',True,False,False,False,True,True,True,True,True],
         'http_router_pure_direct':[True,True,True,True,True,'user','42','me',True,'post-user','42','file','a/b/c.txt','root',True,False,False,False,True,'replacement',True,2,'a/b']}
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),90)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_cookie_lifecycle_and_match_boundaries(self):
        jar=model.Jar();value=model.cookie('x=a; Path=/api','example.com','/',1000);jar.add(value,1000)
        self.assertEqual(jar.header('example.com','/api/v1',False,1000),'x=a')
        self.assertIsNone(jar.header('www.example.com','/api/v1',False,1000));self.assertIsNone(jar.header('example.com','/apiary',False,1000))
        jar.add(model.cookie('x=b; Domain=example.com; Path=/api','example.com','/',1000),1000)
        self.assertEqual(len(jar.values),1);self.assertEqual(jar.header('www.example.com','/api/v1',False,1000),'x=b')
        jar.add(model.cookie('x=gone; Domain=example.com; Path=/api; Max-Age=0','example.com','/',1000),1000)
        self.assertEqual(jar.values,[])

    def test_header_error_channel_and_reserved_framing(self):
        for value in ['a\r\nInjected: yes','a\rno','a\nno']:
            with self.subTest(value=value):
                with self.assertRaises(model.HeaderError):model.checked_headers([('X-Test',value)])
        for name in ['Content-Length','Transfer-Encoding']:
            with self.subTest(name=name):self.assertIsNone(model.response_text(200,'body',headers=[(name,'999')]))

    def test_route_precedence_replacement_and_invalid_patterns(self):
        router=model.Router();router.add('GET','/x/:id','dynamic');router.add('GET','/x/me','static');router.add('POST','/x/:id','post');router.add('GET','/file/*tail','file')
        for method,path,value in [('GET','/x/me','static'),('POST','/x/7','post'),('GET','/file/a/b','file')]:
            with self.subTest(method=method,path=path):self.assertEqual(router.find(method,path)[0],value)
        router.add('GET','/x/:id','new');self.assertEqual(router.find('get','/x/7?debug=1'),('new',{'id':'7'}))
        for method,path in [('', '/bad'),('GET','bad'),('GET','/bad/*tail/more')]:
            with self.subTest(method=method,path=path):self.assertFalse(router.add(method,path,'invalid'))

    def test_wire_bytes_and_expiry_without_wall_clock(self):
        response=model.response_text(200,'é');self.assertIn('Content-Length: 2\r\n',response);self.assertTrue(response.endswith('\r\n\r\né'))
        request=model.request_text('POST','/',body='é',host='a.test');self.assertIn('Content-Length: 2\r\n',request)
        epoch=datetime.datetime(1970,1,1,tzinfo=datetime.timezone.utc);date=datetime.datetime(1994,11,6,8,49,37,tzinfo=datetime.timezone.utc)
        self.assertEqual(int((date-epoch).total_seconds()),784111777)
        self.assertEqual(model.cookie('x=y; Expires=Sun, 06 Nov 1994 08:49:37 GMT','example.com','/',1000)['expires'],784111777)

if __name__=='__main__':unittest.main()
