"""Independent semantics of three complete approved URL/XML/YAML fixtures.

The small YAML reference grammar is limited to the reviewed literals; it is
not a conformance parser or an additional dependency of the Xray product.
"""
from pathlib import Path
import hashlib,json,re
from urllib.parse import quote,quote_plus,unquote,unquote_plus,urlsplit,urlunsplit,urljoin,parse_qsl,urlencode
from xml.dom import minidom,Node

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'stdlib/text/text.xr': '55d2aaa9e524bc1924f6b426b763f9d45e52f926f204ded84c7e8a64289b9aef',
 'stdlib/url/url.xr': 'ed9ff523e0d27db91045e6729d3922675f324f9f4bd95874ed801b0b732625a4',
 'stdlib/xml/xml.xr': '080a8aba0815c850af5b226e7131f7d07211d2e895db31f168a510530891ab31',
 'stdlib/yaml/yaml.xr': '8ae728f5e4b0c0d157006d68582a60ce4da1d450b8509d4fe984be5237d066ae',
 'tests/diff/cases/semantics/stdlib/url_core_direct.xr': 'c9fa2bafd6d1d7037964c55c93d237ed29d7c2912d26b1ce5ea21f717d34f527',
 'tests/diff/cases/semantics/stdlib/xml_pure_module_direct.xr': 'eae16fdff59aa1a8ae6827a8c451a95e97ca9df332246d8f0451610d81778b9a',
 'tests/diff/cases/semantics/stdlib/yaml_pure_module_direct.xr': '7a2a7c5f23bad79826647420ffe7fac19f6675853a18cab131dfb0d2e3f2ddfe'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/016-15-standard-library.md',
 'stdlib/text/text.xr',
 'stdlib/url/url.xr',
 'stdlib/xml/xml.xr',
 'stdlib/yaml/yaml.xr',
 'tests/diff/cases/semantics/stdlib/url_core_direct.xr',
 'tests/diff/cases/semantics/stdlib/xml_pure_module_direct.xr',
 'tests/diff/cases/semantics/stdlib/yaml_pure_module_direct.xr')
_PROGRAMS = {'url_core_direct': 'tests/diff/cases/semantics/stdlib/url_core_direct.xr',
 'xml_pure_module_direct': 'tests/diff/cases/semantics/stdlib/xml_pure_module_direct.xr',
 'yaml_pure_module_direct': 'tests/diff/cases/semantics/stdlib/yaml_pure_module_direct.xr'}
_NAMES=frozenset({'url_core_direct','xml_pure_module_direct','yaml_pure_module_direct'})

class PercentError(ValueError):
    def __init__(self,offset):self.offset=offset

def query(raw):
    data=raw.encode('utf-8')
    for offset,value in enumerate(data):
        if value==37 and (offset+2>=len(data) or not re.fullmatch(b'[0-9a-fA-F]{2}',data[offset+1:offset+3])):raise PercentError(offset)
    return parse_qsl(raw.removeprefix('?'),keep_blank_values=True)

def join_url(*parts):
    result=''
    for value in parts:
        if not value:continue
        result=result.removesuffix('/')
        result+=('/' if result and not value.startswith('/') else '')+value
    return result

def yaml_scalar(raw,anchors):
    raw=raw.strip()
    if raw.startswith('&'):
        name,value=raw[1:].split(None,1);anchors[name]=yaml_scalar(value,anchors);return anchors[name]
    if raw.startswith('*'):return anchors[raw[1:]]
    if raw.startswith('[') and raw.endswith(']'):return [yaml_scalar(x,anchors) for x in raw[1:-1].split(',') if x.strip()]
    if raw.startswith('{') and raw.endswith('}'):
        return {key.strip():yaml_scalar(value,anchors) for key,value in (part.split(':',1) for part in raw[1:-1].split(','))}
    if raw.startswith('"'):return json.loads(raw)
    if raw in {'true','false'}:return raw=='true'
    if raw in {'null','~'}:return None
    if re.fullmatch(r'-?[0-9]+',raw):return int(raw)
    return raw

