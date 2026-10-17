"""Whole collection programs under logical copy semantics, not physical ownership proof."""
from pathlib import Path
import copy,hashlib,math
import complete_json_boundary_model as json_boundary

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/010-9-generics.md': '246757d531f91e2d19d2cd18353b5ec8addf05d309e5489c10da37e48be81226',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'spec/source/sections/019-18-error-codes.md': '2c4416085d5d38f37fb0e434d863cba1ff5d5758d44efb29402fa5158282d9ad',
 'src/shared/xr_float_fmt.h': 'dc3f35e72dd96729e5948f98b8103810d1362bad8cec207aabf93db1352f983b',
 'src/shared/xr_sort_core.h': '12d05da0942da491ae99d49729a27f0dfc71e978f0d3992666624fc150d10cdf',
 'src/shared/xr_value_format_core.h': '88e14ed234554322f60c1a8694a920c4e7990dd04b4bed1a4ed8d9517a333207',
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

_DISPLAY_PROGRAMS = {
 "container_print": "tests/diff/cases/semantics/collections/container_print.xr",
 "slice_value_to_string": "tests/diff/cases/semantics/slice/slice_value_to_string.xr",
 "function_value_to_string": "tests/diff/cases/semantics/function_value_to_string.xr",
 "json_forin_shared_core": "tests/diff/cases/semantics/collections/json_forin_shared_core.xr"
}
_DISPLAY_INPUTS = {
 "tests/diff/cases/semantics/collections/container_print.xr": "5e19e9fd45001241b2d7fc4187990bf5c9d76ca52937e9ce1d2933cd2b2f7f91",
 "tests/diff/cases/semantics/slice/slice_value_to_string.xr": "206e1da48c5d9f6d6a9960cd58370e7ead00f9652f465426b0cc1b58be2b2f03",
 "tests/diff/cases/semantics/function_value_to_string.xr": "94b8631cc3c534662f0ae7441b6c6f79efe7e702dde98b7f807e908f8da29bf5",
 "tests/diff/cases/semantics/collections/json_forin_shared_core.xr": "4757b926dd2da91d6bac3c5dfb61599596cdcc589b1f7fe6906acf6ad775afbf",
 "src/runtime/value/xvalue_format.c": "aa47cbf6f46222719eb0595cf031eb7f90379cc11799dd74564b19dbb3650863",
 "src/aot/xrt_arith.h": "fe850e610049b8b1bed6d8cce152742314fabd0b7110a8edfdd3a8323f5d3a2c",
 "stdlib/types/json.xr": "9b4904f85a13bea98b04c616185183f921ed09761bde1cb48d751784fef48161"
}
_PROGRAMS.update(_DISPLAY_PROGRAMS)
_NAMES=_NAMES|frozenset(_DISPLAY_PROGRAMS)
_INPUTS.update(_DISPLAY_INPUTS)
_REQUIRED_INPUTS+=tuple(_DISPLAY_INPUTS)

_JSON_BOUNDARY_INPUT='tests/unit/xir/legacy_consumers/complete_json_boundary_model.py'
_INPUTS[_JSON_BOUNDARY_INPUT]='17012d55f7022726163e2c2057371b0ceaa52631886c99d13d6f2d7cf5245ae0'
_REQUIRED_INPUTS+=(_JSON_BOUNDARY_INPUT,)

