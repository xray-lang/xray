"""Independent byte grammar for the complete, reviewed HTTP text fixtures.

This model covers their literal inputs and public Xray policy, not general RFC
conformance, network transport, TLS, or an alternate production HTTP parser.
"""
from pathlib import Path
import hashlib,re
from urllib.parse import urljoin,urlsplit

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'stdlib/http/http.xr': '5302929606a5dd63aac569a57b6e5c36a6de5eab74ea6727928beaeddb7c4377',
 'stdlib/http2/http2.xr': '73bb5cb769596d68cdba0ce2f0a13391370527d39350834b16c0d58316ec368d',
 'stdlib/url/url.xr': 'ed9ff523e0d27db91045e6729d3922675f324f9f4bd95874ed801b0b732625a4',
 'tests/diff/cases/semantics/stdlib/http2_surface_direct.xr': 'c596a9ab813f92daf550a66a2abbaa7ff03abbe40dc8a6c559e8a24a7ac14337',
 'tests/diff/cases/semantics/stdlib/http_chunked_body_pure_direct.xr': '930940d138f6f249e32823b4836ae297bfb2a096104446e036344918d62d997d',
 'tests/diff/cases/semantics/stdlib/http_request_head_pure_direct.xr': '769bdd23f1ee2be8ef4ec4331f13bbc54faabd4e8c5789e94ebc0b78e9a20fd9',
 'tests/diff/cases/semantics/stdlib/http_request_message_pure_direct.xr': '03e1eded9726fc4ccfe476ade0a8a191c9988cc60a49203501b2955873d50048',
 'tests/diff/cases/semantics/stdlib/http_response_head_pure_direct.xr': '894a63916ae5a99fd55c56ab80b40fd039da11df205889c5a9a6b3b0aef655d6',
 'tests/diff/cases/semantics/stdlib/http_response_text_pure_direct.xr': '062d43d33695ff6da537fe42af78c2ad83c4ef4c367580e037edf4a3b4c0835b'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/016-15-standard-library.md',
 'stdlib/http/http.xr',
 'stdlib/http2/http2.xr',
 'stdlib/url/url.xr',
 'tests/diff/cases/semantics/stdlib/http2_surface_direct.xr',
 'tests/diff/cases/semantics/stdlib/http_chunked_body_pure_direct.xr',
 'tests/diff/cases/semantics/stdlib/http_request_head_pure_direct.xr',
 'tests/diff/cases/semantics/stdlib/http_request_message_pure_direct.xr',
 'tests/diff/cases/semantics/stdlib/http_response_head_pure_direct.xr',
 'tests/diff/cases/semantics/stdlib/http_response_text_pure_direct.xr')
_PROGRAMS = {'http2_surface_direct': 'tests/diff/cases/semantics/stdlib/http2_surface_direct.xr',
 'http_chunked_body_pure_direct': 'tests/diff/cases/semantics/stdlib/http_chunked_body_pure_direct.xr',
 'http_request_head_pure_direct': 'tests/diff/cases/semantics/stdlib/http_request_head_pure_direct.xr',
 'http_request_message_pure_direct': 'tests/diff/cases/semantics/stdlib/http_request_message_pure_direct.xr',
 'http_response_head_pure_direct': 'tests/diff/cases/semantics/stdlib/http_response_head_pure_direct.xr',
 'http_response_text_pure_direct': 'tests/diff/cases/semantics/stdlib/http_response_text_pure_direct.xr'}
_NAMES=frozenset({'http2_surface_direct','http_chunked_body_pure_direct','http_request_head_pure_direct',
 'http_request_message_pure_direct','http_response_head_pure_direct','http_response_text_pure_direct'})
