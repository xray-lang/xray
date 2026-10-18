import unittest
import complete_text_formats_model as model

class TextFormatTraces(unittest.TestCase):
    def test_complete_traces(self):
        expected={'url_core_direct':['hello%20%E4%B8%96%E7%95%8C%21','hello 世界!','a+b%2Bc','a b+c','https:','example.com','8080','/path','?q=1','#top','example.com:8080','https://example.com:8080','https://example.com:8080/path?q=1#top','http://user:pass@example.com/path','hello world','a&b','','','hello world','skip','msg=hello+world&key=a%26b',9,'https://example.com/a/b/d.html','https://example.com/a/b/c?new=2','https://example.com/a/b/c?old=1#frag','https://cdn.example.com/lib.js','https://example.com/a/d/e','/api/v1/users','/api/v1/users'],
         'xml_pure_module_direct':['root','7','urn:x',2,'A&B','cdata','<raw>','text','plain',2,'comment','ok',0,'entry','42','body','entry','text','<hello>&','<msg>Hello&lt;World</msg>','<entry id="42">body</entry>'],
         'yaml_pure_module_direct':['xray',443,100,2,'done',2,'map','seq','name,ports,limits','hello','hello','has-line','no-newline',2,1,2,'Alice',0,0,'xray',1,'debug-off']}
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),72)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_query_global_offsets_and_empty_values(self):
        for raw,offset in [('ok=1&bad=%GG',9),('x=%',2),('%GG=1',0)]:
            with self.subTest(raw=raw):
                with self.assertRaises(model.PercentError) as error:model.query(raw)
                self.assertEqual(error.exception.offset,offset)
        self.assertEqual(model.query('?empty=&flag&&=skip'),[('empty',''),('flag',''),('','skip')])

    def test_yaml_types_alias_chomping_and_documents(self):
        for token,expected in [('false',False),('1',1),('"1"','1')]:
            with self.subTest(token=token):
                actual=model.yaml_scalar(token,{});self.assertEqual(type(actual),type(expected));self.assertEqual(actual,expected)
        values,anchors=model.parse_yaml('anchor: &v hello\nalias: *v');self.assertEqual(values,{'anchor':'hello','alias':'hello'});self.assertEqual(anchors,{'v':'hello'})
        self.assertEqual(model.parse_yaml('text: |-\n  one\n  two')[0],{'text':'one\ntwo'})
        self.assertEqual(model.yaml_documents('---\na: 1\n...\nb: 2'),[{'a':1},{'b':2}])

    def test_xml_preserved_node_kinds(self):
        for cdata,comments,expected in [(True,False,['cdata']),(False,False,['text']),(True,True,['comment','cdata'])]:
            with self.subTest(cdata=cdata,comments=comments):
                root=model.xml_parse('<root><!-- c --><![CDATA[x]]></root>',cdata,comments)
                self.assertEqual([model.node_kind(c) for c in root.childNodes],expected);self.assertEqual(model.text_content(root),'x')

    def test_roundtrip_and_uri_resolution(self):
        root=model.xml_parse('<msg>Hello&lt;World</msg>');self.assertEqual(root.toxml(),'<msg>Hello&lt;World</msg>')
        self.assertEqual(model.text_content(model.xml_parse('<root>left<child>hidden</child><![CDATA[right]]></root>')),'leftright')
        value,_=model.parse_yaml('name: "xray"\nversion: 1\ndebug: false');self.assertIs(value['debug'],False)
        self.assertEqual(model.join_url('','/api/','','v1','/users'),'/api/v1/users')

if __name__=='__main__':unittest.main()
