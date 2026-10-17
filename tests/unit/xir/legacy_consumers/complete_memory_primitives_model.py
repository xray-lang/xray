"""Derive full raw-memory consumer traces from explicit target facts and bytes.

Only the successful original allocation path is modelled. Symbolic addresses
stand for fresh aligned regions; no host address, ABI query, Xray execution or
compiler output supplies an expected value. These observations do not qualify
allocation failure, volatile ordering, unsafe lifetime, reclamation or providers.
"""
from dataclasses import dataclass
from pathlib import Path
import hashlib
import struct

from complete_ffi_memory_model import aggregate
from independent_observation_format import render

ROOT=Path(__file__).resolve().parents[4]
BASE='tests/diff/cases/semantics/stdlib/'
TARGET='windows-x86_64'
NATIVE_ENDIAN='little'
POINTER_BYTES=8
CACHE_LINE_BYTES=64
_NAMES=frozenset({'mem_bulk_shared_core','mem_from_address_shared_core',
    'mem_layout_introspection','mem_load_store_scalar_shared_core','mem_volatile_shared_core'})
_PROGRAMS={name:BASE+name+'.xr' for name in _NAMES}
_AUTHORITIES=(
    'spec/source/sections/003-2-type-system.md',
    'spec/source/sections/004-3-expressions.md',
    'spec/source/sections/005-4-statements.md',
    'spec/source/sections/006-5-declarations.md',
    'spec/source/sections/015-14-built-in-type-methods.md',
    'src/base/xtarget_data_layout.h','src/base/xtarget_data_layout.c',
    'src/base/xplatform.h','stdlib/mem/mem.xr',
    'tests/unit/xir/legacy_consumers/complete_ffi_memory_model.py',
    'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'src/base/xplatform.h': 'b7fd7125613bdf64abe486effc2b08a7ab87685a1f605d21b0f51da896267fe9',
 'src/base/xtarget_data_layout.c': '6bc2336985a1da1336af372011c2b9b85e162e5deb7eaea08b23d5b1d8448c95',
 'src/base/xtarget_data_layout.h': 'c3e557ea5be1e6c4e3927ce579075f05a9b79503df76bfb52410b679837eae0c',
 'stdlib/mem/mem.xr': '9750e120d6b22f5c2caffd2d59d13238393ac6939c17827090a3829c33fa9dab',
 'tests/diff/cases/semantics/stdlib/mem_bulk_shared_core.xr': 'fa16eb57725849a904a8e4b624c8da02e10216fa91ab35b16fd252e4d0deb830',
 'tests/diff/cases/semantics/stdlib/mem_from_address_shared_core.xr': 'd299371e08a6bf0b2548c281f99272f3da767d47e7c5b85345f2b6f31a90f54d',
 'tests/diff/cases/semantics/stdlib/mem_layout_introspection.xr': 'bbd4969eee7a9a93e601e0481787900fb6668f572b96aca36ccb6e5747bce504',
 'tests/diff/cases/semantics/stdlib/mem_load_store_scalar_shared_core.xr': '2991106a9d42d9c106a55955ac54c5c0df1709f41bbc1a0505d2629feb5fd490',
 'tests/diff/cases/semantics/stdlib/mem_volatile_shared_core.xr': 'd444663d2e12faea5581ffa7704b3b7ec0f4bef5a72896f1bcda012ec93629c4',
 'tests/unit/xir/legacy_consumers/complete_ffi_memory_model.py': 'd7b0623954a9631406a5190c4353bba37db96359bba9808c2a9b82e5cc629b95',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}

_ORIGINAL_NAMES=_NAMES
_LAYOUT_PROGRAMS={
 "repr_c_struct": "tests/diff/cases/semantics/oop/repr_c_struct.xr",
 "repr_packed_aligned_struct_aggregate_param": "tests/diff/cases/semantics/oop/repr_packed_aligned_struct_aggregate_param.xr",
 "union_fixed_array_overlay": "tests/diff/cases/semantics/oop/union_fixed_array_overlay.xr",
 "union_fixed_layout_overlay": "tests/diff/cases/semantics/oop/union_fixed_layout_overlay.xr",
 "fixed_width_type_tests": "tests/diff/cases/semantics/types/fixed_width_type_tests.xr"
}
_NAMES=_NAMES|frozenset(_LAYOUT_PROGRAMS)
_PROGRAMS.update(_LAYOUT_PROGRAMS)
_INPUTS.update({
 "tests/diff/cases/semantics/oop/repr_c_struct.xr": "6cfaab1ce4b27dfc4b0114c37a7c033d6c688104af265f1f3cbf05c60bce7fc7",
 "tests/diff/cases/semantics/oop/repr_packed_aligned_struct_aggregate_param.xr": "16a2b48950ab5f89131f9584b8d239e58eb1294a1cc121503fc80a926adc9fc0",
 "tests/diff/cases/semantics/oop/union_fixed_array_overlay.xr": "33677c6d42770d0c36a43088f40a104d7232102212e73bebb328649c35b7143e",
 "tests/diff/cases/semantics/oop/union_fixed_layout_overlay.xr": "13ec0936c0bbd633da9594b21f42dc6b0160cedd040e4ce11c23acd7af1d2e95",
 "tests/diff/cases/semantics/types/fixed_width_type_tests.xr": "85d27bce99b5df61685c2afeed5157aacc5692c9583ac8489752ae675a206ca6"
})


