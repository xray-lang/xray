"""Derive every output of the complete original WebSocket helper consumer.

The model covers finite helper calls, not sockets, TLS, connection state,
compression, random masking, scheduling, resources or provider qualification.
RFC6455 supplies wire and accept-key facts; local declarations supply helper
selection/limit policy. No product output or Xray/C execution is an oracle.
"""
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit
import base64,hashlib,re

from complete_http_framing_model import parse_head,first_header
from independent_observation_format import render

ROOT=Path(__file__).resolve().parents[4]
ORIGINAL='tests/diff/cases/semantics/stdlib/ws_pure_protocol_direct.xr'
PORT='tests/unit/xir/legacy_consumers/fixtures/websocket_explicit_utf8/root.xr'
IDENTITY='complete_websocket_protocol_ws_pure_protocol_direct'
RFC6455='https://www.rfc-editor.org/rfc/rfc6455.html'
GUID='258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
KEY='dGhlIHNhbXBsZSBub25jZQ=='
_AUTHORITIES=(
    'spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
    'spec/source/sections/006-5-declarations.md','spec/source/sections/009-8-error-handling.md',
    'spec/source/sections/014-13-built-in-functions.md','spec/source/sections/015-14-built-in-type-methods.md',
    'spec/source/sections/016-15-standard-library.md','stdlib/types/string.xr','stdlib/types/array.xr',
    'stdlib/ws/ws.xr','stdlib/crypto/crypto.xr','stdlib/base64/base64.xr','stdlib/text/text.xr',
    'tests/unit/xir/legacy_consumers/complete_http_framing_model.py',
    'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'stdlib/base64/base64.xr': '0d00c8256b55901ce0423da5a72b842df5001c134670d49663e0e071067fa6f1',
 'stdlib/crypto/crypto.xr': 'd622f2ba35963bf7ae52a5db06349adfdbdcdf5a43462a4fc497abdfb60df05d',
 'stdlib/text/text.xr': '55d2aaa9e524bc1924f6b426b763f9d45e52f926f204ded84c7e8a64289b9aef',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'stdlib/ws/ws.xr': 'fd82ecd9793b8b8312aea5ab480367b1689517fbd1520c2653dfc96b52fcafb0',
 'tests/diff/cases/semantics/stdlib/ws_pure_protocol_direct.xr': '84a83b5ddb62ff454e6f4c13ec5ae29c6aeb5d06372b326a68ea40ed7d040759',
 'tests/unit/xir/legacy_consumers/complete_http_framing_model.py': 'da3d84d4bcacc7c6435156428af6edf4a6e68e62964cdaaabe94cd72ef73ce05',
 'tests/unit/xir/legacy_consumers/fixtures/websocket_explicit_utf8/root.xr': '210744193215f46eb813d5115823e735c4c56ef9cc536ea1928de24dfd6edec1',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}


def accept_key(key):return base64.b64encode(hashlib.sha1((key+GUID).encode()).digest()).decode()


def valid_key(key):
    try:return len(base64.b64decode(key,validate=True))==16
    except (ValueError,UnicodeError):return False


def element_tokens(value):return {item.split(';',1)[0].strip().lower() for item in value.split(',')}


def header(head,name):return first_header(head['headers'],name)


def upgrade_request_text():
    return ('GET /chat?room=1 HTTP/1.1\r\nHost: example.com\r\nOrigin: https://app.example\r\n'
        'Upgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Key: '+KEY+
        '\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: superchat, chat\r\n'
        'Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover\r\n\r\n')


def parse_upgrade_request(raw,protocols=None,allow_deflate=True,origins=None):
    head=parse_head(raw)
    if not head or not raw.startswith('GET ') or head['version']!='HTTP/1.1' or not head['requestTarget'].startswith('/'):return None
    if (header(head,'upgrade') or '').lower()!='websocket' or 'upgrade' not in element_tokens(header(head,'connection') or ''):return None
    key=header(head,'sec-websocket-key')
    if not key or not valid_key(key) or (header(head,'sec-websocket-version') or '').strip()!='13':return None
    origin=header(head,'origin')
    if origins and '*' not in origins and origin not in origins:return None
    offered={item.strip().lower() for item in (header(head,'sec-websocket-protocol') or '').split(',')}
    protocol=next((item for item in (protocols or []) if item.lower() in offered),None)
    return {'requestTarget':head['requestTarget'],'secKey':key,'protocol':protocol,
            'deflate':allow_deflate and 'permessage-deflate' in element_tokens(header(head,'sec-websocket-extensions') or ''),'origin':origin}


