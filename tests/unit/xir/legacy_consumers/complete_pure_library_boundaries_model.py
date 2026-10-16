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

_ORIGINAL_NAMES=_NAMES
_SYSTEM_PROGRAMS={
 "math_core_direct_type_preserve": "tests/diff/cases/semantics/stdlib/math_core_direct_type_preserve.xr",
 "rounding_boundary": "tests/diff/cases/semantics/float_fmt/rounding_boundary.xr",
 "crypto_random_system_direct": "tests/diff/cases/semantics/stdlib/crypto_random_system_direct.xr",
 "runtime_domain_bytes": "tests/diff/cases/semantics/stdlib/runtime_domain_bytes.xr"
}
_NAMES=_NAMES|frozenset(_SYSTEM_PROGRAMS)
_PROGRAMS.update(_SYSTEM_PROGRAMS)
_AUTHORITIES+=("stdlib/math/math.xr","stdlib/runtime/runtime.xr","src/coro/xchannel.c","src/coro/xchannel.h","src/runtime/xshared.h","spec/source/sections/018-17-compilation-pipeline.md","src/xir/xxir_float.h","src/xir/xxir_float_format.inc.c",)
_INPUTS.update({
 "tests/diff/cases/semantics/stdlib/math_core_direct_type_preserve.xr": "ed86297b0fc6c1bf2f6fa5ebe975f065454775865009d48b2be0d7426efa46c4",
 "tests/diff/cases/semantics/float_fmt/rounding_boundary.xr": "19fa8714c9d146eb83f4067df1b81f2bbbdbfb192868a36324a24cb5d7353e61",
 "tests/diff/cases/semantics/stdlib/crypto_random_system_direct.xr": "6319e875870f460cf312e04a2813bf4c721727162752db95018ad7197b77b33c",
 "tests/diff/cases/semantics/stdlib/runtime_domain_bytes.xr": "9f5e53e86d9d923f072babd0e98c9594fbea8f9a8e54f8b5f01bc620fcfbb324",
 "stdlib/math/math.xr": "0126a1eb1db1760b5950da3f6be0c47f504f65070afaff3757f7b45f27a86115",
 "stdlib/runtime/runtime.xr": "9f4d87664a727e7b29ceec161efe07d67d4e416c47030b06c682062f8f2fe57c",
 "src/coro/xchannel.c": "8cc701e6dd5e14e2a9b44a64ca77c209b1d2e1736cf5011b1e2e2999ef84f9b3",
 "src/coro/xchannel.h": "2ee14ccab5cf19bab7172a29c1d2ecaaa85bf30a856d1bc5cd21bbfaaa06e6c8",
 "src/runtime/xshared.h": "152b7729cf355225e6bdb77c32734b5da33b0b39e409cb556898a83bca2e56c8",
 "spec/source/sections/018-17-compilation-pipeline.md": "91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440",
 "src/xir/xxir_float.h": "51da2cda6e5da3a0cbd63201a4d20a743fd3671ed7c077337ee8a5407e8ccc63",
 "src/xir/xxir_float_format.inc.c": "84b274ea969dbd5a62a7b8b780d87cad905c20dff46c3f752662c1aab87ef457"
})


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


from fractions import Fraction
from math import isqrt


def power(base,exponent):return Fraction(base**exponent) if exponent>=0 else Fraction(1,base**-exponent)


def nearest_even(value):
    if value<0:raise ValueError('Unsigned rounding helper requires nonnegative input')
    whole,remainder=divmod(value.numerator,value.denominator)
    return whole+int(2*remainder>value.denominator or 2*remainder==value.denominator and whole%2)


def floor_log(value,base):
    if base not in (2,10):raise ValueError('Only binary and decimal exponent bounds are reviewed')
    if value<=0:raise ValueError('Logarithm bound requires positive input')
    exponent=(value.numerator.bit_length()-value.denominator.bit_length()) if base==2 else len(str(value.numerator))-len(str(value.denominator))
    while value<power(base,exponent):exponent-=1
    while value>=power(base,exponent+1):exponent+=1
    return exponent