class Region:
    def __init__(self,length,*,zeroed=False,page_aligned=False):
        if length<0:raise ValueError('Negative region length')
        self.bytes=bytearray(length);self.initialized=[zeroed]*length
        self.page_aligned=page_aligned;self.freed=False

    def span(self,offset,width):
        if self.freed or offset<0 or width<0 or offset+width>len(self.bytes):
            raise ValueError('Invalid live symbolic region span')
        return slice(offset,offset+width)

    def write(self,offset,data):
        span=self.span(offset,len(data));self.bytes[span]=data
        self.initialized[span]=[True]*len(data)

    def read(self,offset,width):
        span=self.span(offset,width)
        if not all(self.initialized[span]):raise ValueError('Read of uninitialized symbolic bytes')
        return bytes(self.bytes[span])

    def store_int(self,offset,width,value,*,signed=False,endian=NATIVE_ENDIAN):
        self.write(offset,value.to_bytes(width,endian,signed=signed))

    def load_int(self,offset,width,*,signed=False,endian=NATIVE_ENDIAN):
        return int.from_bytes(self.read(offset,width),endian,signed=signed)

    def store_float(self,offset,width,value,endian):
        if width not in (4,8) or endian not in ('little','big'):raise ValueError('Invalid IEEE leaf')
        self.write(offset,struct.pack(('<' if endian=='little' else '>')+('f' if width==4 else 'd'),value))

    def load_float(self,offset,width,endian):
        if width not in (4,8) or endian not in ('little','big'):raise ValueError('Invalid IEEE leaf')
        return struct.unpack(('<' if endian=='little' else '>')+('f' if width==4 else 'd'),self.read(offset,width))[0]


@dataclass(frozen=True)
class Address:
    region: Region | None
    offset: int=0

    def add(self,count):return Address(self.region,self.offset+count)

    def alignment_remainder(self,alignment):
        if alignment!=4096 or self.region is None or not self.region.page_aligned:
            raise ValueError('No independent page-alignment premise')
        return self.offset%alignment


@dataclass(frozen=True)
class Pointer:
    address: Address
    pointee: str='u8'
    mutable: bool=False

    def is_null(self):return self.address.region is None and self.address.offset==0

    def read(self,width):
        if self.address.region is None:raise ValueError('Positive read through null')
        return self.address.region.read(self.address.offset,width)

    def write(self,data):
        if not self.mutable or self.address.region is None:raise ValueError('Write requires a live mutable pointer')
        self.address.region.write(self.address.offset,data)

    def offset(self,count):return Pointer(self.address.add(count),self.pointee,self.mutable)


class PointerSlots:
    """Serialize opaque symbolic addresses at the explicit target pointer width."""
    def __init__(self):self.regions={}

    def store(self,memory,offset,pointer):
        address=pointer.address;word=0
        if not pointer.is_null():
            if address.region is None or address.region.freed or not 0<=address.offset<len(address.region.bytes):
                raise ValueError('Pointer slot requires a known live symbolic address')
            if address.region not in self.regions:
                if len(address.region.bytes)>=1<<20:raise ValueError('Symbolic token window exhausted')
                self.regions[address.region]=(len(self.regions)+1)<<20
            word=self.regions[address.region]+address.offset
        memory.store_int(offset,POINTER_BYTES,word)

    def load(self,memory,offset,*,mutable=False):
        word=memory.load_int(offset,POINTER_BYTES)
        if word==0:return Pointer(Address(None),mutable=mutable)
        for region,base in self.regions.items():
            if base<=word<base+len(region.bytes) and not region.freed:
                return Pointer(Address(region,word-base),mutable=mutable)
        raise ValueError('Pointer slot contains no registered live symbolic address')


