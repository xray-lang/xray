"""Whole collection programs under logical copy semantics, not physical ownership proof."""
from pathlib import Path
import copy,hashlib,math

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/010-9-generics.md': '246757d531f91e2d19d2cd18353b5ec8addf05d309e5489c10da37e48be81226',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': 'db71b9a6b69133a2f2254f6d9abb4d6ed425d15af79af0428f176c946c835c6f',
 'spec/source/sections/019-18-error-codes.md': '2c4416085d5d38f37fb0e434d863cba1ff5d5758d44efb29402fa5158282d9ad',
 'src/shared/xr_float_fmt.h': 'dc3f35e72dd96729e5948f98b8103810d1362bad8cec207aabf93db1352f983b',
 'src/shared/xr_sort_core.h': '12d05da0942da491ae99d49729a27f0dfc71e978f0d3992666624fc150d10cdf',
 'src/shared/xr_value_format_core.h': 'ff804a32605d092bcf7c931cc16fe5c6c3b17ff5af8253d85afbe0d0077b3a06',
 'stdlib/math/math.xr': '0126a1eb1db1760b5950da3f6be0c47f504f65070afaff3757f7b45f27a86115',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/f64.xr': '16f8b34079a8ef926027abf5a73c12e2e5601e692c027db00aca622a850cf16c',
 'stdlib/types/i64.xr': '2d9a160b70394a77ec69dccdf7b895ecd5ac7e66548824a835d3eb0a7cd45567',
 'stdlib/types/iterator.xr': '251d08d069d868e8487cae10f92e3ead7b601262a1bbc1d3989fb5871c2c7f94',
 'stdlib/types/map.xr': '49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31',
 'stdlib/types/set.xr': '6c8062693701d60982f550ba8b766b4c20924c9ed36cb0dc146be45f9693db8f',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'tests/diff/cases/semantics/collections/array_join_shared_core.xr': '2ec0e799fc041961cb7df59c2bb28ec2605cc8b774def530e3b6af91d1c2d3dd',
 'tests/diff/cases/semantics/collections/array_literal_index_target.xr': 'dfae303a8daa5dc8c99e50f60d278c0aff095adb6f80afebf840af5098a09f6d',
 'tests/diff/cases/semantics/collections/array_member_scalar_target.xr': '94c256734865804425968c44209ecdcf99d641c8305cc1ff79088d27bf62bdb9',
 'tests/diff/cases/semantics/collections/array_push_scalar_target.xr': 'ea951205fa6a310f000a36ef5023cf5180097d296e63d79257805c653040736c',
 'tests/diff/cases/semantics/collections/array_sort_shared_core.xr': 'e37060c9121881428f5950646cc95747c82ecf3b82455001b37124a91fcbefd1',
 'tests/diff/cases/semantics/collections/method_self_return_ownership.xr': '9bcb05da2486703931b80bf54b2ef2fb58a123db4b854753fd7da562592ac6e5',
 'tests/diff/cases/semantics/collections/stringbuilder_append_shared_core.xr': 'f6e3a712e0684d7f00778d72574e101bdb69f07058044d5bd16b8a967f4cee6d',
 'tests/diff/cases/semantics/collections/stringbuilder_scalar_known_append_shared_core.xr': '7df35f335e589d2d8d5ce883264c5903b7451231c3fdf9cbd94e82bdeba095ea',
 'tests/diff/cases/semantics/user_hashable_map_set_key.xr': '285463b944b53a2353f6ac394f47296fc18a5ec90b958efcb6f85aff7c4438da',
 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/method_self_return_ownership/root.xr': '9cc0f45a28f3b5c1632524e5c099d91ba36d770bcf2d51a8da1d8b0523007bc4',
 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/stringbuilder_append_shared_core/root.xr': 'e7f0d02f512dea48805717a5c661e210b663fa0457f8f264a1ab65c491a3fac7',
 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/stringbuilder_scalar_known_append_shared_core/root.xr': 'a0a030b6590679d153be469971ec2e7867e748a6a534db3c8dd16766f041bebe'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/010-9-generics.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md',
 'spec/source/sections/019-18-error-codes.md',
 'src/shared/xr_float_fmt.h',
 'src/shared/xr_sort_core.h',
 'src/shared/xr_value_format_core.h',
 'stdlib/math/math.xr',
 'stdlib/types/array.xr',
 'stdlib/types/f64.xr',
 'stdlib/types/i64.xr',
 'stdlib/types/iterator.xr',
 'stdlib/types/map.xr',
 'stdlib/types/set.xr',
 'stdlib/types/string.xr',
 'tests/diff/cases/semantics/collections/array_join_shared_core.xr',
 'tests/diff/cases/semantics/collections/array_literal_index_target.xr',
 'tests/diff/cases/semantics/collections/array_member_scalar_target.xr',
 'tests/diff/cases/semantics/collections/array_push_scalar_target.xr',
 'tests/diff/cases/semantics/collections/array_sort_shared_core.xr',
 'tests/diff/cases/semantics/collections/method_self_return_ownership.xr',
 'tests/diff/cases/semantics/collections/stringbuilder_append_shared_core.xr',
 'tests/diff/cases/semantics/collections/stringbuilder_scalar_known_append_shared_core.xr',
 'tests/diff/cases/semantics/user_hashable_map_set_key.xr',
 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/method_self_return_ownership/root.xr',
 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/stringbuilder_append_shared_core/root.xr',
 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/stringbuilder_scalar_known_append_shared_core/root.xr')