@dataclass(frozen=True)
class Url:
    secure: bool
    host: str
    port: int
    path: str


def parse_url(value):
    try:
        if '#' in value:return None
        parsed=urlsplit(value)
        if parsed.scheme not in {'ws','wss'} or not parsed.hostname or parsed.username is not None:return None
        if parsed.netloc.endswith(':'):return None
        secure=parsed.scheme=='wss';port=parsed.port if parsed.port is not None else (443 if secure else 80)
        if not 1<=port<=65535:return None
        host=parsed.netloc[1:parsed.netloc.index(']')] if parsed.netloc.startswith('[') else parsed.netloc.split(':',1)[0]
        return Url(secure,host,port,(parsed.path or '/')+('?' + parsed.query if parsed.query else ''))
    except ValueError:return None


def client_handshake(url,key,protocols=None,deflate=True):
    if not valid_key(key) or any(c in key+url.host+url.path for c in '\r\n') or not url.path.startswith('/'):return None
    if any(not item or '\r' in item or '\n' in item for item in protocols or []):return None
    host='['+url.host+']' if ':' in url.host else url.host
    if url.port!=(443 if url.secure else 80):host+=':'+str(url.port)
    lines=['GET '+url.path+' HTTP/1.1','Host: '+host,'Upgrade: websocket','Connection: Upgrade',
        'Sec-WebSocket-Key: '+key,'Sec-WebSocket-Version: 13']
    if deflate:lines.append('Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover; server_no_context_takeover')
    if protocols:lines.append('Sec-WebSocket-Protocol: '+', '.join(protocols))
    return '\r\n'.join(lines)+'\r\n\r\n'


def server_handshake(key,protocol=None,deflate=False):
    lines=['HTTP/1.1 101 Switching Protocols','Upgrade: websocket','Connection: Upgrade','Sec-WebSocket-Accept: '+accept_key(key)]
    if protocol:lines.append('Sec-WebSocket-Protocol: '+protocol)
    if deflate:lines.append('Sec-WebSocket-Extensions: permessage-deflate')
    return '\r\n'.join(lines)+'\r\n\r\n'


def upgrade_response_text():
    return ('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n'
        'Sec-WebSocket-Accept: '+accept_key(KEY)+'\r\nSec-WebSocket-Protocol: chat\r\n'
        'Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover\r\n\r\n')


def parse_upgrade_response(raw,key,protocols=None,allow_deflate=True):
    head=parse_head(raw,response=True)
    if not head or head['version']!='HTTP/1.1' or head['status']!=101:return None
    if (header(head,'upgrade') or '').lower()!='websocket' or 'upgrade' not in element_tokens(header(head,'connection') or ''):return None
    if header(head,'sec-websocket-accept')!=accept_key(key):return None
    protocol=header(head,'sec-websocket-protocol')
    if protocol is not None and (not protocol or protocols is not None and protocol not in protocols):return None
    deflate='permessage-deflate' in element_tokens(header(head,'sec-websocket-extensions') or '')
    if deflate and not allow_deflate:return None
    return {'statusCode':head['status'],'protocol':protocol,'deflate':deflate}


def valid_close_code(code):return code in {1000,1001,1002,1003,1007,1008,1009,1010,1011} or 3000<=code<=4999


def parse_close(payload):
    if len(payload)>125 or len(payload)==1:return None
    if not payload:return (1005,'')
    code=int.from_bytes(payload[:2],'big')
    if not valid_close_code(code):return None
    try:return (code,payload[2:].decode('utf-8'))
    except UnicodeError:return None


def mask_payload(payload,key):
    if len(key)<4:return bytes(len(payload))
    return bytes(value^key[index%4] for index,value in enumerate(payload))