def parse_yaml(raw):
    lines=[(len(line)-len(line.lstrip()),line.strip()) for line in raw.splitlines() if line.strip()];anchors={}
    def block(position,indent):
        is_sequence=lines[position][1].startswith('- ');result=[] if is_sequence else {}
        while position<len(lines) and lines[position][0]==indent:
            value=lines[position][1];position+=1
            if is_sequence:
                if not value.startswith('- '):raise ValueError('Reviewed sequence grammar changed')
                value=value[2:]
                if ':' in value and not value.startswith('{'):
                    key,payload=value.split(':',1);item={key:yaml_scalar(payload,anchors)}
                    if position<len(lines) and lines[position][0]>indent:
                        tail,position=block(position,lines[position][0]);item.update(tail)
                else:item=yaml_scalar(value,anchors)
                result.append(item)
            else:
                key,payload=value.split(':',1);payload=payload.strip()
                if payload in {'|','|-'}:
                    values=[]
                    while position<len(lines) and lines[position][0]>indent:values.append(lines[position][1]);position+=1
                    result[key]='\n'.join(values)+('' if payload=='|-' else '\n')
                elif payload:result[key]=yaml_scalar(payload,anchors)
                elif position<len(lines) and lines[position][0]>indent:result[key],position=block(position,lines[position][0])
                else:result[key]=None
        return result,position
    if not lines:return None,anchors
    value,end=block(0,lines[0][0])
    if end!=len(lines):raise ValueError('Unconsumed reviewed YAML input')
    return value,anchors

def yaml_documents(raw):
    chunks=[];current=[]
    for line in raw.splitlines():
        if line.strip() in {'---','...'}:
            if current:chunks.append(parse_yaml('\n'.join(current))[0]);current=[]
        else:current.append(line)
    if current:chunks.append(parse_yaml('\n'.join(current))[0])
    return chunks

def xml_parse(raw,preserve_cdata=True,preserve_comments=False):
    doc=minidom.parseString(raw)
    def visit(node):
        for child in list(node.childNodes):
            if child.nodeType==Node.COMMENT_NODE and not preserve_comments:node.removeChild(child)
            elif child.nodeType==Node.CDATA_SECTION_NODE and not preserve_cdata:node.replaceChild(doc.createTextNode(child.data),child)
            else:visit(child)
    visit(doc);return doc.documentElement

def node_kind(node):
    return {Node.ELEMENT_NODE:'element',Node.TEXT_NODE:'text',Node.CDATA_SECTION_NODE:'cdata',Node.COMMENT_NODE:'comment',Node.DOCUMENT_NODE:'document',Node.PROCESSING_INSTRUCTION_NODE:'pi'}[node.nodeType]

def text_content(node):
    if node.nodeType in {Node.TEXT_NODE,Node.CDATA_SECTION_NODE}:return node.data
    return ''.join(child.data for child in node.childNodes if child.nodeType in {Node.TEXT_NODE,Node.CDATA_SECTION_NODE})