_PROGRAMS = {'array_join_shared_core': 'tests/diff/cases/semantics/collections/array_join_shared_core.xr',
 'array_literal_index_target': 'tests/diff/cases/semantics/collections/array_literal_index_target.xr',
 'array_member_scalar_target': 'tests/diff/cases/semantics/collections/array_member_scalar_target.xr',
 'array_push_scalar_target': 'tests/diff/cases/semantics/collections/array_push_scalar_target.xr',
 'array_sort_shared_core': 'tests/diff/cases/semantics/collections/array_sort_shared_core.xr',
 'method_self_return_ownership': 'tests/diff/cases/semantics/collections/method_self_return_ownership.xr',
 'stringbuilder_append_shared_core': 'tests/diff/cases/semantics/collections/stringbuilder_append_shared_core.xr',
 'stringbuilder_scalar_known_append_shared_core': 'tests/diff/cases/semantics/collections/stringbuilder_scalar_known_append_shared_core.xr',
 'user_hashable_map_set_key': 'tests/diff/cases/semantics/user_hashable_map_set_key.xr'}
_PORTS = {'method_self_return_ownership': 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/method_self_return_ownership/root.xr',
 'stringbuilder_append_shared_core': 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/stringbuilder_append_shared_core/root.xr',
 'stringbuilder_scalar_known_append_shared_core': 'tests/unit/xir/legacy_consumers/fixtures/string_accumulation_complete/stringbuilder_scalar_known_append_shared_core/root.xr'}
_NAMES=frozenset({'array_member_scalar_target','array_push_scalar_target','array_literal_index_target','array_join_shared_core',
 'array_sort_shared_core','stringbuilder_append_shared_core','method_self_return_ownership','user_hashable_map_set_key','stringbuilder_scalar_known_append_shared_core'})

class Array:
    def __init__(self,items):self.items=copy.deepcopy(list(items))
    def reverse(self):self.items.reverse();return Array(self.items)
    def sort(self):self.items.sort();return Array(self.items)
    def fill(self,value,start=0,end=None):
        end=len(self.items) if end is None else end
        if not 0<=start<=end<=len(self.items):raise ValueError('E0422')
        self.items[start:end]=[copy.deepcopy(value) for _ in range(end-start)];return Array(self.items)
    def iterator(self):return iter(copy.deepcopy(self.items))

def render(value):
    if value is None:return 'null'
    if type(value) is bool:return str(value).lower()
    if type(value) in {str,int}:return str(value)
    if type(value) is float:
        if math.isnan(value):return 'nan'
        if math.isinf(value):return '-inf' if value<0 else 'inf'
        return str(value)
    raise ValueError('This reviewed trace only renders explicit scalar observations')

def member_trace():
    counts=Array([1,2,3]);counts.items.append(4);counts.items.insert(0,0)
    index=lambda values,needle:values.index(needle) if needle in values else -1
    out=[counts.items[0],counts.items[4],index(counts.items,3),index(counts.items,42),2 in counts.items,42 in counts.items]
    counts.reverse();out.append(counts.items[0]);counts.sort();out.extend([counts.items[0],counts.items[4]])
    span=Array([1,2,3,4,5,6]);span.fill(9,2,5);out.extend(span.items[i] for i in [1,2,4,5]);span.fill(7,4);out.extend(span.items[i] for i in [3,5]);span.fill(4);out.extend(span.items[i] for i in [0,5])
    seed,lo,hi=8,1,3;bounded=Array([1,2,3,4,5]);bounded.fill(seed,lo,hi);out.extend(bounded.items[i] for i in [0,1,2,3]);out.append(index(bounded.items,seed));span.fill(seed);out.append(span.items[2]);return out