def frame(payload,opcode=1,mask_key=None):
    length=len(payload);masked=mask_key is not None
    if masked and len(mask_key)!=4:raise ValueError('Explicit mask must be4 bytes;no entropy is invented')
    tag=length if length<126 else (126 if length<=65535 else 127)
    extra=b'' if length<126 else length.to_bytes(2 if length<=65535 else 8,'big')
    return bytes([128|(opcode&15),(128 if masked else 0)|tag])+extra+(mask_key or b'')+(mask_payload(payload,mask_key) if masked else payload)


@dataclass(frozen=True)
class Frame:
    fin: bool
    rsv1: bool
    opcode: int
    masked: bool
    payload: bytes
    consumed: int


def parse_frame(raw,expect_masked=False,enforce_mask=False,allow_rsv1=False,max_payload=67108864):
    if len(raw)<2:return None
    first,second=raw[:2];fin=bool(first&128);rsv1=bool(first&64);opcode=first&15
    if first&48 or rsv1 and not allow_rsv1 or opcode not in {0,1,2,8,9,10}:return None
    control=bool(opcode&8);masked=bool(second&128);length=second&127;offset=2
    if control and (not fin or length>125) or enforce_mask and masked!=expect_masked:return None
    if length in (126,127):
        width=2 if length==126 else 8
        if len(raw)<offset+width or width==8 and raw[offset]&128:return None
        length=int.from_bytes(raw[offset:offset+width],'big');offset+=width
        if length<(126 if width==2 else 65536):return None
    if max_payload>0 and length>max_payload:return None
    key=None
    if masked:
        if len(raw)<offset+4:return None
        key=raw[offset:offset+4];offset+=4
    if len(raw)<offset+length:return None
    payload=raw[offset:offset+length]
    return Frame(fin,rsv1,opcode,masked,mask_payload(payload,key) if masked else payload,offset+length)


def observations():
    values=[accept_key(KEY),'Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n' in server_handshake(KEY)]
    request=upgrade_request_text();parsed=parse_upgrade_request(request,['chat'],True,['https://app.example'])
    values.extend([parsed['requestTarget'],parsed['secKey']==KEY,parsed['protocol'],parsed['deflate'],parsed['origin']])
    values.append(parse_upgrade_request(request,['missing','superchat'],True,['*'])['protocol'])
    values.append(parse_upgrade_request(request,['chat'],False,['*'])['deflate'])
    for text in [request.replace('GET ','POST '),request.replace(KEY,'bad'),request.replace('Sec-WebSocket-Version: 13','Sec-WebSocket-Version: 12')]:
        values.append(parse_upgrade_request(text,['chat'],True,['*']) is None)
    values.append(parse_upgrade_request(request,['chat'],True,['https://other.example']) is None)
    url=parse_url('wss://example.com/chat?room=1');values.extend([url.secure,url.host,url.port,url.path])
    ipv6=parse_url('ws://[::1]:8080');values.extend([ipv6.secure,ipv6.host,ipv6.port,ipv6.path])
    values.append(parse_url('ws://example.com?token=a').path)
    handshake=client_handshake(url,KEY,['chat'],True)
    values.extend(text in handshake for text in ['GET /chat?room=1 HTTP/1.1\r\n','Host: example.com\r\n','Sec-WebSocket-Protocol: chat\r\n','permessage-deflate'])
    handshake=client_handshake(ipv6,KEY,None,False);values.extend(['Host: [::1]:8080\r\n' in handshake,'permessage-deflate' in handshake])
    values.extend([client_handshake(url,'bad',['chat'],True) is None,client_handshake(url,KEY,['chat\nbad'],True) is None])
    values.extend(parse_url(text) is None for text in ['http://example.com','ws://','ws://example.com:abc','ws://example.com:65536','ws://[::1','ws://example.com/#fragment'])
    response=upgrade_response_text();parsed=parse_upgrade_response(response,KEY,['chat'],True)
    values.extend([parsed['statusCode'],parsed['protocol'],parsed['deflate']])
    for text,protocols,allow in [(response.replace('101','200'),['chat'],True),(response.replace(accept_key(KEY),'bad'),['chat'],True),
            (response.replace('Upgrade: websocket\r\n',''),['chat'],True),(response,['other'],True),(response,['chat'],False)]:
        values.append(parse_upgrade_response(text,KEY,protocols,allow) is None)
    values.extend(parse_close(bytes([3,232,79,75])));empty=parse_close(b'');values.extend([empty[0],len(empty[1])])
    values.extend(valid_close_code(code) for code in [1000,1005,3000,5000])
    values.extend(parse_close(data) is None for data in [bytes([3]),bytes([3,238]),bytes([3,232,192,175]),bytes([3,232,226,130]),bytes([3,232])+bytes(124)])
    values.extend(mask_payload(bytes([1,2,3,4,5]),bytes([1,1,1,1])))
    small=frame(b'Hi');values.extend([len(small),*small])
    extended=frame(bytes(126),2);values.extend(extended[:4])
    extended300=frame(bytes(299)+b'Z',2);parsed=parse_frame(extended300)
    values.extend([extended300[2],extended300[3],len(parsed.payload),parsed.payload[299]])
    parsed=parse_frame(small);values.extend([parsed.fin,parsed.rsv1,parsed.opcode,parsed.masked,parsed.payload.decode('utf-8'),parsed.consumed])
    masked=bytes([129,130,1,2,3,4,73,107]);parsed=parse_frame(masked,True);values.extend([parsed.masked,parsed.payload.decode('utf-8')])
    parsed=parse_frame(bytes([129,1,65,66]));values.extend([parsed.payload.decode('utf-8'),parsed.consumed])
    values.append(parse_frame(bytes([193,0]),allow_rsv1=True).rsv1);values.append(parse_frame(bytes([193,0])) is None)
    invalid=[bytes([131,0]),bytes([9,0]),bytes([137,126,0,126]),bytes([129,126,0,1,65]),
             bytes([129,127,128,0,0,0,0,0,0,1,65]),bytes([129,2,65])]
    values.extend(parse_frame(data) is None for data in invalid);values.append(parse_frame(masked,False,True) is None)
    if len(values)!=94:raise ValueError('Complete WebSocket model lost original observations')
    return values


