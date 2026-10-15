"""Independent state/serialization models for four whole HTTP policy fixtures."""
from pathlib import Path
import datetime,hashlib,json,re
from email.utils import parsedate_to_datetime
from urllib.parse import quote,unquote_plus

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'stdlib/http/http.xr': '5302929606a5dd63aac569a57b6e5c36a6de5eab74ea6727928beaeddb7c4377',
 'stdlib/text/text.xr': '55d2aaa9e524bc1924f6b426b763f9d45e52f926f204ded84c7e8a64289b9aef',
 'stdlib/url/url.xr': 'ed9ff523e0d27db91045e6729d3922675f324f9f4bd95874ed801b0b732625a4',
 'tests/diff/cases/semantics/stdlib/http_cookie_pure_direct.xr': '937c723f50148bfcfd9f95dff920707468dc0f649b2806eec005a402de62cd4d',
 'tests/diff/cases/semantics/stdlib/http_pure_helpers_direct.xr': '02a4d6f8bce23707d13a18ada2877c03e72888aad91c23b05fa0ddca8a772293',
 'tests/diff/cases/semantics/stdlib/http_request_text_pure_direct.xr': '174c52527b158e2f23e393c6acab0a3525cc6e3dd094ef3415559f5f8c3ac4a5',
 'tests/diff/cases/semantics/stdlib/http_router_pure_direct.xr': '0e07b3dc87f6d8510b7b45e507a1837bd99d3a6e9553503a98f36cb3aa8f8929'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/016-15-standard-library.md',
 'stdlib/http/http.xr',
 'stdlib/text/text.xr',
 'stdlib/url/url.xr',
 'tests/diff/cases/semantics/stdlib/http_cookie_pure_direct.xr',
 'tests/diff/cases/semantics/stdlib/http_pure_helpers_direct.xr',
 'tests/diff/cases/semantics/stdlib/http_request_text_pure_direct.xr',
 'tests/diff/cases/semantics/stdlib/http_router_pure_direct.xr')
_PROGRAMS = {'http_cookie_pure_direct': 'tests/diff/cases/semantics/stdlib/http_cookie_pure_direct.xr',
 'http_pure_helpers_direct': 'tests/diff/cases/semantics/stdlib/http_pure_helpers_direct.xr',
 'http_request_text_pure_direct': 'tests/diff/cases/semantics/stdlib/http_request_text_pure_direct.xr',
 'http_router_pure_direct': 'tests/diff/cases/semantics/stdlib/http_router_pure_direct.xr'}
_NAMES=frozenset({'http_cookie_pure_direct','http_pure_helpers_direct','http_request_text_pure_direct','http_router_pure_direct'})

class HeaderError(Exception):pass

