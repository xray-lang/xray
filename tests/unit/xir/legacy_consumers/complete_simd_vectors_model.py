"""Complete original SIMD consumer traces from independent unsigned lane algebra.

Integer widths and shifts follow frozen scalar rules. Lane reinterpretation is
explicit little-byte concatenation from the SIMD declaration, not host layout.
This records all assertions and stores, not only success banners. Legacy
ref Slice store signatures remain unresolved producer work; no ABI, intrinsic,
alignment, acceleration, borrow, resource or provider qualification is inferred.
"""
from frozen_derivation_inputs import resolve as resolve_derivation
from dataclasses import dataclass
from pathlib import Path
import hashlib,re

ROOT=Path(__file__).resolve().parents[4]
_NAMES=('simd_portable_integer_vectors','simd_top_level_shared_vectors')
ORIGINALS=tuple('tests/diff/cases/semantics/stdlib/'+name+'.xr' for name in _NAMES)
_AUTHORITIES=('spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/006-5-declarations.md','spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md','spec/source/sections/016-15-standard-library.md',
 'spec/source/sections/018-17-compilation-pipeline.md','stdlib/simd/simd.xr','stdlib/types/array.xr')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'stdlib/simd/simd.xr': '35c745eb6a37f37a0c7a2d2daf4d4bb953582e930929970d330a2cd3469696d9',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'tests/diff/cases/semantics/stdlib/simd_portable_integer_vectors.xr': 'd2169107c3035c858f47771bbd4c0c6db9bc99f640f071ccb77509a5e9dcb00a',
 'tests/diff/cases/semantics/stdlib/simd_top_level_shared_vectors.xr': 'b053d22d8211064d249e1d957442a43c6799b406081a317fb6b5aa1d9ebe2319'}