def port_text(original):
    if len(re.findall(r'^print\(',original,re.M))!=94 or len(re.findall(r'\b\w+\.payload\.toString\(\)',original))!=3:
        raise ValueError('Complete original WebSocket consumer lost its exact display sites')
    for name in ['parsed','parsedMasked','parsedTail']:
        before=name+'.payload.toString()'
        if original.count(before)!=1:raise ValueError('WebSocket text payload display changed')
        original=original.replace(before,'string.fromUtf8('+name+'.payload[:])')
    return original


def expected_rows():
    if (set(_INPUTS)!=set(_AUTHORITIES)|{ORIGINAL,PORT} or ORIGINAL!='tests/diff/cases/semantics/stdlib/ws_pure_protocol_direct.xr' or
            PORT!='tests/unit/xir/legacy_consumers/fixtures/websocket_explicit_utf8/root.xr'):
        raise ValueError('WebSocket model lost exact source,port or authority identities')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('WebSocket complete source or normative input changed: '+path)
    original=(ROOT/ORIGINAL).read_bytes().replace(b'\r\n',b'\n').decode('utf-8')
    if (ROOT/PORT).read_bytes().replace(b'\r\n',b'\n')!=port_text(original).encode('utf-8'):
        raise ValueError('WebSocket full-source port changed beyond three explicit UTF8 decodes')
    values=observations()
    return [{'id':IDENTITY,'source':PORT,'original_source':ORIGINAL,'required_target':None,
             'expected':{'exit_code':0,'stdout_utf8':''.join(render({'expected':x})+'\n' for x in values),'stderr_utf8':''},
             'observation_count':len(values),
             'model':'Complete original94-observation WebSocket helper body with only3 explicit UTF8 decode ports;independent accept digest,HTTP negotiation,URI,mask/length/frame/control/close byte models. No socket/TLS/compression/entropy/connection-state/resource/lifetime/provider qualification.'}]