_VALUE_BOUNDARY_PROGRAMS={'bool_conversion_truthy_shared_core': 'tests/diff/cases/semantics/collections/bool_conversion_truthy_shared_core.xr', 'array_join_special_object_shared_core': 'tests/diff/cases/semantics/collections/array_join_special_object_shared_core.xr', 'value_format_truncation_shared_core': 'tests/diff/cases/semantics/collections/value_format_truncation_shared_core.xr'}
_PROGRAMS.update(_VALUE_BOUNDARY_PROGRAMS)
_NAMES=_NAMES|frozenset(_VALUE_BOUNDARY_PROGRAMS)
_INPUTS.update({'tests/diff/cases/semantics/collections/bool_conversion_truthy_shared_core.xr': 'c1f90b63c44a001a2f431ab0212c7bad3db659e3dc1373b7ded370c942760338', 'tests/diff/cases/semantics/collections/array_join_special_object_shared_core.xr': '471dfe6c13b893800fcf888a3882c54a6064b33707e663222cb1348de6218dbe', 'tests/diff/cases/semantics/collections/value_format_truncation_shared_core.xr': '93847a826482963e8d4894c3fef17e720b547f13395d96c23883c4e2ba397ded', 'src/shared/xr_truthy_core.h': 'ae980791ae9b4706c205cc6d97287241625623e75b099bd986ea393a97737abf', 'src/shared/xr_range_core.h': '8c0d9ec40d4b40f4c78566a2bcecb25c217a921725edc1dda2929590ddec951e', 'src/runtime/object/xrange.c': '437fa3a87091575c4ac77b740fa1d9e8c90efa90282de6315bb5d26932afcc1d'})
_REQUIRED_INPUTS += ('tests/diff/cases/semantics/collections/bool_conversion_truthy_shared_core.xr', 'tests/diff/cases/semantics/collections/array_join_special_object_shared_core.xr', 'tests/diff/cases/semantics/collections/value_format_truncation_shared_core.xr', 'src/shared/xr_truthy_core.h', 'src/shared/xr_range_core.h', 'src/runtime/object/xrange.c')

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

class DisplayMap:
    """Insertion order and key/value association, independent of hash storage."""
    def __init__(self,pairs=()):
        self.pairs=[]
        for key,value in pairs:self.put(key,value)
    def put(self,key,value):
        for index,(old,_) in enumerate(self.pairs):
            if type(old) is type(key) and old==key:
                self.pairs[index]=(key,value);return
        self.pairs.append((key,value))
    def keys(self):return [key for key,_ in self.pairs]
    def values(self):return [value for _,value in self.pairs]
    def entries(self):return list(self.pairs)

class DisplaySet:
    def __init__(self,values):
        self.values=[]
        for value in values:
            if not any(type(old) is type(value) and old==value for old in self.values):self.values.append(value)

class BorrowedDisplaySlice:
    """Logical typed range over its owner; this is not a physical lifetime proof."""
    def __init__(self,owner,start,end):
        if not isinstance(owner,Array) or type(start) is not int or type(end) is not int or not 0<=start<=end<=len(owner.items):raise ValueError('Invalid borrowed display range')
        self.owner,self.start,self.end=owner,start,end
    def items(self):return self.owner.items[self.start:self.end]
    def __len__(self):return self.end-self.start

class DisplayCallable:
    def __init__(self,debug_name,body):self.debug_name,self.body,self.calls=debug_name,body,0
    def apply(self,value):self.calls+=1;return self.body(value)

class JsonDisplayScalar:
    """Only the two selected scalar arms used by the complete JSON program."""
    def __init__(self,value):
        if type(value) not in {str,int}:raise ValueError('Unreviewed JSON scalar arm')
        self.value=value
    def string(self):return render(self.value)

def display(value,nested=False):
    if isinstance(value,DisplayCallable):return '<fn>'
    if isinstance(value,BorrowedDisplaySlice):value=value.items()
    if isinstance(value,Array):value=value.items
    if isinstance(value,list):return '['+', '.join(display(item,True) for item in value)+']'
    if isinstance(value,tuple):return '('+', '.join(display(item,True) for item in value)+(',' if len(value)==1 else '')+')'
    if isinstance(value,DisplayMap):return '#{'+', '.join(display(k,True)+': '+display(v,True) for k,v in value.entries())+'}'
    if isinstance(value,DisplaySet):return '#['+', '.join(display(item,True) for item in value.values)+']'
    if type(value) is str:return '"'+value+'"' if nested else value
    return render(value)

