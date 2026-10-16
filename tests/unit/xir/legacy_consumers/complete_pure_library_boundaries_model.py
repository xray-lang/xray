"""Derive complete crypto, typed TOML and binary-stdin normal observations.

AES uses finite-field arithmetic and generated substitution tables, independently
of Xray's kernels. The only selected encryption observations are length,
round-trip and wrong-key authentication. No entropy bytes or running time are
predicted. Standard-library parsers/digests do not execute product code.
"""
from dataclasses import dataclass
from pathlib import Path
import hashlib
import hmac
import json
import tomllib

from independent_observation_format import render

ROOT=Path(__file__).resolve().parents[4]
BASE='tests/diff/cases/semantics/stdlib/'
_NAMES=frozenset({'crypto_timing_safe_equal_direct','toml_pure_module_direct','io_read_stdin_shared_core'})
_PROGRAMS={name:BASE+name+'.xr' for name in _NAMES}
_STDIN=BASE+'io_read_stdin_shared_core.stdin'
STDIN_BYTES=b'alpha\r\nbeta\r\nomega\r\n'
_AUTHORITIES=(
    'spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
    'spec/source/sections/005-4-statements.md','spec/source/sections/006-5-declarations.md',
    'spec/source/sections/009-8-error-handling.md','spec/source/sections/014-13-built-in-functions.md',
    'spec/source/sections/015-14-built-in-type-methods.md','spec/source/sections/016-15-standard-library.md',
    'stdlib/crypto/crypto.xr','stdlib/toml/toml.xr','stdlib/io/io.xr',
    'src/io/xfile_provider.c','src/aot/xrt_io.h',
    'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'src/aot/xrt_io.h': '263ee23032e10b34f4477b64f1b6c5c385b536b9fa5a1c2c7d704a2ad86e3a98',
 'src/io/xfile_provider.c': 'aca4dd347d996ff0812b5198229f87f59356b9d5d0ac04a88f2fbaf5f99f7dcf',
 'stdlib/crypto/crypto.xr': 'd622f2ba35963bf7ae52a5db06349adfdbdcdf5a43462a4fc497abdfb60df05d',
 'stdlib/io/io.xr': 'cbb670fe3a25e42a3fa96a3bf3460f9696f459c0fac059b82ba8bc99c4d17096',
 'stdlib/toml/toml.xr': '852b662c86950cbfde4c0cfb2a23e66675b921f0cf16e0e1eb660f6cb3a8b952',
 'tests/diff/cases/semantics/stdlib/crypto_timing_safe_equal_direct.xr': '92c90b37090a22a746c4837c35df06a15765d434d536cbf569f74c53b5aa0c05',
 'tests/diff/cases/semantics/stdlib/io_read_stdin_shared_core.xr': 'c5afbd4bd0983e98251d0c85893481dc95be6b32318fd4590a212f602aaf743a',
 'tests/diff/cases/semantics/stdlib/toml_pure_module_direct.xr': '31fa1eb79f86e7c822d4ad0f630d7fd3f86ecfe1df538426aee8f4b4608ba3e2',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}
_RAW_INPUTS={'tests/diff/cases/semantics/stdlib/io_read_stdin_shared_core.stdin': '7805807c263944785808d663457f8c088a859f1cad0b844a5c28bda816e06b9a'}
EXTERNAL_AUTHORITIES={
    'AES':'https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.197-upd1.pdf',
    'AES256_known_answer':'https://csrc.nist.gov/CSRC/media/Projects/Cryptographic-Standards-and-Guidelines/documents/examples/AES_Core256.pdf',
    'HMAC_known_answer':'https://www.rfc-editor.org/rfc/rfc4231.html#section-4.3'}


def multiply(a,b):
    result=0
    while b:
        if b&1:result^=a
        a=(a<<1)^ (0x11b if a&0x80 else 0);b>>=1
    return result


def field_power(value,power):
    result=1
    while power:
        if power&1:result=multiply(result,value)
        value=multiply(value,value);power>>=1
    return result


def substitute(value):
    inverse=field_power(value,254) if value else 0
    rotate=lambda x,n:((x<<n)|(x>>(8-n)))&255
    return inverse^rotate(inverse,1)^rotate(inverse,2)^rotate(inverse,3)^rotate(inverse,4)^0x63


SBOX=tuple(substitute(i) for i in range(256))
INVERSE_SBOX=tuple(SBOX.index(i) for i in range(256))