def fraction_bits(value):
    if type(value) not in {int,Fraction}:raise ValueError('Use exact source literals,not a host floating calculation')
    value=Fraction(value);sign=int(value<0);value=abs(value)
    if not value:return 0
    exponent=floor_log(value,2)
    if not -1022<=exponent<=1023:raise ValueError('Selected arithmetic requires finite normal binary64 results')
    significand=nearest_even(value*power(2,52-exponent))
    if significand==1<<53:significand>>=1;exponent+=1
    if exponent>1023:raise ValueError('Selected arithmetic overflow')
    return sign<<63|(exponent+1023)<<52|(significand-(1<<52))


def bits_fraction(bits):
    if type(bits) is not int or not 0<=bits<1<<64:raise ValueError('Invalid binary64 bits')
    exponent=(bits>>52)&2047;mantissa=bits&((1<<52)-1)
    if exponent==2047:raise ValueError('Nonfinite values have no rational payload')
    value=Fraction(mantissa)*power(2,-1074) if exponent==0 else Fraction((1<<52)+mantissa)*power(2,exponent-1075)
    return -value if bits>>63 else value


def sqrt_bits(value):
    value=Fraction(value)
    if value<0:raise ValueError('Positive-root helper does not model the NaN case')
    if not value:return 0
    exponent=floor_log(value,2)//2;scaled=value*power(2,2*(52-exponent))
    whole=isqrt(scaled.numerator//scaled.denominator)
    relation=4*scaled.numerator-scaled.denominator*(2*whole+1)**2
    significand=whole+int(relation>0 or relation==0 and whole%2)
    if significand==1<<53:significand>>=1;exponent+=1
    return (exponent+1023)<<52|(significand-(1<<52))


def shortest_float(bits):
    if type(bits) is not int or not 0<=bits<1<<64:raise ValueError('Invalid binary64 bits')
    sign='-' if bits>>63 else '';magnitude=bits&((1<<63)-1);exponent_bits=(magnitude>>52)&2047
    if exponent_bits==2047:return 'nan' if magnitude&((1<<52)-1) else sign+'inf'
    if not magnitude:return sign+'0.0'
    value=bits_fraction(magnitude);previous=bits_fraction(magnitude-1)
    following=bits_fraction(magnitude+1) if magnitude<0x7fefffffffffffff else power(2,1024)
    lower,upper=(previous+value)/2,(value+following)/2;closed=magnitude%2==0
    exponent=floor_log(value,10);chosen=None
    for precision in range(1,18):
        scale=power(10,precision-1-exponent);nearest=nearest_even(value*scale);candidates=[]
        for coefficient in {max(1,nearest-1),max(1,nearest),nearest+1}:
            candidate=Fraction(coefficient)/scale
            if (candidate>lower or closed and candidate==lower) and (candidate<upper or closed and candidate==upper):
                normalized=coefficient
                while normalized%10==0:normalized//=10
                candidates.append((abs(candidate-value),normalized%2,coefficient))
        if candidates:
            coefficient=min(candidates)[2];decimal_power=exponent-precision+1;chosen=(coefficient,decimal_power);break
    if chosen is None:raise ValueError('No shortest binary64 round-trip candidate within17digits')
    coefficient,decimal_power=chosen
    while coefficient%10==0:coefficient//=10;decimal_power+=1
    digits=str(coefficient);exponent=decimal_power+len(digits)-1
    if -4<=exponent<16:
        position=exponent+1
        if position<=0:text='0.'+'0'*-position+digits
        elif position>=len(digits):text=digits+'0'*(position-len(digits))
        else:text=digits[:position]+'.'+digits[position:]
        if '.' not in text:text+='.0'
    else:text=digits[0]+('.'+digits[1:] if len(digits)>1 else '')+'e'+('+' if exponent>=0 else '-')+str(abs(exponent))
    return sign+text


@dataclass(frozen=True)
class FloatValue:
    bits:int
    @classmethod
    def literal(cls,value):return cls(fraction_bits(value))
    def fraction(self):return bits_fraction(self.bits)
    def nan(self):return self.bits&0x7ff0000000000000==0x7ff0000000000000 and bool(self.bits&((1<<52)-1))
    def finite(self):return self.bits&0x7ff0000000000000!=0x7ff0000000000000
    def text(self):return shortest_float(self.bits)
    def typename(self):return 'f64'


@dataclass(frozen=True)
class IntegerValue:
    value:int
    def typename(self):return 'i64'


def math_value(name,*values,calls=None):
    if calls is not None:calls.append(name)
    args=[value if isinstance(value,FloatValue) else FloatValue.literal(value) for value in values]
    if name in {'isNaN','isFinite'}:
        if len(args)!=1:raise ValueError('Wrong predicate arity')
        return args[0].nan() if name=='isNaN' else args[0].finite()
    if any(value.nan() for value in args):return FloatValue(0x7ff8000000000000)
    x=[value.fraction() for value in args]
    if name=='abs':return FloatValue.literal(abs(x[0]))
    if name in {'min','max'}:
        return (min if name=='min' else max)(args,key=lambda value:value.fraction()) if args else FloatValue(0x7ff8000000000000)
    if name=='clamp':
        value,lo,hi=x;lo,hi=min(lo,hi),max(lo,hi);return FloatValue.literal(min(max(value,lo),hi))
    if name in {'floor','ceil','round','trunc','sign'}:
        value=x[0]
        if name=='floor':answer=value.numerator//value.denominator
        elif name=='ceil':answer=-((-value.numerator)//value.denominator)
        elif name=='trunc':answer=(1 if value>=0 else -1)*(abs(value.numerator)//value.denominator)
        elif name=='round':answer=(1 if value>=0 else -1)*((abs(value)+Fraction(1,2)).numerator//(abs(value)+Fraction(1,2)).denominator)
        else:answer=int(value>0)-int(value<0)
        return IntegerValue(answer)
    if name=='sqrt':return FloatValue(0x7ff8000000000000) if x[0]<0 else FloatValue(sqrt_bits(x[0]))
    if name=='pow' and x[1]==2:return FloatValue.literal(x[0]*x[0])
    if name=='hypot':return FloatValue(sqrt_bits(sum(value*value for value in x)))
    if name=='cbrt' and x==[27]:
        if 3**3!=27:raise ValueError('Cube identity failed')
        return FloatValue.literal(3)
    if name=='fmod':
        quotient=x[0]/x[1];whole=(1 if quotient>=0 else -1)*(abs(quotient.numerator)//quotient.denominator)
        return FloatValue.literal(x[0]-whole*x[1])
    if name=='lerp':return FloatValue.literal(x[0]+(x[1]-x[0])*x[2])
    facts={('sin',(0,)):0,('cos',(0,)):1,('tan',(0,)):0,('asin',(0,)):0,('acos',(1,)):0,('atan',(0,)):0,('atan2',(0,1)):0,
     ('log',(1,)):0,('log10',(100,)):2,('log2',(8,)):3,('exp',(0,)):1,('sinh',(0,)):0,('cosh',(0,)):1,('tanh',(0,)):0,
     ('log1p',(0,)):0,('expm1',(0,)):0,('degToRad',(0,)):0,('radToDeg',(0,)):0}
    if (name,tuple(x)) not in facts:raise ValueError('Outside the reviewed exact mathematical identities')
    return FloatValue.literal(facts[name,tuple(x)])


def math_core_trace():
    calls=[];ev=lambda name,*args:math_value(name,*args,calls=calls)
    a=ev('abs',-42);amin=ev('abs',-(2**63));mn=ev('min',3,7,5);mx=ev('max',3,7,5);cl=ev('clamp',5,10,0)
    out=[a.typename(),a,amin.typename(),amin.fraction()>0,mn.typename(),mn,mx.typename(),mx,cl.typename(),cl,
     ev('min',9,4,6),ev('max',9,4,6),ev('clamp',15,10,0),ev('min',Fraction(7,2),Fraction(36,5)).typename(),ev('clamp',Fraction(11,2),10,0),
     ev('floor',Fraction(9,5)),ev('ceil',Fraction(6,5)),ev('round',Fraction(8,5)),ev('trunc',Fraction(9,5))]
    out.extend(ev(name,*args) for name,args in [('sin',(0,)),('cos',(0,)),('tan',(0,)),('asin',(0,)),('acos',(1,)),('atan',(0,)),('atan2',(0,1)),
     ('log',(1,)),('log10',(100,)),('log2',(8,)),('exp',(0,)),('sinh',(0,)),('cosh',(0,)),('tanh',(0,)),('hypot',(3,4)),('cbrt',(27,)),('fmod',(7,4)),
     ('log1p',(0,)),('expm1',(0,)),('lerp',(2,4,Fraction(1,4))),('degToRad',(0,)),('radToDeg',(0,)),('sign',(-10,))])
    out.extend([ev('isNaN',ev('sqrt',-1)),ev('isFinite',1),ev('isFinite',ev('sqrt',-1))])
    if len(calls)!=42 or len(out)!=45:raise ValueError('Lost original typed math calls or outputs')
    return [value.text() if isinstance(value,FloatValue) else value.value if isinstance(value,IntegerValue) else value for value in out],calls


def float_binary(left,right,operation):
    a,b=left.fraction(),right.fraction()
    value={'add':lambda:a+b,'sub':lambda:a-b,'mul':lambda:a*b,'div':lambda:a/b}[operation]()
    return FloatValue.literal(value)


def rounding_trace():
    a,b,c=(FloatValue.literal(value) for value in [Fraction(1,10),Fraction(1,5),Fraction(3,10)])
    add=float_binary(a,b,'add');difference=float_binary(add,c,'sub');negative=float_binary(float_binary(c,a,'sub'),b,'sub')
    root=math_value('sqrt',2);square=float_binary(math_value('sqrt',2),math_value('sqrt',2),'mul')
    squared=math_value('pow',a,2);one=FloatValue.literal(1);two=FloatValue.literal(2);three=FloatValue.literal(3)
    return [value.text() for value in [add,difference,negative,root,square,squared,float_binary(one,three,'div'),float_binary(two,three,'div')]]


HEX=frozenset('0123456789abcdef')


def random_hex_domains(count):
    if type(count) is not int or count<=0:raise ValueError('Selected random request must be positive')
    return tuple(HEX for _ in range(2*count))


def uuid_domains():
    digits=list(random_hex_domains(16));digits[12]=frozenset('4');digits[16]=frozenset('89ab')
    result=[];start=0
    for length in (8,4,4,4,12):
        if result:result.append(frozenset('-'))
        result.extend(digits[start:start+length]);start+=length
    return tuple(result)


def uuid_from_bytes(data):
    if len(data)!=16:raise ValueError('Selected UUID requires16 bytes')
    data=bytearray(data);data[6]=(data[6]&15)|64;data[8]=(data[8]&63)|128
    text=''.join(format(value,'02x') for value in data)
    return '-'.join(text[a:b] for a,b in [(0,8),(8,12),(12,16),(16,20),(20,32)])


def random_trace():
    r8,r16=random_hex_domains(8),random_hex_domains(16);uuid=uuid_domains();parts=[[]]
    for domain in uuid:
        if domain==frozenset('-'):parts.append([])
        else:
            if '-' in domain:raise ValueError('Ambiguous symbolic UUID separator')
            parts[-1].append(domain)
    variant=uuid[19]
    return [len(r8)==16,len(r16)==32,len(uuid)==36,len(parts)==5,*[len(part)==length for part,length in zip(parts,(8,4,4,4,12))],
     uuid[14]==frozenset('4'),bool(variant) and variant<=frozenset('89ab')]


@dataclass(frozen=True)
class DomainRead:
    base:str
    nonnegative:bool
    charge_minimum:int=0
    def at_least_zero(self):return self.nonnegative and self.charge_minimum>=0
    def guaranteed_greater(self,previous):return self.base==previous.base and self.charge_minimum>previous.charge_minimum


def channel_charge_minimum(capacity):
    if type(capacity) is not int or capacity<0:raise ValueError('Invalid selected channel capacity')
    # The owned Channel header is a nonempty SYNC_SHARED allocation. This is
    # only a positive lower bound, never a prediction of actual byte counts.
    return 1


def domain_trace():
    before=DomainRead('shared_before_channel',True);during=DomainRead(before.base,True,channel_charge_minimum(16))
    static=DomainRead('static_after_close',True);a=DomainRead('shared_read_a_after_close',True);b=DomainRead('shared_read_b_after_close',True)
    return [during.guaranteed_greater(before),static.at_least_zero(),b.at_least_zero() and a.at_least_zero()]


def system_observations():
    rows={'math_core_direct_type_preserve':math_core_trace()[0],'rounding_boundary':rounding_trace(),
     'crypto_random_system_direct':random_trace(),'runtime_domain_bytes':domain_trace()}
    if set(rows)!=set(_SYSTEM_PROGRAMS) or sum(map(len,rows.values()))!=67:raise ValueError('Lost full math/random/domain observations')
    return rows


def observations():
    rows={'crypto_timing_safe_equal_direct':crypto_trace(),'toml_pure_module_direct':toml_trace(),
            'io_read_stdin_shared_core':stdin_trace(STDIN_BYTES)}
    rows.update(system_observations());return rows


def expected_rows():
    if (set(_PROGRAMS)!=_NAMES or any(path!=BASE+name+'.xr' for name,path in _PROGRAMS.items() if name in _ORIGINAL_NAMES) or
            any(_PROGRAMS.get(name)!=path for name,path in _SYSTEM_PROGRAMS.items()) or
            set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()) or set(_RAW_INPUTS)!={_STDIN}):
        raise ValueError('Pure library model lost complete original programs or exact authorities')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Pure library source or authority changed: '+path)
    for path,digest in _RAW_INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes()).hexdigest()!=digest or (ROOT/path).read_bytes()!=STDIN_BYTES:
            raise ValueError('Original stdin bytes changed;line endings are observable')
    traces=observations()
    if set(traces)!=_NAMES or sum(map(len,traces.values()))!=112:raise ValueError('Pure library model lost complete original observations')
    rows=[]
    for name,path in sorted(_PROGRAMS.items()):
        row={'id':'complete_pure_library_boundaries_'+name,'source':path,'original_source':path,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':''.join(render({'expected':value})+'\n' for value in traces[name]),'stderr_utf8':''},
            'observation_count':len(traces[name]),
            'model':'Complete unchanged original;independent AES256/CBC+HMAC/PKCS7 and digests,typed TOML recovery and exact binary stdin. No product output or timing/entropy/provider/security qualification.'}
        if name=='io_read_stdin_shared_core':row['required_stdin_hex']=STDIN_BYTES.hex()
        rows.append(row)
    for row in rows:
        if row['original_source'] in _SYSTEM_PROGRAMS.values():
            row['model']='Complete unchanged original;independent exact rational/binary64 arithmetic and shortest decimal interval display,declared math return families and exact selected identities,all-byte UUID/hex shape domains,and symbolic nonnegative/positive shared-allocation counter predicates. No product output,entropy distribution,physical meter,lifetime,FI,safety or provider qualification.'
    return rows