def json_iteration_scores(mapping,expected):
    keys=pairs=0
    for key in mapping.keys():
        if key in expected:keys+=expected[key][0]
    for key,value in mapping.entries():
        if key in expected and value.string()==expected[key][1]:pairs+=expected[key][0]
    return keys,pairs

def display_observations():
    container=[Array([1,2,3]),Array([1.5,0.1,100.0]),Array(['a','b','c']),Array([[1,2],[3,4]]),Array([True,False,True]),
     (1,'x',2.5),(42,),Array([10,20,30]),Array([['x'],['y','z']]),DisplayMap([(1,10),(2,20),(3,30)]),DisplaySet([1,2,3]),
     DisplayMap([('a',1),('b',2)]),DisplayMap([(1,Array([1,2])),(2,Array([3,4]))]),DisplayMap(),DisplaySet(['x','y'])]
    rows={'container_print':[display(value) for value in container]}
    ints,names=Array([10,20,30,40]),Array(['ab','cd','ef'])
    view,nview=BorrowedDisplaySlice(ints,1,3),BorrowedDisplaySlice(names,0,2)
    empty,whole=BorrowedDisplaySlice(ints,2,2),BorrowedDisplaySlice(ints,0,4)
    rendered,nrendered=display(view),display(nview)
    describe=lambda value:len(value)
    rows['slice_value_to_string']=[display(view),rendered,nrendered,display(empty),display(whole),'v='+rendered,'n='+nrendered,len(rendered.encode('utf-8')),len(nrendered.encode('utf-8')),describe(view)]
    named,lambda_=DisplayCallable('helper',lambda n:n+1),DisplayCallable('',lambda n:n*2)
    apply=lambda fn,n:fn.apply(n)
    rows['function_value_to_string']=[display(named),display(named),display(lambda_),display(lambda_),len(display(named)),len(display(lambda_)),'f='+display(named),apply(named,4),apply(lambda_,4),display(named)==display(lambda_)]
    import json
    parsed=json.loads('{"name":"Alice","age":30}',object_pairs_hook=list)
    obj=DisplayMap((key,JsonDisplayScalar(value)) for key,value in parsed)
    dyn=DisplayMap();dyn.put('msg',JsonDisplayScalar('hello world'));dyn.put('key',JsonDisplayScalar('a&b'))
    static_keys,static_pairs=json_iteration_scores(obj,{'name':(1,'Alice'),'age':(10,'30')})
    dynamic_keys,dynamic_pairs=json_iteration_scores(dyn,{'msg':(1,'hello world'),'key':(10,'a&b')})
    rows['json_forin_shared_core']=['static keys='+str(static_keys),'static pairs='+str(static_pairs),'dynamic keys='+str(dynamic_keys),'dynamic pairs='+str(dynamic_pairs),
     'keys length='+str(len(dyn.keys())),'values length='+str(len(dyn.values())),'entries length='+str(len(dyn.entries()))]
    if set(rows)!=set(_DISPLAY_PROGRAMS) or sum(map(len,rows.values()))!=42:raise ValueError('Lost complete display observations')
    return rows

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


class RangeText:
    def __init__(self,start,end):
        if type(start) is not int or type(end) is not int:raise ValueError('Range endpoints must be exact integers')
        self.start,self.end=start,end
    def text(self):return str(self.start)+'..'+str(self.end)


def explicit_bool(value):
    if value is None:return False
    if type(value) is bool:return value
    if type(value) in {int,float}:return value!=0
    if type(value) is str:return len(value)!=0
    if isinstance(value,Array):return len(value.items)!=0
    if isinstance(value,DisplayMap):return len(value.entries())!=0
    if isinstance(value,DisplaySet):return len(value.values)!=0
    raise ValueError('Unreviewed explicit bool conversion family')


def join_selected_values(values,separator):
    if type(separator) is not str:raise ValueError('Selected join separator must be string')
    return separator.join(value.text() if isinstance(value,RangeText) else render(value) for value in values)