def bulk_copy(destination,source,count,*,overlap=False):
    if count<=0:return
    if destination is None or source is None:raise ValueError('Positive copy requires both pointers')
    first,last=destination.address,source.address
    if (not overlap and first.region is last.region and
            max(first.offset,last.offset)<min(first.offset+count,last.offset+count)):
        raise ValueError('Non-overlapping copy cannot accept overlapping ranges')
    data=source.read(count)
    destination.write(data)


def bulk_set(destination,value,count):
    if count<=0:return
    if destination is None:raise ValueError('Positive fill requires a pointer')
    destination.write(bytes([value&255])*count)


def bulk_compare(first,second,count):
    if count<=0:return 0
    if first is None or second is None:raise ValueError('Positive comparison requires both pointers')
    a,b=first.read(count),second.read(count)
    return (a>b)-(a<b)


def volatile_store(pointer,value,width):
    if width in (1,2,4,8):pointer.write((value%(1<<(8*width))).to_bytes(width,NATIVE_ENDIAN))


def volatile_load(pointer,width):
    return int.from_bytes(pointer.read(width),NATIVE_ENDIAN,signed=width==8) if width in (1,2,4,8) else 0


def layout_trace():
    scalars=[(1,1),(2,2),(4,4),(8,8),(4,4),(8,8),(POINTER_BYTES,POINTER_BYTES),(6,2)]
    result=[value for pair in scalars for value in pair]
    header,offsets=aggregate([(1,1),(2,2),(1,1),(4,4)])
    result.extend([*header,*offsets])
    packed,offsets=aggregate([(1,1),(4,4),(2,2)],packed=True)
    result.extend([*packed,*offsets])
    inner,_=aggregate([(2,2),(1,1)]);result.extend(inner)
    outer,offsets=aggregate([header,inner,(1,1)]);result.extend([*outer,*offsets])
    return result


def bulk_trace():
    src=Pointer(Address(Region(8)),mutable=True);dst=Pointer(Address(Region(8)),mutable=True)
    result=[CACHE_LINE_BYTES];bulk_set(src,7,8);result.extend([src.read(8)[0],src.read(8)[7]])
    bulk_copy(dst,src,8);result.extend([dst.read(8)[0],dst.read(8)[5],bulk_compare(src,dst,8)])
    dst.write(b'\x09');result.append(bulk_compare(src,dst,8)!=0)
    bulk_copy(None,None,-1);bulk_copy(None,None,0,overlap=True);bulk_set(None,255,-1)
    result.append(bulk_compare(None,None,-1))
    bulk_set(dst,1,8);dst.write(b'\x05\x06');bulk_copy(dst.offset(2),dst,4,overlap=True)
    result.extend([dst.read(8)[2],dst.read(8)[3],'ok'])
    src.address.region.freed=dst.address.region.freed=True
    return result


def address_trace():
    null=Pointer(Address(None));result=[null.is_null(),0]
    region=Region(4096,page_aligned=True);p=Pointer(Address(region),mutable=True)
    p.write(b'\x2a');p.offset(15).write(b'\x63');address=p.address
    q=Pointer(address);result.extend([q.is_null(),q.read(1)[0],q.offset(15).read(1)[0]])
    q15=Pointer(address.add(15));result.extend([q15.read(1)[0],q.address==address])
    q32=Pointer(address,'u32',True);q32.write((16909060).to_bytes(4,NATIVE_ENDIAN))
    result.append(region.load_int(0,4,endian='little'))
    for pointee in ['Ptr<u8>','f32','f64']:result.append(Pointer(address,pointee).address==address)
    result.extend([0,0,address.alignment_remainder(4096)])
    reg=Pointer(address.add(8),mutable=True);volatile_store(reg,305419896,4)
    result.append(volatile_load(reg,4));region.freed=True;result.extend([True,'ok'])
    return result