def observations():
    rows={};value=urlsplit('https://example.com:8080/path?q=1#top')
    values=[quote('hello 世界!',safe='-._~'),unquote('hello%20%E4%B8%96%E7%95%8C%21'),quote_plus('a b+c'),unquote_plus('a+b%2Bc')]
    values.extend([value.scheme+':',value.hostname,str(value.port),value.path,'?'+value.query,'#'+value.fragment,value.netloc,value.scheme+'://'+value.netloc,urlunsplit(value),urlunsplit(urlsplit('http://user:pass@example.com/path'))])
    pairs=query('msg=hello+world&key=a%26b');first=lambda data,key:next(v for k,v in data if k==key)
    values.extend([first(pairs,'msg'),first(pairs,'key')]);other=query('?empty=&flag&msg=hello+world&&=skip');values.extend(first(other,key) for key in ['empty','flag','msg',''])
    values.append(urlencode(pairs))
    try:query('ok=1&bad=%GG')
    except PercentError as error:values.append(error.offset)
    values.extend(urljoin(base,relative) for base,relative in [('https://example.com/a/b/c','d.html'),('https://example.com/a/b/c?old=1#top','?new=2'),('https://example.com/a/b/c?old=1','#frag'),('https://example.com/a/b/c','//cdn.example.com/lib.js'),('https://example.com/a/b/c','../d/./e')])
    values.extend([join_url('/api/','/v1/','/users'),join_url('','/api/','','v1','/users')]);rows['url_core_direct']=values

    root=xml_parse('<?xml version="1.0"?><root xmlns="urn:x" id="7"><item>A&amp;B</item><![CDATA[<raw>]]></root>');kids=list(root.childNodes)
    values=[root.tagName,root.getAttribute('id'),root.namespaceURI,len(kids),text_content(kids[0]),node_kind(kids[1]),kids[1].data]
    plain=xml_parse('<root><![CDATA[plain]]></root>',False);values.extend([node_kind(plain.childNodes[0]),text_content(plain)])
    comments=xml_parse('<root><!-- keep --><leaf/></root>',preserve_comments=True);values.extend([len(comments.childNodes),node_kind(comments.childNodes[0])])
    valid=xml_parse('<ok/>');values.extend([valid.tagName,0])
    doc=minidom.Document();made=doc.createElement('entry');made.setAttribute('id','42');made.appendChild(doc.createTextNode('body'))
    values.extend([made.tagName,made.getAttribute('id'),text_content(made),made.tagName]);text=doc.createTextNode('<hello>&');values.extend([node_kind(text),text.data])
    values.extend([xml_parse('<msg>Hello&lt;World</msg>').toxml(),made.toxml()]);rows['xml_pure_module_direct']=values

    raw='app:\n  name: xray\n  ports: [80, 443]\n  limits: {max: 100, timeout: 30}\nitems:\n  - id: 1\n    status: done\n  - {id: 2, status: pending}\n';doc,anchors=parse_yaml(raw);app=doc['app'];ports=app['ports'];items=doc['items']
    values=[app['name'],ports[1],app['limits']['max'],len(items),items[0]['status'],items[1]['id'],'map' if type(doc) is dict else 'invalid','seq' if type(ports) is list else 'invalid',','.join(app)]
    aliases,_=parse_yaml('anchor: &val hello\nalias: *val');values.extend([aliases['anchor'],aliases['alias']])
    block,_=parse_yaml('text: |-\n  line one\n  line two');value=block['text'];values.extend(['has-line' if 'line one' in value else 'missing-line','has-newline' if value.endswith('\n') else 'no-newline'])
    docs=yaml_documents('---\na: 1\n...\nb: 2');values.extend([len(docs),docs[0]['a'],docs[1]['b']])
    strict,_=parse_yaml('name: Alice\nage: 30');values.append(strict['name']);report,report_anchors=parse_yaml('name: Alice\nage: 30');values.extend([0,len(report_anchors)])
    mapping={'name':'xray','version':1,'debug':False};wire='\n'.join(k+': '+json.dumps(v) for k,v in mapping.items());reparsed,_=parse_yaml(wire)
    values.extend([reparsed['name'],reparsed['version'],'debug-on' if reparsed['debug'] is True else 'debug-off']);rows['yaml_pure_module_direct']=values
    if set(rows)!=_NAMES:raise ValueError('Text format model lost a complete source')
    return rows

def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES):raise ValueError('Text formats lost reviewed identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib'}:raise ValueError('Text model requires delivered identities')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Text source/authority changed: '+name)
    values=observations();rows=[]
    for name,source in sorted(_PROGRAMS.items()):
        if source!='tests/diff/cases/semantics/stdlib/'+name+'.xr' or source not in _INPUTS:raise ValueError('Text program source changed')
        render=lambda v:str(v).lower() if type(v) is bool else str(v)
        rows.append({'id':'complete_text_formats_'+name,'source':source,'original_source':source,'required_target':None,'expected':{'exit_code':0,'stdout_utf8':''.join(render(v)+'\n' for v in values[name]),'stderr_utf8':''},
            'model':'Complete original URL/XML/YAML program; independent ordered query/byte offset, DOM node semantics and approved YAML grammar/value model. Not format conformance or physical resource qualification.',
            'observation_count':len(values[name])})
    return rows