def limited_container_display(value):
    """Keep the complete logical value, showing the public first32 elements."""
    if isinstance(value,Array):entries=value.items;prefix,suffix='[',']';part=lambda item:display(item,True)
    elif isinstance(value,DisplayMap):entries=value.entries();prefix,suffix='#{','}';part=lambda item:display(item[0],True)+': '+display(item[1],True)
    elif isinstance(value,DisplaySet):entries=value.values;prefix,suffix='#[',']';part=lambda item:display(item,True)
    else:raise ValueError('Unreviewed top-level truncated container')
    shown=min(len(entries),32);inside=', '.join(part(item) for item in entries[:shown])
    if len(entries)>shown:inside+=', ...('+str(len(entries)-shown)+' more)'
    return prefix+inside+suffix


def complete_truncation_values(count=35):
    if type(count) is not int or count<0:raise ValueError('Invalid original loop extent')
    array=Array([]);mapping=DisplayMap();set_=DisplaySet([])
    for value in range(count):
        array.items.append(value);mapping.put(value,value+100)
        if value not in set_.values:set_.values.append(value)
    outputs=[limited_container_display(container) for container in (array,mapping,set_)]
    if len(array.items)!=count or len(mapping.entries())!=count or len(set_.values)!=count:raise ValueError('Display destroyed undisplayed values')
    return outputs,{'original_loop_iterations':count,'original_mutation_operations':3*count,'complete_container_lengths':[count]*3,
        'public_display_element_limit':32,'shown_elements_per_container':min(count,32),'undisplayed_elements_per_container':max(0,count-32),
        'full_original_values_retained_after_display':True,'physical_storage_and_output_ownership_qualified':False}


def value_boundary_observations():
    cases=[('null',None),('false',False),('true',True),('zero-int',0),('nonzero-int',-1),('zero-float',0.0),('nonzero-float',0.25),('empty-string',''),('nonempty-string','x'),
        ('empty-array',Array([])),('full-array',Array([1])),('empty-map',DisplayMap()),('full-map',DisplayMap([('x',1)])),
        ('empty-boolmap',DisplayMap()),('full-boolmap',DisplayMap([(True,1)])),('empty-set',DisplaySet([])),('full-set',DisplaySet(['x'])),
        ('empty-typed-set',DisplaySet([])),('full-typed-set',DisplaySet([7]))]
    truth=[label+'='+render(explicit_bool(value)) for label,value in cases]
    ranges=[RangeText(1,3),RangeText(4,4),RangeText(9,12)]
    joins=[join_selected_values(ranges,','),join_selected_values(['tag',RangeText(2,5),None],':')]
    return {'bool_conversion_truthy_shared_core':truth,'array_join_special_object_shared_core':joins,
        'value_format_truncation_shared_core':complete_truncation_values()[0]}


def value_boundary_semantic_traces():
    return {'bool_conversion_truthy_shared_core':{'explicit_conversion_calls':19,'implicit_condition_truthiness_not_granted':True,
        'original_sized_container_families':['Array<i64>','Map<string,i64>','Map<bool,i64>','Set<string>','Set<u8>'],
        'conversion_state_access_and_provider_qualified':False},
        'array_join_special_object_shared_core':{'original_range_endpoints':[[1,3],[4,4],[9,12],[2,5]],
        'original_ranges_joined_as_values_not_enumerated':True,'empty_range_keeps_4_dotdot_4_text':True,'null_text_is_null':True,
        'original_range_Array_elements':3,'original_mixed_Array_elements':3,'real_join_ToString_capabilities_and_runtime_qualified':False},
        'value_format_truncation_shared_core':complete_truncation_values()[1]}


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
    rows.update(display_observations())
    rows.update(value_boundary_observations())
    rows[_TYPE_NAMES_NAME]=type_names_trace()['observations']
    rows[_NET_REFERENCE_NAME]=net_reference_trace()['observations']
    if set(rows)!=_NAMES:raise ValueError('Collection model lost a full source')
    return rows