def round_keys(key):
    if len(key)!=32:raise ValueError('This independent cipher accepts AES-256 keys only')
    words=[list(key[i:i+4]) for i in range(0,32,4)];rcon=1
    for index in range(8,60):
        value=words[index-1][:]
        if index%8==0:
            value=[SBOX[x] for x in value[1:]+value[:1]];value[0]^=rcon;rcon=multiply(rcon,2)
        elif index%8==4:value=[SBOX[x] for x in value]
        words.append([a^b for a,b in zip(words[index-8],value)])
    return [sum(words[i:i+4],[]) for i in range(0,60,4)]


def shift_rows(state,inverse=False):
    return [state[4*((column+(-row if inverse else row))%4)+row] for column in range(4) for row in range(4)]


def mix_columns(state,inverse=False):
    matrix=((14,11,13,9),(9,14,11,13),(13,9,14,11),(11,13,9,14)) if inverse else (
        (2,3,1,1),(1,2,3,1),(1,1,2,3),(3,1,1,2))
    result=[]
    for offset in range(0,16,4):
        column=state[offset:offset+4]
        for row in matrix:
            value=0
            for coefficient,item in zip(row,column):value^=multiply(coefficient,item)
            result.append(value)
    return result


def aes_block(key,block,*,decrypt=False):
    if len(block)!=16:raise ValueError('AES requires one complete block')
    keys=round_keys(key);state=list(block)
    add=lambda a,b:[x^y for x,y in zip(a,b)]
    if not decrypt:
        state=add(state,keys[0])
        for index in range(1,15):
            state=shift_rows([SBOX[x] for x in state])
            if index<14:state=mix_columns(state)
            state=add(state,keys[index])
    else:
        state=add(state,keys[14])
        for index in range(13,-1,-1):
            state=[INVERSE_SBOX[x] for x in shift_rows(state,True)]
            state=add(state,keys[index])
            if index:state=mix_columns(state,True)
    return bytes(state)


def keys_for(key):
    return tuple(hmac.new(key,label,hashlib.sha256).digest() for label in (b'xray-aead-enc-v1',b'xray-aead-mac-v1'))


def encrypt(key,plain,iv):
    if len(iv)!=16:raise ValueError('Explicit independent IV must be16 bytes')
    enc_key,mac_key=keys_for(key);padding=16-len(plain)%16;padded=plain+bytes([padding])*padding
    state=iv;cipher=bytearray()
    for index in range(0,len(padded),16):
        state=aes_block(enc_key,bytes(x^y for x,y in zip(padded[index:index+16],state)));cipher.extend(state)
    signed=iv+bytes(cipher);return (signed+hmac.new(mac_key,signed,hashlib.sha256).digest()).hex()


def decrypt(key,wire):
    try:
        if len(wire)%2 or any(c not in '0123456789abcdefABCDEF' for c in wire):return None
        raw=bytes.fromhex(wire)
    except ValueError:return None
    cipher_length=len(raw)-48
    if cipher_length<=0 or cipher_length%16:raise ValueError('CryptoError.InvalidLength')
    enc_key,mac_key=keys_for(key);signed,tag=raw[:-32],raw[-32:]
    if not hmac.compare_digest(hmac.new(mac_key,signed,hashlib.sha256).digest(),tag):return None
    state=raw[:16];plain=bytearray()
    for index in range(16,len(signed),16):
        block=signed[index:index+16]
        plain.extend(x^y for x,y in zip(aes_block(enc_key,block,decrypt=True),state));state=block
    padding=plain[-1]
    if not 1<=padding<=16 or plain[-padding:]!=bytes([padding])*padding:return None
    return bytes(plain[:-padding])


def crypto_trace():
    pairs=[('abc','abc'),('abc','abd'),('abc','ab'),('',''),('same length','same length'),
           ('same length','same lengtz'),('prefix','prefix suffix')]
    result=[a.encode()==b.encode() for a,b in pairs]
    # Four distinct supplied IVs check only the original IV-invariant projections.
    projections=[]
    for iv in [bytes(16),bytes(range(16)),bytes([255])*16,bytes(range(16,32))]:
        cipher=encrypt(b'secret',b'hello world',iv);empty=encrypt(b'secret',b'',iv)
        projections.append([len(cipher)==64,decrypt(b'secret',cipher)==b'hello world',
            len(empty)==64,decrypt(b'secret',empty)==b'',decrypt(b'wrong-key',cipher) is None])
    if any(value!=projections[0] for value in projections):raise ValueError('Original crypto projections changed with independent IV')
    result.extend(projections[0])
    for algorithm in ['md5','sha1','sha256','sha512']:result.append(hashlib.new(algorithm,b'abc').hexdigest())
    for algorithm in ['md5','sha1','sha256','sha512']:result.append(hmac.new(b'key',b'data',algorithm).hexdigest())
    result.append(hmac.new(b'Jefe',b'what do ya want for nothing?',hashlib.sha256).hexdigest())
    result.append('sha224' not in {'md5','sha1','sha256','sha512'})
    return result