def scalar_trace():
    memory=Region(96,zeroed=True);result=[]
    leaves=[(0,1,-2,True,'little'),(1,1,250,False,'little'),(2,2,-1234,True,'little'),
            (4,2,4660,False,'big'),(8,4,-12345678,True,'little'),(12,4,16909060,False,'big'),
            (16,8,-5,True,'little'),(24,8,72623859790382856,False,'big')]
    for offset,width,value,signed,endian in leaves:memory.store_int(offset,width,value,signed=signed,endian=endian)
    memory.store_float(32,4,1.5,'little');memory.store_float(40,8,-2.25,'big')
    for offset,width,_,signed,endian in leaves:result.append(memory.load_int(offset,width,signed=signed,endian=endian))
    result.extend([memory.load_float(32,4,'little'),memory.load_float(40,8,'big')])
    slots=PointerSlots();q=Pointer(Address(memory));p=Pointer(Address(memory),mutable=True)
    slots.store(memory,56,q);slots.store(memory,64,p)
    result.extend([slots.load(memory,56)==q,slots.load(memory,64,mutable=True)==p])
    for offset,endian in [(72,'little'),(76,'big')]:
        memory.store_int(offset,4,270544960,endian=endian);result.append(memory.load_int(offset,4,endian=endian))
    return result


def volatile_trace():
    memory=Region(16);p=Pointer(Address(memory),mutable=True);bulk_set(p,0,16)
    volatile_store(p,305419896,4);result=[volatile_load(p,4),volatile_load(p,1)]
    for value,width in [(255,1),(43981,2),(72623859790382856,8)]:
        volatile_store(p,value,width);result.append(volatile_load(p,width))
    memory.freed=True;result.append('ok');return result