_PORT_NAMES=frozenset({'stringbuilder_append_shared_core','method_self_return_ownership','stringbuilder_scalar_known_append_shared_core'})

def port_text(name,text):
    if name==_TYPE_NAMES_NAME:
        if text.count('var sb = StringBuilder()')!=1:raise ValueError('Whole type-name retirement lost its single constructor')
        return text.replace('var sb = StringBuilder()','var sb = ""')
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
    for row in rows:
        if row['original_source'] in _DISPLAY_PROGRAMS.values():
            row['model']='Complete unchanged original; independent recursive container display,insertion order,singleton tuple comma,typed Slice range,callable name-free display plus actual calls,and selected JSON scalar/key/value association. No physical lifetime,Source,generated-C,FI,safety or provider qualification.'
    for row in rows:
        name=Path(row['original_source']).stem
        if name in _VALUE_BOUNDARY_PROGRAMS:
            row['required_semantic_trace']=value_boundary_semantic_traces()[name]
            row['model']='Complete unchanged original explicit bool conversions,Range and nullable ToString joins,or all35 iterations/105 mutations with public first32 display and exact remaining suffix. Full underlying values retained;no implicit conditions,physical layout,output ownership,Source,FI,lifetime or provider qualification inferred.'
    for row in rows:
        if row['original_source']==_TYPE_NAMES_ORIGINAL:
            row['required_semantic_trace']=type_names_trace()
            row['model']='Whole original ten typeName/print calls with all scalar,Array,Map,Set andRange operands preserved;only retired builder becomes owned String. Fixed neutral name labels and Source top-family rules;Range Source admission,string ownership,FI and providers remain unqualified.'
    for row in rows:
        if row['original_source']==_NET_REFERENCE_ORIGINAL:
            row['required_semantic_trace']=net_reference_trace()
            row['model']='Complete unchanged original four-name net import,two helper bodies andallthreeprints;one typed callable-reference acceptance andtwo nominal enum family names. Neither counts helper nor copyBidirectional body is called;no socket/bytecopy/cancellation/ABI/Source/lifetime/FI/provider qualification inferred.'
    rows.extend(json_boundary.expected_rows())
    return rows


_TYPE_NAMES_NAME='type_names_shared_core'
_TYPE_NAMES_ORIGINAL='tests/diff/cases/semantics/collections/type_names_shared_core.xr'
_TYPE_NAMES_PORT='tests/unit/xir/legacy_consumers/fixtures/type_names_owned_string/root.xr'
_TYPE_NAMES_REFERENCE='tests/unit/xir/legacy_consumers/fixtures/type_names_reference/reference.json'
_PROGRAMS[_TYPE_NAMES_NAME]=_TYPE_NAMES_ORIGINAL
_PORTS[_TYPE_NAMES_NAME]=_TYPE_NAMES_PORT
_NAMES=frozenset(set(_NAMES)|{_TYPE_NAMES_NAME})
_PORT_NAMES=frozenset(set(_PORT_NAMES)|{_TYPE_NAMES_NAME})
_INPUTS.update({'tests/diff/cases/semantics/collections/type_names_shared_core.xr': '55aba745cce5ef51de15ee28fbd38f3c78c06f7a4cd6d4657103a5d779372f40', 'tests/unit/xir/legacy_consumers/fixtures/type_names_owned_string/root.xr': '9c9798c5eea11e5bfef550e52134dd7af4b58d6915ada35fbbc98f15b6cd1204', 'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/reference.json': 'f1377b58bd991f76e6756cec3506a3df173423c2289d1fc6eea2288839bc1adb', 'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/xr_type_names_core.h.txt': '8a506e6252834b12c27686390502072c2110a2148418c16961e9fb58a856deb1', 'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/xxir_source_type_name.inc.c.txt': '0109a06b9e09b2968061d9cdd27bdd088b66ee75d159e827773caf3d61bcaa51', 'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/014-13-built-in-functions.md.txt': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380', 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380'})
_REQUIRED_INPUTS += ('tests/diff/cases/semantics/collections/type_names_shared_core.xr', 'tests/unit/xir/legacy_consumers/fixtures/type_names_owned_string/root.xr', 'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/reference.json', 'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/xr_type_names_core.h.txt', 'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/xxir_source_type_name.inc.c.txt', 'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/014-13-built-in-functions.md.txt', 'spec/source/sections/014-13-built-in-functions.md')


