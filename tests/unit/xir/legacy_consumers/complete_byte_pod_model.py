"""Complete byte-container and POD-view value/fault obligations.

The byte store is independent of Xray and native host memory. It preserves
every original print, mutation, overlap and fault; it does not establish a
legal exclusive borrow, allocation recovery, address lifetime or provider.
The old Slice mutation spelling still requires an atomic source migration.
"""
from pathlib import Path
import hashlib,math,struct
from independent_observation_format import render as scalar_render
ROOT=Path(__file__).resolve().parents[4]
BASE='tests/diff/cases/semantics/collections/'
TARGET='windows-x86_64'
_NAMES=('byte_array_append_repeat_safe_shared_core','byte_array_constructor_shared_core',
 'byte_array_range_arg_shared_core','byte_slice_bulk_ops_shared_core',
 'byte_slice_float_load_store_shared_core','byte_slice_load_store_endian_shared_core',
 'byte_slice_signed_load_store_shared_core','byte_uint8_specialization_shared_core',
 'span_compare_pod_shared_core','span_copyfrom_pod_shared_core',
 'span_fill_pod_shared_core','span_reinterpret_asbytes_shared_core')
_PROGRAMS={name:BASE+name+'.xr' for name in _NAMES}
_AUTHORITIES=('spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md','spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/015-14-built-in-type-methods.md','stdlib/types/array.xr',
 'src/base/xtarget_data_layout.h','src/base/xtarget_data_layout.c',
 'src/shared/xr_array_core.h','src/shared/xr_byte_array_append_core.h',
 'src/shared/xr_byte_array_repeat_core.h','src/shared/xr_byte_slice_scalar_core.h',
 'src/shared/xr_byte_compare_core.h','src/shared/xr_pod_slice_core.h',
 'src/shared/xr_error_messages.h','src/runtime/xerror_codes.h',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'src/base/xtarget_data_layout.c': '6bc2336985a1da1336af372011c2b9b85e162e5deb7eaea08b23d5b1d8448c95',
 'src/base/xtarget_data_layout.h': 'c3e557ea5be1e6c4e3927ce579075f05a9b79503df76bfb52410b679837eae0c',
 'src/runtime/xerror_codes.h': 'c57c353946012402931711b2b736013c8b5208fcb5de22370a230cc75e0f8a56',
 'src/shared/xr_array_core.h': '9b589270a1eadfc2fa94ab85e4192bae07af4a4e45f15dd27558954763091f66',
 'src/shared/xr_byte_array_append_core.h': 'bf365df86a3175dbb5db072e5816a27c23b8c99683135fe1eca1a0c57348de13',
 'src/shared/xr_byte_array_repeat_core.h': 'e81538aa330e15705f4174ca26009f5bd82b46255efad774e35312775f0da3f2',
 'src/shared/xr_byte_compare_core.h': '2f1f88ceae706a5ec7ffdcbf21de00d446f657c0ed9bea09c5f186ded16001c1',
 'src/shared/xr_byte_slice_scalar_core.h': '128483a67dd27f266eba40f9f5e21b4bfb69021da7ca15bdf6560bc7b990ba73',
 'src/shared/xr_error_messages.h': '9cb2a61e702eb0f768e4a9e7fb1a1a3d7176cc603dfb955ca1e2415e6e8c615c',
 'src/shared/xr_pod_slice_core.h': 'ab982f646593c93de92b09a3cc8eceddd10c5cd537481d08f10a9c91c618f1f6',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'tests/diff/cases/semantics/collections/byte_array_append_repeat_safe_shared_core.xr': 'c9f7d305d10ccf5d89daf12c1f48bf370571d09d65862a92542c25c1c8ea08e8',
 'tests/diff/cases/semantics/collections/byte_array_constructor_shared_core.xr': '524eb46f9dff2610a119f7f51e132020c07dd894e8e3868775e3d4b9ffb69634',
 'tests/diff/cases/semantics/collections/byte_array_range_arg_shared_core.xr': '15eb8e566fa4bf8c1c940df8bbf1d441fbb242efab4cc9765ac3d92fe624314c',
 'tests/diff/cases/semantics/collections/byte_slice_bulk_ops_shared_core.xr': '2e810fec2662c62b3edb8a68109e44eacd9b4832aaf7efb71d66a37a9aefe20a',
 'tests/diff/cases/semantics/collections/byte_slice_float_load_store_shared_core.xr': 'b050a62c73acba153e72a1b785d08947acab61140f10e7b496ca1246daf5e009',
 'tests/diff/cases/semantics/collections/byte_slice_load_store_endian_shared_core.xr': 'f0bf016f7ea22f8a9464ec5d77e6338d6e9cdc00b173c4f5e6174c478b3ccec7',
 'tests/diff/cases/semantics/collections/byte_slice_signed_load_store_shared_core.xr': '166159df2170574962c1314221480e715e97563d6b3d49f2909f1e07bfbd53e9',
 'tests/diff/cases/semantics/collections/byte_uint8_specialization_shared_core.xr': 'de380a8bb11f1ad4107a5eaebcf762bb79b8de8b24f6f4cf120f4ff93cd6230d',
 'tests/diff/cases/semantics/collections/span_compare_pod_shared_core.xr': 'e9d229e6ff96d64ae9caef01129b3f857eee174e8eb6e69d54b17adb57c7203c',
 'tests/diff/cases/semantics/collections/span_copyfrom_pod_shared_core.xr': 'abd92e8f536320a88854872c83afed7b8af3cc58c04a9a6d9a0887c683b33012',
 'tests/diff/cases/semantics/collections/span_fill_pod_shared_core.xr': 'f9f8e422e846c6ac1fbac467d40e99736b43db251424fe53b34abda04a77bf40',
 'tests/diff/cases/semantics/collections/span_reinterpret_asbytes_shared_core.xr': '84ad42893b32a488608cd0bdf44eacd02f8572d12d657783de9c05ef6968eb81',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}


