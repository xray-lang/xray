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


def observations():
    return {'mem_bulk_shared_core':bulk_trace(),'mem_from_address_shared_core':address_trace(),
            'mem_layout_introspection':layout_trace(),'mem_load_store_scalar_shared_core':scalar_trace(),
            'mem_volatile_shared_core':volatile_trace()}


def expected_rows():
    if (set(_PROGRAMS)!=_NAMES or any(path!=BASE+name+'.xr' for name,path in _PROGRAMS.items()) or
            set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()) or
            (TARGET,NATIVE_ENDIAN,POINTER_BYTES,CACHE_LINE_BYTES)!=('windows-x86_64','little',8,64)):
        raise ValueError('Memory model lost an exact complete program, authority or target premise')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Memory program or normative input changed: '+path)
    traces=observations()
    if set(traces)!=_NAMES or sum(map(len,traces.values()))!=82:raise ValueError('Memory model lost complete original observations')
    return [{'id':'complete_memory_primitives_'+name,'source':path,'original_source':path,'required_target':TARGET,
             'expected':{'exit_code':0,'stdout_utf8':''.join(render({'expected':value})+'\n' for value in traces[name]),'stderr_utf8':''},
             'observation_count':len(traces[name]),
             'model':'Complete unchanged original normal allocation path;independent initialized bytes, symbolic typed pointer identity, explicit x64 little-endian scalar/layout facts and bulk policy. No host query or product-output oracle;all unsafe/volatile/resource/lifetime/provider duties remain open.'}
            for name,path in sorted(_PROGRAMS.items())]