def type_name_reference():
    import json,re
    ref=json.loads((ROOT/_TYPE_NAMES_REFERENCE).read_text(encoding='utf-8'))
    if ref['fixed_commit']!='460622cf45c82ab0bc93be14d2f0ac3fce21bef6' or ref['Range_value_Source_admission_qualified'] or ref['runtime_output_used_to_author_expectations']:
        raise ValueError('Type-name reference scope or fixed identity changed')
    content={}
    wanted={'src/shared/xr_type_names_core.h','src/xir/xxir_source_type_name.inc.c','spec/source/sections/014-13-built-in-functions.md'}
    if len(ref['source_snapshots'])!=3 or {x['source'] for x in ref['source_snapshots']}!=wanted:raise ValueError('Type-name reference source set changed')
    for row in ref['source_snapshots']:
        if row['snapshot'] not in _INPUTS:raise ValueError('Type-name reference is not delivered')
        raw=(ROOT/row['snapshot']).read_bytes().replace(b'\r\n',b'\n')
        if hashlib.sha256(raw).hexdigest()!=row['lf_sha256'] or hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()!=row['git_blob_oid']:
            raise ValueError('Type-name source snapshot changed')
        content[row['source']]=raw.decode()
    macros=dict(re.findall(r'^#define TYPE_NAME_([A-Z0-9_]+) "([^"\n]+)"$',content['src/shared/xr_type_names_core.h'],re.M))
    wanted={'I64':'i64','F64':'f64','BOOL':'bool','NULL':'null','STRING':'string','ARRAY':'Array','MAP':'Map','SET':'Set','RANGE':'Range'}
    if any(macros.get(k)!=v for k,v in wanted.items()):raise ValueError('Public type-name labels changed')
    source=content['src/xir/xxir_source_type_name.inc.c']
    if 'case XR_XIR_TYPE_MAP:' not in source or 'case XR_XIR_TYPE_SET:' not in source or 'if (!top)' not in source or 'TYPE_NAME_MAP : TYPE_NAME_SET' not in source:
        raise ValueError('Top-level builtin Map/Set name policy changed')
    return wanted


def reviewed_type_name(shape):
    names=type_name_reference()
    scalar={'i64':'I64','f64':'F64','bool':'BOOL','null-literal':'NULL','string':'STRING'}
    if type(shape) is str and shape in scalar:return names[scalar[shape]]
    admitted={('Array','i64'):'ARRAY',('Map','string','i64'):'MAP',('Set','i64'):'SET',('Range','i64'):'RANGE'}
    if type(shape) is tuple and shape in admitted:return names[admitted[shape]]
    raise ValueError('Unreviewed whole-program type-name operand')


def type_names_trace():
    shapes=['i64','f64','bool','null-literal','string',('Array','i64'),('Map','string','i64'),('Set','i64'),('Range','i64'),'string']
    values=[0,1.5,True,None,'x',[1,2,3],{'a':1},[1,2,3],{'start':2,'end':6,'step':1,'inclusive':False},'']
    return {'observations':[reviewed_type_name(shape) for shape in shapes],'original_operand_shapes':shapes,
        'all_original_operand_values':values,'original_typeName_calls':10,'original_print_expressions':10,
        'retired_builder_target':'owned empty String','generic_arguments_not_printed_for_top_builtin_families':True,
        'Range_Source_admission_and_result_string_ownership_FI_provider_qualified':False}

from dataclasses import dataclass