def render(value):
    if type(value) is list:return '['+', '.join(scalar_render({'expected':x}) for x in value)+']'
    return scalar_render({'expected':value})


class Fault(ValueError):
    def __init__(self,message,code=430):super().__init__(message);self.code=code


class Bytes:
    def __init__(self,count=0,fill=0):self.data=bytearray([fill&255])*count
    def append(self,values):
        # A complete source snapshot survives grow/reallocation and aliasing.
        self.data.extend(bytes(values))
    def repeat(self,distance,count):
        if type(distance) is not int or type(count) is not int or distance<=0 or count<0 or distance>len(self.data):
            raise Fault('Array<u8>.repeatFrom(distance, count) range/grow failure')
        start=len(self.data)
        for i in range(count):self.data.append(self.data[start-distance+i])
    def view(self,start=0,count=None,width=1,*,mutable=True):
        count=(len(self.data)-start)//width if count is None else count
        return View(self,start,count,width,mutable=mutable)


class View:
    def __init__(self,owner,start,count,width,*,mutable=True):
        if start<0 or count<0 or width not in (1,2,4,8) or start+count*width>len(owner.data):
            raise ValueError('Invalid complete symbolic POD view')
        self.owner,self.start,self.count,self.width,self.mutable=owner,start,count,width,mutable
    def raw(self):return bytes(self.owner.data[self.start:self.start+self.count*self.width])
    def window(self,start,end):
        if not 0<=start<=end<=self.count:raise ValueError('Invalid original subwindow')
        return View(self.owner,self.start+start*self.width,end-start,self.width,mutable=self.mutable)
    def _write(self,start,data):
        if not self.mutable:raise ValueError('Shared view cannot write')
        if start<0 or start+len(data)>self.count*self.width:raise Fault('Symbolic write out of bounds')
        first=self.start+start;self.owner.data[first:first+len(data)]=data
    def set(self,index,value):
        if not 0<=index<self.count:raise Fault('Symbolic element out of bounds')
        self._write(index*self.width,(value%(1<<(8*self.width))).to_bytes(self.width,'little'))
    def get(self,index):
        if not 0<=index<self.count:raise Fault('Symbolic element out of bounds')
        return int.from_bytes(self.raw()[index*self.width:(index+1)*self.width],'little')
    def fill(self,value):
        encoded=(value%(1<<(8*self.width))).to_bytes(self.width,'little')
        self._write(0,encoded*self.count);return self
    def store(self,offset,width,value,endian='little',*,floating=False):
        if floating:data=struct.pack(('<' if endian=='little' else '>')+('f' if width==4 else 'd'),value)
        else:data=(value%(1<<(8*width))).to_bytes(width,endian)
        self._write(offset,data)
    def load(self,offset,width,endian='little',*,signed=False,floating=False):
        if type(offset) is not int or offset<0 or offset+width>self.count*self.width:
            raise Fault('Slice<u8>.load<u'+str(width*8)+'>() offset out of bounds')
        data=self.raw()[offset:offset+width]
        if floating:return struct.unpack(('<' if endian=='little' else '>')+('f' if width==4 else 'd'),data)[0]
        return int.from_bytes(data,endian,signed=signed)
    def copy_from(self,source):
        if self.width!=source.width:raise ValueError('POD element width mismatch')
        if source.count>self.count:
            raise Fault(('Slice<u8>' if self.width==1 else 'Slice')+'.copyFrom(src) range out of bounds')
        snapshot=source.raw();self._write(0,snapshot);return self.window(0,source.count)
    def compare(self,other):
        if self.width!=other.width:raise ValueError('POD element width mismatch')
        a,b=self.raw(),other.raw();return (a>b)-(a<b)
    def prefix(self,other):
        count=0
        for a,b in zip(self.raw(),other.raw()):
            if a!=b:break
            count+=1
        return count
    def repeat(self,offset,distance,count):
        if offset<0 or distance<=0 or distance>offset or count<0 or offset+count>self.count:
            raise Fault('Slice<u8>.repeatFrom(dstOffset, distance, count) range out of bounds')
        for i in range(count):self.set(offset+i,self.get(offset-distance+i))
    def as_bytes(self):return View(self.owner,self.start,self.count*self.width,1,mutable=self.mutable)
    def reinterpret(self,width):
        byte_count=self.count*self.width
        if byte_count%width:raise Fault('Slice<u8>.reinterpret<T>() length is not divisible by target size')
        # The positive original buffers have ordinary aligned allocation; offsets
        # carry the only relative alignment premise. No host address is queried.
        if self.start%width:raise Fault('Slice<u8>.reinterpret<T>() source address is not aligned for target type',404)
        return View(self.owner,self.start,byte_count//width,width,mutable=self.mutable)


def byte(value):
    if type(value) is bool:return int(value)
    if type(value) is int:return value%256
    if type(value) is float and math.isfinite(value):return math.trunc(value)%256
    return 0


def pod(values,width):
    owner=Bytes(len(values)*width);view=owner.view(width=width)
    for i,value in enumerate(values):view.set(i,value)
    return view


def caught(action):
    try:action()
    except Fault as error:return [str(error),error.code]
    raise ValueError('Required complete original fault did not occur')


def observations():
    traces={}
    source=bytes(range(65,70));out=Bytes();out.append(source[1:4])
    values=[len(out.data),render(list(out.data))];out.repeat(3,6);values += [len(out.data),render(list(out.data))]
    alias=Bytes();alias.append(b'XY');alias.append(alias.view().raw())
    values += [len(alias.data),render(list(alias.data))]
    error=caught(lambda:out.repeat(0,1));values += [error[1],'range/grow' in error[0]]
    traces[_NAMES[0]]=values
    ints=[257,-1,0,128];flags=[True,False,True];floats=[3.8,-2.2,260.0];mixed=[1,True,'x',258]
    traces[_NAMES[1]]=[len(ints),*map(byte,ints),*map(byte,flags),*map(byte,floats),*map(byte,mixed),byte(257),byte(257)]
    view=Bytes(8,1).view();error=caught(lambda:view.load(4294967296,4))
    traces[_NAMES[2]]=['before-range'];assert error[1]==430

    dst,source=Bytes(8,1),Bytes(4,7);dv,sv=dst.view(),source.view();sv.store(1,2,2312)
    copied=dv.window(2,6).copy_from(sv);middle,head=dv.window(2,6),dv.window(0,4);prefix=dv.window(2,5)
    values=[*dst.data,copied.count,middle.compare(sv),head.compare(sv),sv.compare(head),prefix.compare(sv),sv.compare(prefix)]
    values += [middle.prefix(sv),prefix.prefix(sv),head.prefix(sv),dv.window(0,0).prefix(sv)]
    repeat=Bytes(10).view();repeat.store(0,2,16961);repeat.repeat(2,2,6);values += list(repeat.raw()[:8])
    traces[_NAMES[3]]=values
    data=Bytes(16);v=data.view();v.store(0,4,1.5,floating=True);v.store(4,4,-2.5,'big',floating=True);v.store(8,8,-2.25,floating=True)
    traces[_NAMES[4]]=[*[data.data[i] for i in [*range(8),14,15]],v.load(0,4,floating=True),v.load(4,4,'big',floating=True),v.load(8,8,floating=True)]
    data=Bytes(12);v=data.view();v.store(0,2,4660);v.store(2,2,4660,'big');v.store(4,4,16909060,'big');v.store(8,2,2569)
    traces[_NAMES[5]]=[*[data.data[i] for i in [0,1,2,3,4,7]],v.load(0,2),v.load(2,2,'big'),v.load(4,4,'big'),v.load(8,2)]
    data=Bytes(24);v=data.view();v.store(0,2,-2);v.store(2,2,-3,'big');v.store(4,4,-12345678);v.store(8,4,-16909060,'big');v.store(16,8,-1)
    traces[_NAMES[6]]=[*data.data[:4],v.load(0,2,signed=True),v.load(2,2,'big',signed=True),v.load(4,4,signed=True),v.load(8,4,'big',signed=True),v.load(16,8,signed=True),v.load(0,2)]
    data=Bytes(4,1);source=Bytes(2,7);data.append(source.data);data.repeat(2,2);v=data.view();v.store(0,2,513)
    traces[_NAMES[7]]=[len(data.data),v.load(0,2),*[data.data[i] for i in [0,1,4,5,6,7]]]
    a,b,c,d=pod([1,2],4),pod([1,2],4),pod([1,3],4),pod([1],4);w=pod([9,7,8,6],2)
    traces[_NAMES[8]]=[a.compare(b),a.compare(c),c.compare(a),a.compare(d),d.compare(a),w.window(1,3).compare(w.window(2,4))]
    dst,source=pod([0]*4,4),pod([17,42],4);copied=dst.window(1,3).copy_from(source)
    overlap=pod([1,2,3,4],2);overlap.window(1,4).copy_from(overlap.window(0,3))
    traces[_NAMES[9]]=[*[dst.get(i) for i in range(4)],copied.count,*[overlap.get(i) for i in range(4)],*caught(lambda:dst.window(0,1).copy_from(source))]
    words=pod([0]*4,4);filled=words.window(1,3).fill(9);flags=pod([0]*3,1);flags.fill(1);chars=pod([ord('a')]*3,4);chars.fill(ord('z'));zeros=pod([7]*3,2);zeros.fill(0)
    traces[_NAMES[10]]=[filled.count,*[words.get(i) for i in range(4)],*[bool(flags.get(i)) for i in range(3)],''.join(chr(chars.get(i)) for i in range(3)),sum(zeros.get(i) for i in range(3))]
    words=pod([0,0],4);raw=words.as_bytes();raw.fill(1);values=[raw.count,words.get(0),words.get(1)]
    raw=Bytes(6).view();pairs=raw.reinterpret(2);values.append(pairs.count);pairs.set(1,30583);values += [raw.get(2),raw.get(3)]
    roundtrip=pairs.as_bytes();values.append(roundtrip.count);roundtrip.fill(9);values += [pairs.get(0),pairs.get(2)]
    values += caught(lambda:raw.window(0,5).reinterpret(4))+caught(lambda:raw.window(1,5).reinterpret(4))
    traces[_NAMES[11]]=values
    if set(traces)!=set(_NAMES):raise ValueError('Complete byte/POD trace lost an original program')
    return traces


def _validate():
    required=set(_AUTHORITIES)|set(_PROGRAMS.values())
    if set(_INPUTS)!=required or len(_PROGRAMS)!=12:raise ValueError('Complete byte/POD input coverage changed')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete byte/POD original or authority changed: '+path)


def expected_rows():
    _validate();traces=observations();rows=[]
    for name in _NAMES:
        termination=(1,'[Uncaught Panic] E0430: Slice<u8>.load<u32>() offset out of bounds\n') if name==_NAMES[2] else (0,'')
        rows.append({'id':'complete_byte_pod_'+name,'source':_PROGRAMS[name],'required_target':TARGET,
            'expected':{'exit_code':termination[0],'stdout_utf8':''.join(render(x)+'\n' for x in traces[name]),'stderr_utf8':termination[1]},
            'observation_count':len(traces[name]),'required_semantic_trace':traces[name],
            'model':'Complete original byte/POD values and exact termination;independent byte store,IEEE scalar representation,unsigned lexicographic bytes,memmove snapshot,live repetition and relative view alignment. Array<u8>.toString uses current container format. Old Slice mutation/borrow/call admission and all original FI/resource/lifetime/physical/provider duties remain OPEN;no source port or guessed API.'})
    return rows