def self_return_trace():
    out=[];a=Array([3,1,2]);b=a.reverse();out.append('b0 '+str(b.items[0]));b.items[0]=9;out.append('a0 '+str(a.items[0]));a.reverse();out.append('a0-again '+str(a.items[0]))
    builder='';builder+='hello';builder='';same=builder;same+='world';out.append('sb '+builder);builder='';out.append('cleared '+builder)
    value='ab'+'cd';out.extend([prefix+' '+text for prefix,text in [('t',value.replace('zz','yy',1)),('u',value*1),('v',value.replace('zz','yy')),('s',value)]])
    a=Array([3,1,2]);s=a.sort();s.items[0]=7;out.append('a0 '+str(a.items[0]));a.sort();filled=a.fill(5);out.append('f '+str(filled.items[0])+' '+str(a.items[1]));bytes_=Array([0]*4);bytes_.fill(9);r=bytes_.fill(3);out.append('r0 '+str(r.items[0]))
    copied=('hi'+' there').encode();out.append('b '+str(copied[0])+' '+str(copied[1]))
    rows=Array([[1,2],[3,4],[5,6]]);rows.items.pop();rows.items.pop(0);discard=[copy.deepcopy(row) for row in rows.items if len(row)>0];out.extend(['rows '+str(len(rows.items)),'rows0-after-filter '+str(rows.items[0][0])])
    a=Array([3,1,2]);discard=[x*2 for x in a.items];discard=[x for x in a.items if x>1]
    text='a,b,c';mapping={'k':[1,2]};discard=text.split(',');discard=text[:2];discard=copy.deepcopy(mapping['k']);out.extend(['s '+text+' '+str(len(mapping)),'map-value '+str(mapping['k'][0])])
    rows=Array([[1,2],[3,4]]);iterator=rows.iterator();next(iterator);kept=next(iterator);out.extend(['kept '+str(kept[0]),'rows0 '+str(rows.items[0][0])])
    a=Array([1,2,3]);b=a.reverse();out.extend('y '+str(x*2) for x in b.items);sorted_=a.sort();out.append('y '+str(sorted_.items[0]));filled=a.fill(4);out.append('y '+str(filled.items[0]));builder='';builder+='done';builder='';same=builder;same+='x';out.append('y '+str(a.items[0]));return out

def observations():
    rows={'array_member_scalar_target':member_trace(),'method_self_return_ownership':self_return_trace()}
    counts=[1,2,3];counts.extend([4,-5]);ratios=[0.5];ratios.append(1.25);flags=[True];flags.append(False);out=[len(counts),counts[3],counts[4],len(ratios),ratios[1],flags[1]];counts.append(7);out.append(counts[5]);rows['array_push_scalar_target']=out
    a=[1,2,3];out=[a[0],a[2]];a[1]=99;out.extend([a[1],sum([-7,11]),a[1],sum(a)]);rows['array_literal_index_target']=out
    rows['array_join_shared_core']=[sep.join(render(value) for value in items) for sep,items in [('|',[12,-3,0,99]),('/',[False,True,False]),(',',[1.5,-0.0,2.25]),('-',[65,0,255]),(':',['a',None,True,42,2.5])]]+['empty='+','.join([])]
    huge=lambda a,b: -4294967296 if a<b else 4294967296 if a>b else 0
    # Comparator uses sign, not a narrowing conversion of its i64 magnitude.
    from functools import cmp_to_key
    values=sorted([4,1,3,2],key=cmp_to_key(huge));out=[*values,*sorted(['b','aa','a','ab']),*sorted([5,2,9,2]),*sorted([True,False,True,False])]
    floats=sorted([2.0,float('nan'),-0.0,-3.0,0.0],key=lambda value:(math.isnan(value),0.0 if math.isnan(value) else value));out.extend([floats[0]==-3.0,floats[1]==0.0 and floats[2]==0.0,floats[3]==2.0,math.isnan(floats[4])]);out.extend(sorted(['é','z','中','a'],key=lambda value:value.encode('utf-8')));rows['array_sort_shared_core']=out
    builder=''
    for value in ['bool=',True,'; null=',None,'; range=','1..4','; array=','['+', '.join(str(x) for x in [1,2,3])+']']:builder+=render(value)
    rows['stringbuilder_append_shared_core']=[builder]
    pieces=['[',True,'|',False,'|','中','|','A','|',None,']'];rows['stringbuilder_scalar_known_append_shared_core']=[''.join(render(v) for v in pieces)]
    # A key's declared equality compares both fields; its hash is only a bucket hint.
    key=lambda a,b:(a,b);mapping={key(1,'x'):'one',key(2,'yy'):'two'};mapping[key(1,'x')]='ONE';out=[len(mapping),mapping[key(1,'x')],mapping[key(2,'yy')],key(1,'x') in mapping,key(3,'z') in mapping];removed=mapping.pop(key(2,'yy'),None) is not None;out.extend([removed,len(mapping)]);seen={key(5,'p'),key(5,'p'),key(6,'q')};out.extend([len(seen),key(5,'p') in seen,key(9,'z') in seen]);rows['user_hashable_map_set_key']=out
    if set(rows)!=_NAMES:raise ValueError('Collection model lost a full source')
    return rows