TOKEN=re.compile(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+")

def all_headers(headers,name):return [v for k,v in headers if k.lower()==name.lower()]
def first_header(headers,name):
    values=all_headers(headers,name);return values[0] if values else None

def fields(lines,limit):
    result=[]
    for line in lines:
        if not line or line[0] in ' \t' or ':' not in line:raise ValueError('Invalid header line')
        name,value=line.split(':',1);value=value.strip()
        if not TOKEN.fullmatch(name) or any(ord(c)<32 and c!='\t' for c in value):raise ValueError('Invalid header')
        result.append((name,value))
        if limit>0 and len(result)>limit:raise ValueError('Header limit')
    return result

def parse_head(raw,response=False,max_headers=100):
    try:
        encoded=raw.encode();boundary=encoded.index(b'\r\n\r\n')+4
        lines=encoded[:boundary-4].decode('utf-8').split('\r\n');first=lines.pop(0)
        if response:
            match=re.fullmatch(r'(HTTP/1\.[01]) ([0-9]{3})(?: (.*))?',first)
            if not match or int(match[2])<100:return None
            version=match[1];initial={'version':version,'status':int(match[2]),'reason':(match[3] or '').strip()}
        else:
            match=re.fullmatch(r'([^ ]+) ([^ ]+) (HTTP/1\.[01])',first)
            if not match or any(ord(c)<33 for c in match[1]+match[2]):return None
            version=match[3];initial={'method':match[1].upper(),'requestTarget':match[2],'version':version}
        headers=fields(lines,max_headers);lengths=all_headers(headers,'content-length')
        if any(not re.fullmatch(r'[0-9]+',value) for value in lengths):return None
        lengths=[int(value) for value in lengths]
        if len(set(lengths))>1:return None
        length=lengths[0] if lengths else -1
        encodings=all_headers(headers,'transfer-encoding')
        if any(value.lower()!='chunked' for value in encodings):return None
        chunked=bool(encodings)
        if chunked and length>=0:return None
        connection={token.strip().lower() for value in all_headers(headers,'connection') for token in value.split(',')}
        keep='close' not in connection and (version=='HTTP/1.1' or 'keep-alive' in connection)
        return dict(initial,headers=headers,headerBytes=boundary,contentLength=length,chunked=chunked,keepAlive=keep)
    except (ValueError,UnicodeError):return None

def parse_chunks(raw,max_body=67108864,max_trailers=50):
    try:
        data=raw.encode();position=0;parts=[];total=0
        while True:
            end=data.index(b'\r\n',position);size_text=data[position:end].split(b';',1)[0].strip()
            if not re.fullmatch(b'[0-9a-fA-F]+',size_text):return None
            size=int(size_text,16);position=end+2
            if not size:
                lines=[]
                while True:
                    end=data.index(b'\r\n',position);line=data[position:end].decode('utf-8');position=end+2
                    if not line:return {'body':''.join(parts),'trailers':fields(lines,max_trailers),'consumedBytes':position}
                    lines.append(line)
            total+=size
            if max_body>0 and total>max_body:return None
            if position+size+2>len(data) or data[position+size:position+size+2]!=b'\r\n':return None
            parts.append(data[position:position+size].decode('utf-8'));position+=size+2
    except (ValueError,UnicodeError):return None

def parse_message(raw,response=False,max_body=67108864):
    head=parse_head(raw,response)
    if head is None:return None
    data=raw.encode();start=head['headerBytes'];trailers=[]
    try:
        if response and (100<=head['status']<200 or head['status'] in [204,304]):body='';end=start
        elif head['chunked']:
            parsed=parse_chunks(data[start:].decode('utf-8'),max_body)
            if parsed is None:return None
            body,trailers,end=parsed['body'],parsed['trailers'],start+parsed['consumedBytes']
        elif head['contentLength']>=0:
            length=head['contentLength'];end=start+length
            if end>len(data) or (max_body>0 and length>max_body):return None
            body=data[start:end].decode('utf-8')
        elif response:
            body=data[start:].decode('utf-8');end=len(data)
            if max_body>0 and end-start>max_body:return None
        else:body='';end=start
        return {'head':head,'body':body,'trailers':trailers,'consumedBytes':end}
    except UnicodeError:return None

def origin(url):
    value=urlsplit(url);return value.scheme.lower(),value.hostname.lower(),value.port or (443 if value.scheme.lower()=='https' else 80)

def redirect(status,current,location,method='GET'):
    if status not in [301,302,303,307,308]:return None
    target=urljoin(current,location);method=method.upper();drop=status==303 and method!='HEAD'
    return {'url':target,'method':'GET' if drop else method,'stripSensitiveHeaders':origin(current)!=origin(target),'dropBody':drop}

def invalid_head_inputs(response=False):
    prefix='HTTP/1.1 200 OK' if response else 'GET / HTTP/1.1'
    result=[prefix+'\r\n'+line+'\r\n\r\n' for line in ['Bad Name: x','Content-Length: 5\r\nContent-Length: 6','Content-Length: 5\r\nTransfer-Encoding: chunked','Transfer-Encoding: gzip, chunked','Transfer-Encoding: chunked, gzip']]
    return result

def invalid_message_inputs(response=False):
    prefix='HTTP/1.1 200 OK' if response else 'POST / HTTP/1.1'
    return [prefix+'\r\n'+tail for tail in ['Content-Length: 5\r\n\r\nabc','Transfer-Encoding: chunked\r\n\r\n4\r\nabc\r\n0\r\n\r\n','Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n','Transfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n','Transfer-Encoding: chunked, gzip\r\n\r\nhello','Transfer-Encoding: chunked\r\n\r\n1\r\né\r\n0\r\n\r\n']]

def observations():
    rows={'http2_surface_direct':['bool']}
    raw='4\r\nWiki\r\n5;ext=x\r\npedia\r\n0\r\nExpires: soon\r\nX-Trailer: one\r\nX-Trailer: two\r\n\r\nignored';parsed=parse_chunks(raw);empty=parse_chunks('0\r\n\r\n')
    values=[parsed['body'],parsed['consumedBytes'],raw.encode()[parsed['consumedBytes']:].decode(),first_header(parsed['trailers'],'expires'),'|'.join(all_headers(parsed['trailers'],'x-trailer')),first_header(parsed['trailers'],'X-Trailer'),parse_chunks(raw)['body'],empty['body']=='',empty['consumedBytes']]
    values.extend(parse_chunks(text) is None for text in ['3\r\nab\r\n0\r\n\r\n','z\r\nbad\r\n0\r\n\r\n','2\r\nab\n0\r\n\r\n','2\r\nab\r\n0\r\n folded: nope\r\n\r\n'])
    values.extend([parse_chunks('4\r\nWiki\r\n0\r\nA: 1\r\nB: 2\r\n\r\n',99,1) is None,parse_chunks('4\r\nWiki\r\n0\r\n\r\n',3) is None,parse_chunks('4\r\nWiki\r\n0\r\n\r\n')['body']=='Wiki']);rows['http_chunked_body_pure_direct']=values

    raw='post /submit?q=1 HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\nX-Trace: one\r\nX-Trace: two\r\n\r\n4\r\ndata\r\n0\r\n\r\n';head=parse_head(raw)
    values=[head[k] for k in ['method','requestTarget','version','contentLength','chunked','keepAlive']]
    values.extend([first_header(head['headers'],'host'),'|'.join(all_headers(head['headers'],'x-trace')),first_header(head['headers'],'X-Trace'),raw.encode()[head['headerBytes']:].decode(),parse_head('GET http://example.com/a HTTP/1.1\r\nHost: proxy\r\n\r\n')['requestTarget'],parse_head('GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n')['keepAlive']])
    bad=['BAD METHOD / HTTP/1.1\r\n\r\n','GET /bad path HTTP/1.1\r\n\r\n','GET / HTTP/2\r\n\r\n',*invalid_head_inputs()]
    values.extend(parse_head(text) is None for text in bad);values.extend([parse_head('GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\n\r\n',max_headers=1) is None,parse_head('HEAD / HTTP/1.1\r\n\r\n')['method']]);rows['http_request_head_pure_direct']=values

    for response,name in [(False,'http_request_message_pure_direct'),(True,'http_response_text_pure_direct')]:
        prefix='HTTP/1.1 200 OK' if response else 'POST /submit HTTP/1.1'
        raw=prefix+'\r\nContent-Length: 5\r\nX-Trace: one\r\n\r\nhelloextra';fixed=parse_message(raw,response)
        values=[fixed['head']['status']] if response else [fixed['head']['method'],fixed['head']['requestTarget']]
        values.extend([fixed['body'],fixed['consumedBytes'],first_header(fixed['head']['headers'],'x-trace'),len(fixed['trailers']),raw.encode()[fixed['consumedBytes']:].decode()])
        prefix='HTTP/1.1 200 OK' if response else 'POST /upload HTTP/1.1'
        chunked=parse_message(prefix+'\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\nChecksum: ok\r\nChecksum: newer\r\n\r\nleft',response)
        values.extend([chunked['body'],chunked['head']['chunked'],first_header(chunked['trailers'],'checksum'),'|'.join(all_headers(chunked['trailers'],'checksum')),chunked['consumedBytes']])
        if response:
            close=parse_message('HTTP/1.0 200 OK\r\n\r\nuntil-close',True);values.extend([close['body'],close['consumedBytes']])
        else:
            no_body=parse_message('GET /next HTTP/1.1\r\nHost: example.com\r\n\r\nnext');values.extend([no_body['body']=='',no_body['consumedBytes']])
        prefix='HTTP/1.1 200 OK' if response else 'POST /utf8 HTTP/1.1'
        unicode=parse_message(prefix+'\r\nContent-Length: 3\r\n\r\néxtrail',response);values.extend([unicode['body'],len(unicode['body'].encode()),unicode['consumedBytes']])
        unicode_chunk=parse_message(prefix+'\r\nTransfer-Encoding: chunked\r\n\r\n2\r\né\r\n0\r\n\r\n',response);values.extend([unicode_chunk['body'],len(unicode_chunk['body'].encode())])
        if response:
            no_body=parse_message('HTTP/1.1 204 No Content\r\n\r\nleft',True);values.extend([no_body['body']=='',no_body['consumedBytes']])
        bad=invalid_message_inputs(response);values.append(parse_message(bad[0],response) is None)
        values.append(parse_message(('HTTP/1.1 200 OK' if response else 'POST / HTTP/1.1')+'\r\nContent-Length: 5\r\n\r\nhello',response,3) is None)
        values.extend(parse_message(text,response) is None for text in bad[1:])
        values.append(parse_message('HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n',True)['body']=='' if response else parse_message('GET / HTTP/1.1\r\n\r\n')['body']=='')
        rows[name]=values

    raw='HTTP/1.1 302 Found\r\nLocation: /next?q=1\r\nContent-Length: 5\r\nConnection: close\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\nhello';head=parse_head(raw,True)
    values=[head[k] for k in ['version','status','reason','contentLength','chunked','keepAlive']]
    values.extend([first_header(head['headers'],'location'),'|'.join(all_headers(head['headers'],'set-cookie')),first_header(head['headers'],'Content-Length')])
    chunked=parse_head('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n',True);values.extend(chunked[k] for k in ['status','contentLength','chunked','keepAlive'])
    values.append(parse_head('HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n',True)['keepAlive'])
    bad=invalid_head_inputs(True);bad.insert(1,'HTTP/1.1 200 OK\r\nContent-Length: five\r\n\r\n');bad.insert(2,'HTTP/1.1 200 OK\r\n folded: nope\r\n\r\n')
    values.extend(parse_head(text,True) is None for text in bad);values.append(parse_head('HTTP/1.1 200 OK\r\nContent-Length: 5\r\n',True,max_headers=10) is None)
    values.extend([308 in [301,302,303,307,308],304 not in [301,302,303,307,308],urljoin('https://a.test/dir/page?q=1','../next')])
    for args in [(303,'https://a.test/form','/done','POST'),(302,'http://a.test:80/start','https://b.test/next','POST')]:
        decision=redirect(*args);values.extend(decision[k] for k in ['url','method','stripSensitiveHeaders','dropBody'])
    values.extend([redirect(302,'http://a.test/start','http://a.test:80/next')['stripSensitiveHeaders'],redirect(200,'http://a.test','/next') is None]);rows['http_response_head_pure_direct']=values
    if set(rows)!=_NAMES:raise ValueError('HTTP model lost complete source identity')
    return rows

def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES):raise ValueError('HTTP model lost reviewed identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib'}:raise ValueError('HTTP input is not delivered source')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('HTTP source/authority changed: '+name)
    values=observations();rows=[]
    for name,source in sorted(_PROGRAMS.items()):
        if source!='tests/diff/cases/semantics/stdlib/'+name+'.xr' or source not in _INPUTS:raise ValueError('HTTP source identity changed')
        rows.append({'id':'complete_http_framing_'+name,'source':source,'original_source':source,'required_target':None,'expected':{'exit_code':0,'stdout_utf8':''.join((str(v).lower() if type(v) is bool else str(v))+'\n' for v in values[name]),'stderr_utf8':''},
            'model':'Full original HTTP text fixture; independent byte-framing grammar, UTF-8 boundaries, ordered case-insensitive headers and explicit redirect policy. HTTP2 observes only declared bool return type; no transport/availability qualification.',
            'observation_count':len(values[name])})
    return rows