def checked_headers(pairs):
    result=[]
    for name,value in pairs:
        if not re.fullmatch(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+",name) or '\r' in value or '\n' in value:raise HeaderError('Invalid header')
        result.append((name,value))
    return result

def injection_rejected():
    try:checked_headers([('X-Bad','a\r\nInjected: y')])
    except HeaderError:return True
    return False

def request_text(method,path,headers=(),body='',host='',strip_sensitive=False):
    method=method.strip().upper()
    if not method or any(ord(c)<33 for c in method) or not path.startswith('/') or any(ord(c)<33 for c in path):return None
    pairs=[(k,v) for k,v in checked_headers(headers) if not strip_sensitive or k.lower() not in {'authorization','proxy-authorization'}]
    present={k.lower() for k,v in pairs}
    if 'host' not in present:
        if not host or any(ord(c)<33 for c in host):return None
        pairs.append(('Host',host))
    for name,value in [('User-Agent','xray-http/1.0'),('Accept','*/*'),('Connection','keep-alive'),('Accept-Encoding','gzip, deflate')]:
        if name.lower() not in present:pairs.append((name,value))
    if body and 'content-length' not in present:pairs.append(('Content-Length',str(len(body.encode()))))
    return method+' '+path+' HTTP/1.1\r\n'+''.join(k+': '+v+'\r\n' for k,v in pairs)+'\r\n'+body

def response_text(status,body='',content_type='text/plain; charset=utf-8',headers=(),keep_alive=True):
    content_type=content_type or 'text/plain; charset=utf-8'
    if '\r' in content_type or '\n' in content_type:return None
    pairs=checked_headers(headers)
    if any(k.lower() in {'content-length','transfer-encoding'} for k,v in pairs):return None
    reason={200:'OK',201:'Created',204:'No Content',404:'Not Found'}[status]
    fields=[('Content-Type',content_type),('Content-Length',str(len(body.encode()))),*pairs,('Connection','keep-alive' if keep_alive else 'close')]
    return 'HTTP/1.1 '+str(status)+' '+reason+'\r\n'+''.join(k+': '+v+'\r\n' for k,v in fields)+'\r\n'+body

def domain_matches(cookie_domain,request_domain):
    a,b=cookie_domain.lower(),request_domain.lower();return b==a or b.endswith('.'+a)

def cookie(header,domain,path,now):
    parts=[part.strip() for part in header.split(';')]
    if '=' not in parts[0]:return None
    name,value=parts.pop(0).split('=',1)
    if not name:return None
    default_path=path.rsplit('/',1)[0] if path.startswith('/') else '/'
    result={'name':name,'value':value,'domain':domain,'path':default_path or '/','expires':0,'secure':False,'httpOnly':False,'sameSite':'','hostOnly':True}
    for part in parts:
        key,separator,value=part.partition('=');key=key.strip().lower();value=value.strip()
        if key=='domain' and value:
            value=value.lstrip('.')
            if not domain_matches(value,domain):return None
            result.update(domain=value,hostOnly=False)
        elif key=='path' and value:result['path']=value
        elif key=='expires' and value:result['expires']=int(parsedate_to_datetime(value).timestamp())
        elif key=='max-age' and re.fullmatch(r'-?[0-9]+',value):result['expires']=now+int(value)
        elif key=='secure':result['secure']=True
        elif key=='httponly':result['httpOnly']=True
        elif key=='samesite' and value.lower() in {'strict','lax','none'}:result['sameSite']=value.title()
    if result['sameSite']=='None' and not result['secure']:return None
    return result

class Jar:
    def __init__(self):self.values=[]
    def add(self,value,now):
        key=lambda c:(c['name'],c['domain'].lower(),c['path'])
        self.values=[c for c in self.values if key(c)!=key(value) and not (0<c['expires']<=now)]
        if not (0<value['expires']<=now):self.values.append(value.copy())
    def header(self,domain,path,secure,now):
        result=[]
        for c in self.values:
            if 0<c['expires']<=now or (c['secure'] and not secure):continue
            if c['hostOnly'] and c['domain'].lower()!=domain.lower():continue
            if not c['hostOnly'] and not domain_matches(c['domain'],domain):continue
            if not (path==c['path'] or (path.startswith(c['path']) and (c['path'].endswith('/') or path[len(c['path']):].startswith('/')))):continue
            result.append(c['name']+'='+c['value'])
        return '; '.join(result) if result else None

class Router:
    def __init__(self):self.routes={}
    @staticmethod
    def segments(path):return [] if path=='/' else path.lstrip('/').split('/')
    def add(self,method,path,value):
        method=method.strip().lower()
        if not method or not path.startswith('/') or '?' in path:return False
        segments=self.segments(path)
        if any((part.startswith('*') or part.startswith('{*')) and i!=len(segments)-1 for i,part in enumerate(segments)):return False
        self.routes[method,path]=value;return True
    def find(self,method,path):
        method=method.strip().lower();actual=self.segments(path.split('?',1)[0]);dynamic=None
        for (declared,pattern),value in self.routes.items():
            if declared!=method:continue
            pieces=self.segments(pattern);params={};position=0;valid=True
            for piece in pieces:
                wildcard=piece[1:] if piece.startswith('*') else piece[2:-1] if piece.startswith('{*') and piece.endswith('}') else None
                if wildcard is not None:params[wildcard]='/'.join(actual[position:]);position=len(actual);break
                if position>=len(actual):valid=False;break
                if piece.startswith(':'):
                    if not actual[position]:valid=False;break
                    params[piece[1:]]=actual[position]
                elif piece!=actual[position]:valid=False;break
                position+=1
            if valid and position==len(actual):
                found=(value,params)
                if not params:return found
                if dynamic is None:dynamic=found
        return dynamic

def observations():
    rows={};now=1000;c=cookie('session=abc123','example.com','/account/login',now)
    values=[c[k] for k in ['name','value','domain','path']]+[c['sameSite']=='',c['hostOnly'],c['name']+'='+c['value']]
    token=cookie('token=xyz; Path=/api; Secure; HttpOnly; SameSite=None','example.com','/',now)
    values.extend(token[k] for k in ['secure','httpOnly','sameSite','hostOnly'])
    values.extend([cookie('pref=x; SameSite=None','example.com','/',now) is None,cookie('hack=1; Domain=evil.com','example.com','/',now) is None,cookie('old=ok; Expires=Sun, 06 Nov 1994 08:49:37 GMT','example.com','/',now)['expires']==784111777])
    jar=Jar();jar.add(c,now);jar.add(token,now);values.extend([len(jar.values),jar.header('example.com','/account/profile',False,now),jar.header('www.example.com','/api/v1',True,now)])
    domain=cookie('domain=d; Domain=example.com; Path=/api; Secure','www.example.com','/api/login',now);values.append(domain['hostOnly']);jar.add(domain,now)
    values.extend([jar.header('www.example.com','/api/v1',True,now),jar.header('www.example.com','/api/v1',False,now)])
    jar.add(cookie('session=def; Domain=example.com; Path=/account','example.com','/account/login',now),now);values.extend([len(jar.values),jar.header('example.com','/account/profile',False,now)])
    for raw in ['gone=1; Max-Age=-1','remove=before; Path=/','remove=after; Path=/; Max-Age=0']:
        jar.add(cookie(raw,'example.com','/',now),now);values.append(len(jar.values))
    jar.values=[c for c in jar.values if c['domain'].lower()!='example.com'];values.append(len(jar.values));rows['http_cookie_pure_direct']=values

    values=[quote('a b+张',safe='-._~'),quote(":/?#[]@!$&'()*+,;=",safe='-._~'),unquote_plus('a+b%2Bc%ZZ%'),unquote_plus('%E5%BC%A0%E4%B8%89')]
    text=response_text(200,'hello');values.extend([text.startswith('HTTP/1.1 200 OK\r\n'),'Content-Type: text/plain; charset=utf-8\r\n' in text,'Content-Length: 5\r\n' in text,text.endswith('\r\n\r\nhello')])
    unicode=response_text(200,'é');values.extend(['Content-Length: 2\r\n' in unicode,unicode.endswith('\r\n\r\né')])
    body=json.dumps({'error':'no'},separators=(',',':'));response=response_text(404,body,'application/json; charset=utf-8')
    values.extend([response.startswith('HTTP/1.1 404 Not Found\r\n'),'Content-Type: application/json; charset=utf-8\r\n' in response,'Content-Length: 14\r\n' in response,response.endswith('{"error":"no"}')])
    empty=response_text(204);values.extend([empty.startswith('HTTP/1.1 204 No Content\r\n'),'Content-Length: 0\r\n' in empty,empty.endswith('\r\n\r\n')])
    custom=response_text(201,'created','text/custom',[('X-Trace','abc')],False);values.extend([custom.startswith('HTTP/1.1 201 Created\r\n'),'Content-Type: text/custom\r\n' in custom,'X-Trace: abc\r\n' in custom,'Connection: close\r\n' in custom,custom.endswith('\r\n\r\ncreated')])
    values.extend([response_text(200,'bad','text/plain',[('Content-Length','999')]) is None,response_text(200,'bad','text/plain',[('Transfer-Encoding','chunked')]) is None,injection_rejected(),response_text(200,'bad','text\r\nbad') is None]);rows['http_pure_helpers_direct']=values

    values=[]
    for label,result in [('basic',request_text('get','/api?q=1',[('X-Test','ok')],host='example.com')),('body',request_text('post','/submit',[('Content-Type','text/plain')],'hello','example.com')),('custom-host',request_text('GET','/',[('Host','override.test')]))]:values.extend([label,'null' if result is None else result.replace('\r\n','|')])
    custom=request_text('GET','/',[('Accept-Encoding','identity')],host='example.com');values.extend(['Accept-Encoding: identity' in custom,'Accept-Encoding: gzip, deflate' in custom])
    stripped=request_text('GET','/private',[('Authorization','secret'),('Proxy-Authorization','proxy-secret'),('Accept','application/json')],host='api.test',strip_sensitive=True)
    values.extend(['Authorization' in stripped,'Proxy-Authorization' in stripped,'Accept: application/json' in stripped,request_text('GET','/bad path',host='example.com') is None,injection_rejected(),request_text('GET','/',host='bad host') is None,request_text('GET','/',host='example.com') is not None]);rows['http_request_text_pure_direct']=values

    router=Router();values=[router.add(*args) for args in [('GET','/users/:id','user'),('GET','/users/me','me'),('POST','/users/:id','post-user'),('GET','/files/{*path}','file'),('GET','/','root')]]
    user=router.find('get','/users/42?debug=1');values.extend([user[0],user[1]['id']]);exact=router.find('GET','/users/me');values.extend([exact[0],exact[1].get('id') is None])
    post=router.find('POST','/users/42');values.extend([post[0],post[1]['id']]);file=router.find('GET','/files/a/b/c.txt');values.extend([file[0],file[1]['path'],router.find('GET','/')[0]])
    values.extend([router.find('DELETE','/users/42') is None,router.add('','/bad','x'),router.add('GET','bad','x'),router.add('GET','/bad/*tail/more','x'),router.add('GET','/users/:id','replacement'),router.find('GET','/users/7')[0]])
    other=Router();values.append(other.add('GET','/x/*tail',1));tail=other.find('GET','/x/a/b');values.extend([tail[0]+1,tail[1]['tail']]);rows['http_router_pure_direct']=values
    if set(rows)!=_NAMES:raise ValueError('HTTP policy lost a complete program')
    return rows

def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES):raise ValueError('HTTP policy lost source identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib'}:raise ValueError('HTTP policy requires delivered inputs')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('HTTP policy source/authority changed: '+name)
    values=observations();rows=[]
    for name,source in sorted(_PROGRAMS.items()):
        if source!='tests/diff/cases/semantics/stdlib/'+name+'.xr' or source not in _INPUTS:raise ValueError('HTTP policy original source changed')
        rendered=lambda value:'null' if value is None else str(value).lower() if type(value) is bool else str(value)
        rows.append({'id':'complete_http_policy_'+name,'source':source,'original_source':source,'required_target':None,'expected':{'exit_code':0,'stdout_utf8':''.join(rendered(v)+'\n' for v in values[name]),'stderr_utf8':''},
            'model':'Complete original HTTP policy body; independent cookie state, ordered full wire serialization, error channel and static/dynamic route models. No network/physical resource qualification.',
            'observation_count':len(values[name])})
    return rows