_PORT_NAMES=frozenset({'stringbuilder_append_shared_core','method_self_return_ownership','stringbuilder_scalar_known_append_shared_core'})

def port_text(name,text):
    if name not in _PORT_NAMES:raise ValueError('Unreviewed StringBuilder retirement port')
    if name=='method_self_return_ownership':
        transforms=[('var sb = StringBuilder()','var sb = ""',2),('sb.append("hello")','sb += "hello"',1),('sb.append("done")','sb += "done"',1),
         ('var same = sb.clear()','sb = ""\n    var same = sb',2),('same.append("world")','same += "world"',1),('same.append("x")','same += "x"',1),('    sb.clear()','    sb = ""',1)]
        for before,after,count in transforms:
            if text.count(before)!=count:raise ValueError('Owned string retirement lost the exact full original body')
            text=text.replace(before,after)
        if text.count('sb.toString()')!=3:raise ValueError('Retirement port lost owned string reads')
        return text.replace('    sb.toString()\n','    copy(sb)\n').replace('sb.toString()','sb')
    if text.count('var sb = StringBuilder()')!=1:raise ValueError('Scalar append port lost original constructor')
    text=text.replace('var sb = StringBuilder()','var sb = ""');result=[];count=0
    for line in text.splitlines(keepends=True):
        if line.strip().startswith('sb.append('):
            line=line.replace('sb.append(null)','sb += "null"',1) if line.strip()=='sb.append(null)' else line.replace('sb.append(','sb += string(',1)
            count+=1
        result.append(line)
    if count!=({'stringbuilder_append_shared_core':8,'stringbuilder_scalar_known_append_shared_core':11}[name]):raise ValueError('Scalar append port lost an original argument')
    output=''.join(result)
    if output.count('sb.toString()')!=1:raise ValueError('Scalar retirement port lost final string observation')
    return output.replace('sb.toString()','sb')

def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES) or set(_PORTS)!=_PORT_NAMES:raise ValueError('Collection exact identities changed')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib','src'}:raise ValueError('Collection input is not delivered')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Collection source/authority changed: '+name)
    for name,port in _PORTS.items():
        if port not in _INPUTS:raise ValueError('Missing whole retirement port')
        original=(ROOT/_PROGRAMS[name]).read_bytes().replace(b'\r\n',b'\n').decode('utf-8')
        if (ROOT/port).read_bytes().replace(b'\r\n',b'\n')!=port_text(name,original).encode():raise ValueError('Retirement port changed more than reviewed string operations')
    rows=[]
    for name,values in sorted(observations().items()):
        source=_PROGRAMS[name]
        if Path(source).stem!=name or source not in _INPUTS:raise ValueError('Collection original source identity changed')
        rows.append({'id':'complete_collection_values_'+name,'source':_PORTS.get(name,source),'original_source':source,'required_target':None,
         'expected':{'exit_code':0,'stdout_utf8':''.join(render(v)+'\n' for v in values),'stderr_utf8':''},
         'model':'Complete original source; independent scalar/ordered container operations,Array and string logical copies,sign comparator/NaN last/UTF8 ordering;no RC/physical/generated-C/fullFI qualification.',
         'observation_count':len(values)})
    return rows