@dataclass(frozen=True)
class Diagnostic:
    kind: str
    line: int
    message: str


def toml_report(text):
    """Recover the selected flat diagnostic inputs; valid TOML uses tomllib."""
    try:return tomllib.loads(text),[],len(text.split('\n'))
    except tomllib.TOMLDecodeError:pass
    data={};errors=[]
    for index,line in enumerate(text.split('\n'),1):
        if line.lstrip().startswith('['):raise ValueError('No invented malformed table-recovery policy')
        try:fragment=tomllib.loads(line)
        except tomllib.TOMLDecodeError:
            errors.append(Diagnostic('SyntaxError',index,''));continue
        for key,value in fragment.items():
            if key in data:errors.append(Diagnostic('DuplicateKey',index,'Duplicate key'))
            else:data[key]=value
    return data,errors,len(text.split('\n'))


def stringify_flat(table):
    def value(item):
        if type(item) is bool:return str(item).lower()
        if type(item) is int:return str(item)
        if type(item) is str:return json.dumps(item,ensure_ascii=False)
        raise ValueError('Only the complete original scalar table is serialized here')
    return '\n'.join(key+' = '+value(item) for key,item in table.items())+'\n'


def toml_trace():
    basic=tomllib.loads('name = "xray"\nversion = 2\nactive = true')
    result=[basic['name'],basic['version'],basic['active']]
    fruit=tomllib.loads('fruit.name = "apple"\nfruit.color = "red"')['fruit'];result.extend([fruit['name'],fruit['color']])
    products=tomllib.loads('[[products]]\nname = "Hammer"\n\n[[products]]\nname = "Nail"')['products']
    result.extend([len(products),products[1]['name']])
    data,errors,_=toml_report('name = "Alice"\nname = "Bob"');diag=errors[0]
    result.extend([len(errors)>0,'TomlDiagnostic',diag.kind=='DuplicateKey',diag.line,diag.message,data['name']])
    _,errors,_=toml_report('x = [1, 2');result.append(errors[0].kind=='SyntaxError')
    _,errors,lines=toml_report('name = "Alice"\nage = 30');result.extend([len(errors),lines>0])
    output=stringify_flat({'name':'xray','version':1,'enabled':True})
    result.extend(['name = "xray"' in output,'version = 1' in output,tomllib.loads(output)['enabled']])
    return result


def stdin_trace(data):
    text=data.decode('utf-8')
    return [len(data),text.startswith('alpha\n'),'\nbeta\n' in text,text.endswith('omega\n')]


def observations():
    return {'crypto_timing_safe_equal_direct':crypto_trace(),'toml_pure_module_direct':toml_trace(),
            'io_read_stdin_shared_core':stdin_trace(STDIN_BYTES)}


def expected_rows():
    if (set(_PROGRAMS)!=_NAMES or any(path!=BASE+name+'.xr' for name,path in _PROGRAMS.items()) or
            set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()) or set(_RAW_INPUTS)!={_STDIN}):
        raise ValueError('Pure library model lost complete original programs or exact authorities')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Pure library source or authority changed: '+path)
    for path,digest in _RAW_INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes()).hexdigest()!=digest or (ROOT/path).read_bytes()!=STDIN_BYTES:
            raise ValueError('Original stdin bytes changed;line endings are observable')
    traces=observations()
    if set(traces)!=_NAMES or sum(map(len,traces.values()))!=45:raise ValueError('Pure library model lost complete original observations')
    rows=[]
    for name,path in sorted(_PROGRAMS.items()):
        row={'id':'complete_pure_library_boundaries_'+name,'source':path,'original_source':path,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':''.join(render({'expected':value})+'\n' for value in traces[name]),'stderr_utf8':''},
            'observation_count':len(traces[name]),
            'model':'Complete unchanged original;independent AES256/CBC+HMAC/PKCS7 and digests,typed TOML recovery and exact binary stdin. No product output or timing/entropy/provider/security qualification.'}
        if name=='io_read_stdin_shared_core':row['required_stdin_hex']=STDIN_BYTES.hex()
        rows.append(row)
    return rows