def aligned_aggregate(fields,*,packed=False,alignment=0):
    if type(alignment) is not int or alignment<0 or alignment and alignment&(alignment-1):raise ValueError('Invalid explicit alignment')
    (size,natural),offsets=aggregate(fields,packed=packed)
    align=max(natural,alignment)
    return ((size+align-1)//align*align,align),offsets


class RecordValue:
    """Logical values for the selected scalar and nested-record declarations."""
    def __init__(self,declaration,fields):
        import copy
        self.declaration,self.fields=declaration,copy.deepcopy(fields)
    def copy(self):return RecordValue(self.declaration,self.fields)
    def __eq__(self,other):return self is other


class UnionValue:
    """Explicit fixed bytes; reading an inactive member requires unsafe access."""
    def __init__(self,members,active,value):
        self.members=dict(members);self.active=None
        size=max(width for width,_,_ in self.members.values());self.memory=Region(size)
        self.store(active,value)
    def store(self,name,value):
        width,signed,array=self.members[name]
        if array:
            if len(value)!=width or any(type(x) is not int or not 0<=x<=255 for x in value):raise ValueError('Invalid fixed byte array')
            self.memory.write(0,bytes(value))
        else:
            if type(value) is not int:raise ValueError('Integer union member requires its exact scalar family')
            self.memory.store_int(0,width,value,signed=signed)
        self.active=name
    def read(self,name,*,unsafe=False):
        if name!=self.active and not unsafe:raise ValueError('Inactive union access requires unsafe')
        width,signed,array=self.members[name]
        # A fixed array read returns a logical copy, even from shared union bytes.
        return list(self.memory.read(0,width)) if array else self.memory.load_int(0,width,signed=signed)


def exactly_representable(value,target):
    integers={'i8':(8,True),'u8':(8,False),'i16':(16,True),'u16':(16,False),'i32':(32,True),'u32':(32,False),'i64':(64,True),'u64':(64,False)}
    if target in integers:
        bits,signed=integers[target];low=-(1<<(bits-1)) if signed else 0;high=(1<<(bits-int(signed)))-1
        return type(value) is int and low<=value<=high
    if target not in {'f32','f64'}:raise ValueError('Unknown fixed numeric type')
    if type(value) is not float:return False
    import math
    if not math.isfinite(value):raise ValueError('Nonfinite values are outside the selected complete program')
    if target=='f64':return True
    try:return struct.unpack('<f',struct.pack('<f',value))[0]==value
    except OverflowError:return False


def checked_optional_cast(value,target):return value if exactly_representable(value,target) else None


def record_value_trace():
    def make_point():return RecordValue('CPoint',{'x':10,'y':20})
    p=make_point();p.fields['x']+=2;q=RecordValue('CPoint',{'x':12,'y':20})
    out=[p.fields['x'],p.fields['y'],p==q]
    b=RecordValue('CBox',{'p':RecordValue('CPoint',{'x':1,'y':2}),'tag':3});b.fields['p'].fields['x']+=b.fields['tag']
    return out+[b.fields['p'].fields['x'],b.fields['p'].fields['y'],b.fields['tag']]


def aggregate_parameter_trace():
    calls=[]
    def make_packed(tag,value):calls.append('make_packed');return RecordValue('PackedPair',{'tag':tag,'value':value})
    def make_aligned(value):calls.append('make_aligned');return RecordValue('AlignedWord',{'value':value})
    def make_packed_aligned(tag,value):calls.append('make_packed_aligned');return RecordValue('PackedAligned',{'tag':tag,'value':value})
    def sum_packed(value):calls.append('sum_packed');p=value.copy();return p.fields['tag']+p.fields['value']
    def read_aligned(value):calls.append('read_aligned');p=value.copy();return p.fields['value']
    def sum_packed_aligned(value):calls.append('sum_packed_aligned');p=value.copy();return p.fields['tag']+p.fields['value']
    p=make_packed(5,37);a=make_aligned(42);pa=make_packed_aligned(6,36)
    out=[sum_packed(p),read_aligned(a),sum_packed_aligned(pa)]
    packed,po=aligned_aggregate([(1,1),(4,4)],packed=True)
    aligned,_=aligned_aggregate([(4,4)],alignment=16)
    packed_aligned,pao=aligned_aggregate([(1,1),(4,4)],packed=True,alignment=16)
    return out+[*packed,po[1],*aligned,*packed_aligned,pao[1]],calls


def erased_numeric_trace():
    text=lambda value:'null' if value is None else render({'expected':value})
    is_=lambda value,target:text(exactly_representable(value,target))
    cast=lambda value,target:text(checked_optional_cast(value,target))
    small,big,negative,exact,inexact,string=7,3000000000,-5,1.5,0.1,'s'
    return ['7 is i32='+is_(small,'i32')+' is i8='+is_(small,'i8')+' is u8='+is_(small,'u8'),
     '7 as i32='+cast(small,'i32'),'7 as i8?='+cast(small,'i8'),
     '3e9 is i32='+is_(big,'i32')+' is i64='+is_(big,'i64'),'3e9 as i32?='+cast(big,'i32'),
     '-5 is u8='+is_(negative,'u8')+' is i16='+is_(negative,'i16'),
     '1.5 is f32='+is_(exact,'f32')+' is f64='+is_(exact,'f64'),'1.5 as f32?='+cast(exact,'f32'),
     '0.1 is f32='+is_(inexact,'f32')+' is f64='+is_(inexact,'f64'),'0.1 as f32?='+cast(inexact,'f32'),
     '"s" is i32='+is_(string,'i32')+' is f32='+is_(string,'f32')]


def fixed_layout_observations():
    bits=UnionValue({'i':(4,True,False),'u':(4,False,False)},'i',-1)
    unsigned=bits.read('u',unsafe=True);bits.store('u',0x7fffffff)
    x=UnionValue({'u':(4,False,False),'b':(4,False,True)},'u',0x01020304)
    copied=x.read('b',unsafe=True);bytes_out=list(copied);copied[0]=255
    rows={'repr_c_struct':record_value_trace(),'repr_packed_aligned_struct_aggregate_param':aggregate_parameter_trace()[0],
     'union_fixed_layout_overlay':[unsigned,bits.read('i',unsafe=True)],'union_fixed_array_overlay':bytes_out+[x.read('u',unsafe=True)],
     'fixed_width_type_tests':erased_numeric_trace()}
    if set(rows)!=set(_LAYOUT_PROGRAMS) or sum(map(len,rows.values()))!=35:raise ValueError('Lost whole layout/numeric observations')
    return rows


def observations():
    rows={'mem_bulk_shared_core':bulk_trace(),'mem_from_address_shared_core':address_trace(),
            'mem_layout_introspection':layout_trace(),'mem_load_store_scalar_shared_core':scalar_trace(),
            'mem_volatile_shared_core':volatile_trace()}
    rows.update(fixed_layout_observations());return rows


def expected_rows():
    if (set(_PROGRAMS)!=_NAMES or any(path!=BASE+name+'.xr' for name,path in _PROGRAMS.items() if name in _ORIGINAL_NAMES) or
            any(_PROGRAMS.get(name)!=path for name,path in _LAYOUT_PROGRAMS.items()) or
            set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()) or
            (TARGET,NATIVE_ENDIAN,POINTER_BYTES,CACHE_LINE_BYTES)!=('windows-x86_64','little',8,64)):
        raise ValueError('Memory model lost an exact complete program, authority or target premise')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Memory program or normative input changed: '+path)
    traces=observations()
    if set(traces)!=_NAMES or sum(map(len,traces.values()))!=117:raise ValueError('Memory model lost complete original observations')
    rows=[{'id':'complete_memory_primitives_'+name,'source':path,'original_source':path,'required_target':TARGET,
             'expected':{'exit_code':0,'stdout_utf8':''.join(render({'expected':value})+'\n' for value in traces[name]),'stderr_utf8':''},
             'observation_count':len(traces[name]),
             'model':'Complete unchanged original normal allocation path;independent initialized bytes, symbolic typed pointer identity, explicit x64 little-endian scalar/layout facts and bulk policy. No host query or product-output oracle;all unsafe/volatile/resource/lifetime/provider duties remain open.'}
            for name,path in sorted(_PROGRAMS.items())]
    for row in rows:
        if row['original_source'] in _LAYOUT_PROGRAMS.values():
            row['model']='Complete unchanged original;independent nominal scalar/nested struct values,by-value aggregate calls,explicit packed/aligned x64 layout,unsafe fixed union bytes and fixed-array logical copies,erased numeric exact representability and optional casts. No product ABI,physical ownership,lifetime,FI,safety or provider qualification.'
    return rows+[unaligned_expected_row()]