@dataclass(frozen=True)
class Vector:
    width: int
    lanes: tuple

    def __post_init__(self):
        if self.width not in (8,32,64) or not self.lanes or any(type(x) is not int or not 0<=x<1<<self.width for x in self.lanes):
            raise ValueError('Exact unsigned vector width and canonical lanes required')

    @classmethod
    def splat(cls,width,count,value):return cls(width,(value,)*count)

    @classmethod
    def load(cls,width,count,storage,offset=0):
        if offset<0 or offset+count>len(storage):raise ValueError('Vector load exceeds full source view')
        return cls(width,tuple(storage[offset:offset+count]))

    def binary(self,other,operation):
        if (self.width,len(self.lanes))!=(other.width,len(other.lanes)):raise ValueError('Vector types differ')
        return Vector(self.width,tuple(operation(a,b)%(1<<self.width) for a,b in zip(self.lanes,other.lanes)))

    def add(self,other):return self.binary(other,lambda a,b:a+b)
    def mul(self,other):return self.binary(other,lambda a,b:a*b)
    def xor(self,other):return self.binary(other,lambda a,b:a^b)

    def shift(self,count,left=False):
        count%=64
        return Vector(self.width,tuple(((x<<count) if left else (x>>count))%(1<<self.width) for x in self.lanes))

    def shuffle(self,indices):
        if len(indices)!=len(self.lanes) or any(not 0<=i<len(self.lanes) for i in indices):raise ValueError('Invalid lane selection')
        return Vector(self.width,tuple(self.lanes[i] for i in indices))

    def swap(self):return self.shuffle([i^1 for i in range(len(self.lanes))])

    def replace(self,index,value):
        if not 0<=index<len(self.lanes):raise ValueError('Invalid lane replacement')
        return Vector(self.width,self.lanes[:index]+(value,)+self.lanes[index+1:])

    def reinterpret(self,width):
        data=b''.join(x.to_bytes(self.width//8,'little') for x in self.lanes)
        if width not in (8,32,64) or len(data)%(width//8):raise ValueError('Inexact vector byte width')
        return Vector(width,tuple(int.from_bytes(data[i:i+width//8],'little') for i in range(0,len(data),width//8)))

    def widen(self,other,indices):
        if self.width!=32 or other.width!=32 or len(self.lanes)!=len(other.lanes):raise ValueError('Widen needs exact matching u32 vectors')
        if len(indices)*2!=len(self.lanes) or any(not 0<=i<len(self.lanes) for i in indices):raise ValueError('Invalid widening selection')
        return Vector(64,tuple(self.lanes[i]*other.lanes[i] for i in indices))

    def unzip(self,other,odd=False):
        if self.width!=other.width or len(self.lanes)!=len(other.lanes):raise ValueError('Unzip vector types differ')
        return Vector(self.width,self.lanes[int(odd)::2]+other.lanes[int(odd)::2])

    def reduce(self):return sum(self.lanes)%(1<<self.width)

    def store(self,storage,offset=0):
        if offset<0 or offset+len(self.lanes)>len(storage):raise ValueError('Vector store exceeds full destination view')
        storage[offset:offset+len(self.lanes)]=self.lanes


class Trace:
    def __init__(self):self.assertions=[];self.stores=[]

    def equal(self,label,actual,expected):
        passed=type(actual) is type(expected) and actual==expected
        self.assertions.append({'label':label,'actual':actual,'expected':expected,'passed':passed})
        if not passed:raise ValueError('Original SIMD assertion failed: '+label)

    def truth(self,label,value):self.equal(label,value,True)

    def store(self,label,value,storage,offset=0):
        prior=storage.copy();value.store(storage,offset)
        self.stores.append({'label':label,'offset':offset,'lane_count':len(value.lanes),'width':value.width,
            'before':prior,'after':storage.copy()})


def portable_trace(native_bytes=16,accelerated=False,scalable=False):
    # The source baseline is16. Alternate finite prefixes are algebra controls,
    # not a claim that any target/provider selects or implements them.
    if native_bytes not in (16,32,64) or type(accelerated) is not bool or type(scalable) is not bool:
        raise ValueError('Only declared baseline and bounded alternative prefixes are modeled')
    t=Trace();eq=t.equal
    bytes32=[(i*17+3)&255 for i in range(32)];b=Vector.load(8,16,bytes32,3)
    eq('b.extract0',b.lanes[0],bytes32[3]);eq('b.extract15',b.lanes[15],bytes32[18])
    eq('byte xor',b.xor(Vector.splat(8,16,255)).lanes[4],(~bytes32[7])%256)
    words=[1,2,3,4,5,6];a32=Vector.load(32,4,words,1);b32=Vector.splat(32,4,7)
    eq('add',a32.add(b32).lanes[0],9);eq('mul',a32.mul(b32).lanes[3],35)
    eq('shift roundtrip',a32.shift(3,True).shift(3).lanes[2],4)
    eq('shift32',a32.shift(32,True).lanes[0],0);eq('shift63',a32.shift(63).lanes[3],0)
    eq('shuffle',a32.shuffle([3,2,1,0]).lanes[0],5)
    eq('swap0',a32.swap().lanes[0],3);eq('swap2',a32.swap().lanes[2],5)
    even=a32.widen(b32,[0,2]);eq('even0',even.lanes[0],14);eq('even1',even.lanes[1],28)
    eq('odd reduce',a32.widen(b32,[1,3]).reduce(),56)
    even_words=a32.unzip(b32);odd_words=a32.unzip(b32,True)
    eq('unzip even0',even_words.lanes[0],2);eq('unzip even2',even_words.lanes[2],7)
    eq('unzip odd0',odd_words.lanes[0],3);eq('unzip odd2',odd_words.lanes[2],7)
    low=even_words.widen(odd_words,[0,1]);high=even_words.widen(odd_words,[2,3])
    eq('low0',low.lanes[0],6);eq('low1',low.lanes[1],20);eq('high0',high.lanes[0],49);eq('high1',high.lanes[1],49)
    lanes64=[18446744073709551615,9];a64=Vector(64,tuple(lanes64))
    eq('wrap64',a64.add(Vector.splat(64,2,1)).lanes[0],0);eq('swap64',a64.swap().lanes[0],9)
    eq('replace64',a64.replace(1,21).lanes[1],21);eq('reinterpret32',a64.reinterpret(32).lanes[0],4294967295)
    eq('reinterpret64',a64.reinterpret(32).reinterpret(64).lanes[1],9)
    roundtrip=b.reinterpret(32).reinterpret(8)
    for i in range(16):eq('roundtrip32['+str(i)+']',roundtrip.lanes[i],b.lanes[i])
    imported=b.reinterpret(32);eq('imported return swap',imported.swap().lanes[0],imported.lanes[1])
    roundtrip64=b.reinterpret(64).reinterpret(8)
    for i in range(16):eq('roundtrip64['+str(i)+']',roundtrip64.lanes[i],b.lanes[i])
    out=[None]*4;t.store('out',a64,out,1);eq('out1',out[1],lanes64[0]);eq('out2',out[2],lanes64[1])
    x32=Vector(32,tuple(range(1,9)));products=x32.widen(x32.swap(),[0,2,4,6]);odd_products=x32.widen(x32,[1,3,5,7])
    eq('odd products reduce',odd_products.reduce(),120)
    eq('wide splat odd',Vector.splat(32,8,3).widen(Vector.splat(32,8,4),[1,3,5,7]).reduce(),48)
    wide=Vector(64,(10,20,30,40));combined=wide.add(wide.swap())
    transformed=wide.xor(wide.shift(1)).shift(2,True).reinterpret(32)
    eq('transformed widen',transformed.widen(Vector.splat(32,8,1),[0,2,4,6]).reduce(),488)
    eq('wide bytes roundtrip',wide.reinterpret(8).reinterpret(64).reduce(),100)
    combined=combined.add(products);eq('combined reduce',combined.reduce(),300)
    wide_out=[None]*4;t.store('wideOut',combined,wide_out)
    for i,value in enumerate([32,42,100,126]):eq('wideOut['+str(i)+']',wide_out[i],value)
    xbytes=Vector.load(8,32,bytes32).xor(Vector.load(8,32,bytes32));t.store('wideOut zero',xbytes.reinterpret(64),wide_out)
    eq('wideOut zero0',wide_out[0],0)
    bytes512=[(i*13+5)&255 for i in range(64)];zero512=Vector.load(8,64,bytes512).xor(Vector.load(8,64,bytes512))
    t.store('bytes512',zero512,bytes512);eq('bytes512 first',bytes512[0],0);eq('bytes512 last',bytes512[63],0)
    words512=Vector(32,tuple(range(1,17)));products512=words512.widen(words512.swap(),list(range(0,16,2)))
    lanes512=Vector(64,(10,20,30,40,50,60,70,80));combined512=lanes512.swap().add(products512)
    combined512=combined512.xor(combined512.shift(47)).add(Vector.splat(32,16,3).widen(words512,list(range(1,16,2))).shift(32,True))
    out512=[None]*8;t.store('out512',combined512,out512)
    eq('out512 first',out512[0],22+(6<<32));eq('out512 last',out512[7],310+(48<<32))
    roundtrip512=lanes512.reinterpret(32).widen(Vector.splat(32,16,1),list(range(0,16,2)))
    t.store('out512 roundtrip',roundtrip512,out512);eq('out512 roundtrip first',out512[0],10);eq('out512 roundtrip last',out512[7],80)
    native_bytes_buffer=[1]*64;native_data=Vector.load(8,native_bytes,native_bytes_buffer);native_words=native_data.reinterpret(32)
    native_products=native_words.widen(native_words.swap(),list(range(0,native_bytes//4,2)))
    native_result=native_products.xor(native_products).add(Vector.splat(64,native_bytes//8,5)).mul(Vector.splat(64,native_bytes//8,3)).shift(1).swap()
    native_result=native_result.add(native_data.xor(native_data).reinterpret(64));native_out=[99]*8
    t.store('nativeOut',native_result,native_out)
    for i in range(native_bytes//8):eq('nativeOut['+str(i)+']',native_out[i],7)
    t.truth('native width minimum',native_bytes>=16);t.truth('accelerated tautology',accelerated or not accelerated);t.truth('scalable tautology',scalable or not scalable)
    if (len(t.assertions),len(t.stores))!=(81+native_bytes//8,7):raise ValueError('Original full SIMD assertion/store trace lost')
    return {'stdout_utf8':'OK simd portable integer vectors\n','original_assertions':t.assertions,'original_stores':t.stores,
        'initialization_loop_iterations':{'bytes32':32,'bytes512':64,'nativeBytesBuffer':64},'roundtrip_lane_assertions':32,
        'active_native_bytes':native_bytes,'native_out':native_out}


def top_level_trace():
    t=Trace();eq=t.equal;lanes64=(18446744073709551615,9);global64=Vector(64,lanes64);global32=global64.reinterpret(32)
    for i,value in enumerate([4294967295,4294967295,9,0]):eq('global32['+str(i)+']',global32.lanes[i],value)
    round64=global32.reinterpret(64)
    for i in range(2):eq('round64['+str(i)+']',round64.lanes[i],lanes64[i])
    bytes16=tuple(range(1,17));global_bytes=Vector(8,bytes16);global_words=global_bytes.reinterpret(32)
    eq('global words0',global_words.lanes[0],67305985);eq('global words3',global_words.lanes[3],269422093)
    round_bytes=global_words.reinterpret(8)
    for i in range(16):eq('round bytes['+str(i)+']',round_bytes.lanes[i],bytes16[i])
    if len(t.assertions)!=24:raise ValueError('Original top-level SIMD assertion loop lost')
    return {'stdout_utf8':'OK simd top-level shared vectors\n','original_assertions':t.assertions,
        'global64_lanes':list(global64.lanes),'global32_lanes':list(global32.lanes),'global_byte_roundtrip':list(round_bytes.lanes)}


def expected_rows():
    if (set(_INPUTS)!=set(_AUTHORITIES)|set(ORIGINALS) or _NAMES!=('simd_portable_integer_vectors','simd_top_level_shared_vectors')
        or ORIGINALS!=tuple('tests/diff/cases/semantics/stdlib/'+n+'.xr' for n in _NAMES)):
        raise ValueError('Complete SIMD source or authority identities changed')
    for name,digest in _INPUTS.items():
        if hashlib.sha256(resolve_derivation(ROOT, name, digest).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('SIMD complete source or normative input changed: '+name)
    rows=[]
    for name,source,trace,sites in zip(_NAMES,ORIGINALS,[portable_trace(),top_level_trace()],[(49,3),(9,0)]):
        text=(ROOT/source).read_text(encoding='utf-8')
        if (text.count('assertEqual('),len(re.findall(r'\bassert\(',text)))!=sites or text.count('print(')!=1:
            raise ValueError('Original complete SIMD body lost assertions or output sites')
        row={'id':'complete_simd_vectors_'+name,'source':source,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':trace['stdout_utf8'],'stderr_utf8':''},'observation_count':1,
            'required_original_assertions':trace['original_assertions'],'required_original_assertion_count':len(trace['original_assertions']),
            'model':'Complete unchanged original SIMD body;independent unsigned lane algebra,modulo64 shifts,little-byte reinterpretation,all assertions and store traces. Baseline nativeBytes16;alternate16/32/64 prefix controls do not qualify targets. Legacy ref Slice write interface remains open;no product ABI/intrinsic/alignment/acceleration/borrow/resource/provider PASS.'}
        row['required_semantic_trace']={k:v for k,v in trace.items() if k not in {'stdout_utf8','original_assertions'}}
        rows.append(row)
    return rows