_NET_REFERENCE_NAME='net_copy_bidirectional_type_surface'
_NET_REFERENCE_ORIGINAL='tests/diff/cases/semantics/stdlib/net_copy_bidirectional_type_surface.xr'
_NET_REFERENCE_DECLARATIONS='stdlib/net/net.xr'
_PROGRAMS[_NET_REFERENCE_NAME]=_NET_REFERENCE_ORIGINAL
_NAMES=frozenset(set(_NAMES)|{_NET_REFERENCE_NAME})
_INPUTS.update({'tests/diff/cases/semantics/stdlib/net_copy_bidirectional_type_surface.xr': 'cf115225bc75cd1aaaccbd2b9cd24f1fcc6a0592625caf4a48d86eb928fbc3cd', 'stdlib/net/net.xr': '3894b2d9b0fb7f167b4f39334f0fdedd7bd70d5d1ceada35ff8b9247e08c8d85'})
_REQUIRED_INPUTS += (_NET_REFERENCE_ORIGINAL,_NET_REFERENCE_DECLARATIONS)


@dataclass(frozen=True)
class NetCallableReference:
    parameter_types: tuple
    result_type: str
    symbol: str='copyBidirectional'

    def invoke(self,*arguments):
        raise AssertionError('Original type-surface consumer never calls the copier')


def accept_net_copier(reference):
    # Prove the written callable type before the helper's constant true body.
    if (type(reference) is not NetCallableReference or reference.parameter_types!=('NetConn','NetConn')
            or reference.result_type!='CopyBidirectionalResult' or reference.symbol!='copyBidirectional'):
        raise ValueError('Unreviewed net callable-reference type')
    return True


def net_reference_declarations():
    import re
    source=(ROOT/_NET_REFERENCE_DECLARATIONS).read_text(encoding='utf-8')
    signature=re.search(r'^export fn copyBidirectional\(a: NetConn, b: NetConn\) -> (CopyBidirectionalResult) \{',source,re.M)
    enum=re.search(r'^export enum (NetError) \{([^}]+)\}',source,re.M)
    if signature is None or enum is None:raise ValueError('Complete net type declarations changed')
    variants=[line.strip().rstrip(',') for line in enum[2].splitlines() if line.strip() and not line.lstrip().startswith('//')]
    if not {'Timeout','Cancelled'}<=set(variants):raise ValueError('Observed net error variants changed')
    ref=(ROOT/'tests/unit/xir/legacy_consumers/fixtures/type_names_reference/xxir_source_type_name.inc.c.txt').read_text(encoding='utf-8')
    type_name_reference()
    if 'case XR_XIR_TYPE_NOMINAL:' not in ref or '(const char *) declaration->name.bytes, declaration->name.length' not in ref:
        raise ValueError('Fixed nominal family type-name rule changed')
    return {'parameters':('NetConn','NetConn'),'result':signature[1],'nominal_name':enum[1],'variants':variants}


def net_reference_trace():
    declarations=net_reference_declarations();reference=NetCallableReference(declarations['parameters'],declarations['result'])
    outputs=[accept_net_copier(reference)]
    for variant in ('Timeout','Cancelled'):
        if variant not in declarations['variants']:raise ValueError('Missing complete net observation')
        outputs.append(declarations['nominal_name'])
    return {'observations':outputs,'original_imports':['CopyBidirectionalResult','NetConn','NetError','copyBidirectional'],
        'original_helper_bodies':['counts','accepts'],'counts_helper_invocations':0,'accepts_helper_invocations':1,
        'copier_argument_invocations':0,'copier_argument_symbol':reference.symbol,'copier_parameter_types':reference.parameter_types,
        'copier_result_type':reference.result_type,'typeName_variants':['Timeout','Cancelled'],'typeName_nominal_family':declarations['nominal_name'],
        'original_print_expressions':3,'actual_socket_or_copy_operations':0,
        'network_copy_cancellation_ABI_Source_lifetime_FI_provider_qualified':False}