_UNALIGNED_SOURCE = 'tests/diff/cases/semantics/stdlib/mem_load_store_unaligned_shared_core.xr'
_UNALIGNED_ID = 'complete_memory_primitives_mem_load_store_unaligned_shared_core'
_UNALIGNED_INPUTS = {'tests/diff/cases/semantics/stdlib/mem_load_store_unaligned_shared_core.xr': 'e0490f8b8648ebdbbadd22087143c1b443b6b1cb9b5bd2aa89031d4aec5fd9e0', 'stdlib/mem/mem.xr': '9750e120d6b22f5c2caffd2d59d13238393ac6939c17827090a3829c33fa9dab', 'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677', 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380', 'src/frontend/analyzer/xa_intrinsic_registry.def': 'b82d2d08038fe71e60bda2f3d9f88b1bb7711be72b28f11a8d15eb635c7ccc4b'}


def unaligned_memory_trace(region_class=Region):
    memory=region_class(16,zeroed=True)
    events=[]
    stores=((1,2,0x1234),(4,4,0x01020304),(8,8,0x0102030405060708))
    for offset,width,value in stores:
        memory.store_int(offset,width,value,endian='little')
        events.append(('store',offset,width,value,'LE'))
    outputs=[]
    for offset,width,value in stores:
        outputs.append(memory.load_int(offset,width,endian='little'))
        events.append(('load',offset,width,'LE'))
    for offset in (1,2,4,5,6,7,8,15):
        outputs.append(memory.read(offset,1)[0])
        events.append(('array_read',offset))
    return {'observations':outputs,'final_array_bytes':list(memory.read(0,16)),
            'events':events,'allocation_length':16,'same_slice_and_pointer_backing':True,
            'original_store_calls':3,'original_load_calls':3,'original_byte_observations':8,
            'unaligned_first_access_offset':1,'explicit_byte_order':'LE',
            'host_endian_address_alignment_orABI_queries':0,
            'physical_borrow_lifetime_FI_safety_orprovider_qualified':False}


def unaligned_expected_row():
    required={_UNALIGNED_SOURCE,'stdlib/mem/mem.xr','spec/source/sections/003-2-type-system.md',
              'spec/source/sections/014-13-built-in-functions.md',
              'src/frontend/analyzer/xa_intrinsic_registry.def'}
    if set(_UNALIGNED_INPUTS)!=required or _UNALIGNED_SOURCE!=BASE+'mem_load_store_unaligned_shared_core.xr':
        raise ValueError('Whole unaligned memory original or authorities changed')
    for name,digest in _UNALIGNED_INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Whole unaligned memory input changed: '+name)
    trace=unaligned_memory_trace()
    if len(trace['observations'])!=11:raise ValueError('Whole unaligned memory observations lost')
    return {'id':_UNALIGNED_ID,'source':_UNALIGNED_SOURCE,'original_source':_UNALIGNED_SOURCE,
            'required_target':None,'expected':{'exit_code':0,'stderr_utf8':'',
                'stdout_utf8':''.join(str(x)+chr(10) for x in trace['observations'])},
            'observation_count':11,'required_semantic_trace':trace,
            'model':'Complete unchanged 16-byte array/Slice/MutPtr original;all three unsafe stores and three loads with explicit LE,including offset1 u16,then all eight original backing-array byte reads. Independent initialized shared-region bytes yield all11outputs without host address,ABI,provider orXray queries. Logical alias andbyte semantics do not qualify real borrow lifetime,alignment fault handling,physical ownership,FI,resources,safety,providers or mixed execution.'}
