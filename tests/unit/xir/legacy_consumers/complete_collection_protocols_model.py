"""Independent complete callback, ordered bool Map, ref and scalar-text traces."""
from pathlib import Path
import copy,hashlib,struct
import complete_numeric_model

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/010-9-generics.md': '246757d531f91e2d19d2cd18353b5ec8addf05d309e5489c10da37e48be81226',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '1db2fee5baa4751012795fa7764f40d3b5a3de6274b7cd51a4586a41fe54ed1e',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/bool.xr': '8ad71c5d00a9255a5a8d3478f0515b948511758ecb1373848ce6a9393270e26b',
 'stdlib/types/i64.xr': '2d9a160b70394a77ec69dccdf7b895ecd5ac7e66548824a835d3eb0a7cd45567',
 'stdlib/types/map.xr': '49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31',
 'tests/diff/cases/semantics/collections/_array_fill_ref_lib.xr': 'dba3bf1ee43bedfa5478ef7fac827ca7e52be0c3d840b2985d58d3ef197f1de0',
 'tests/diff/cases/semantics/collections/array_fill_ref_cross_module.xr': '4fd01f2b9b6c05e6ee17d09074871b621f46ee947a271a7cbf1928dde4005c35',
 'tests/diff/cases/semantics/collections/hof_index_param.xr': '28e21028a5ad97a86dc000b385732d1594a01bd40679ecbb84ecefa295b88aaf',
 'tests/diff/cases/semantics/collections/map_bool_key.xr': '8aceac7a024ea4974980d51444c15fa8a458b81fd157cb5b55eb94a80c26b5bd',
 'tests/diff/cases/semantics/int_wrap/generic_int_format_shared_core.xr': 'ed37526163e9069a3973a2dd81152437fe286570829b76cf3caf5711591c2e62',
 'tests/unit/xir/legacy_consumers/complete_numeric_model.py': '25cbd5981ce4371670942bb94f8139d4be239e08cddfb7e62fcb6bd102ee27e2',
 'tests/unit/xir/legacy_consumers/fixtures/collection_protocols_complete/generic_int_format_shared_core/root.xr': 'e4d29133d335b1b0fda1c8dbcc417b1811e9d8b1c22c95c5e5c1e5bcef664193'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/010-9-generics.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md',
 'stdlib/types/array.xr',
 'stdlib/types/bool.xr',
 'stdlib/types/i64.xr',
 'stdlib/types/map.xr',
 'tests/diff/cases/semantics/collections/_array_fill_ref_lib.xr',
 'tests/diff/cases/semantics/collections/array_fill_ref_cross_module.xr',
 'tests/diff/cases/semantics/collections/hof_index_param.xr',
 'tests/diff/cases/semantics/collections/map_bool_key.xr',
 'tests/diff/cases/semantics/int_wrap/generic_int_format_shared_core.xr',
 'tests/unit/xir/legacy_consumers/complete_numeric_model.py',
 'tests/unit/xir/legacy_consumers/fixtures/collection_protocols_complete/generic_int_format_shared_core/root.xr')
_PROGRAMS = {'array_fill_ref_cross_module': 'tests/diff/cases/semantics/collections/array_fill_ref_cross_module.xr',
 'generic_int_format_shared_core': 'tests/diff/cases/semantics/int_wrap/generic_int_format_shared_core.xr',
 'hof_index_param': 'tests/diff/cases/semantics/collections/hof_index_param.xr',
 'map_bool_key': 'tests/diff/cases/semantics/collections/map_bool_key.xr'}
_PORTS = {'generic_int_format_shared_core': 'tests/unit/xir/legacy_consumers/fixtures/collection_protocols_complete/generic_int_format_shared_core/root.xr'}
_NAMES=frozenset({'hof_index_param','map_bool_key','array_fill_ref_cross_module','generic_int_format_shared_core'})
_PORT_NAMES=frozenset({'generic_int_format_shared_core'})

def scalar(value):
    if type(value) is bool:return str(value).lower()
    if type(value) in {int,str,float}:return str(value)
    raise ValueError('This trace requires an explicit scalar rendering')

class BoolMap:
    """Ordered language keys use bool identity; Python integers grant no key access."""
    def __init__(self,kind='i64'):
        if kind not in {'i64','f32'}:raise ValueError('Unreviewed value type')
        self.kind=kind;self.items={}
    @staticmethod
    def key(key):
        if type(key) is not bool:raise ValueError('Map<bool,T> requires an exact bool key')
        return key
    def put(self,key,value):
        key=self.key(key)
        if self.kind=='i64':
            if type(value) is not int or not -(1<<63)<=value<(1<<63):raise ValueError('i64 value required')
        else:
            if type(value) not in {int,float}:raise ValueError('f32 value required')
            value=struct.unpack('!f',struct.pack('!f',value))[0]
        self.items[key]=value
    def has(self,key):return self.key(key) in self.items
    def get(self,key):return self.items.get(self.key(key))
    def delete(self,key):return self.items.pop(self.key(key),None) is not None
    def snapshot(self):return copy.deepcopy(self.items)

def bool_map_dump(mapping):
    out=['size='+str(len(mapping.items)),
         'has true='+scalar(mapping.has(True))+' has false='+scalar(mapping.has(False))]
    out.extend('pair '+scalar(k)+'='+scalar(v) for k,v in mapping.items.items())
    out.extend('key '+scalar(k) for k in mapping.items)
    out.extend('val '+scalar(v) for v in mapping.items.values())
    return out

def bool_map_trace():
    out=['-- insert false then true --'];a=BoolMap();a.put(False,34);a.put(True,21)
    out+=bool_map_dump(a);out.append('get true='+str(a.get(True))+' get false='+str(a.get(False)))
    out.append('-- insert true then false --');b=BoolMap();b.put(True,7);b.put(False,9);out+=bool_map_dump(b)
    out.append('-- update keeps position --');b.put(True,70);out+=bool_map_dump(b)
    out.append('-- delete + reinsert moves to end --');b.delete(True);b.put(True,700);out+=bool_map_dump(b)
    out.append('-- delete to empty --');b.delete(True);b.delete(False)
    out.append('size='+str(len(b.items))+' has true='+scalar(b.has(True)))
    out.append('-- float32 narrowing --');c=BoolMap('f32');c.put(True,1.5);c.put(False,2.25)
    out.extend(['csize='+str(len(c.items)),'cget true='+scalar(c.get(True))+' cget false='+scalar(c.get(False))])
    out.extend('cf '+scalar(k)+'='+scalar(v) for k,v in c.items.items())
    out.append('-- class field --');bag=BoolMap();bag.put(True,21);bag.put(False,34)
    out.extend(['fill='+str(len(bag.items)),'probe='+str(sum(bag.get(k) for k in [True,False] if bag.has(k)))])
    return out

def indexed_callback_trace():
    items=[10,20,30]
    # The one-argument callback consumes the element prefix; two-argument
    # callbacks observe the original source index, before filtering.
    mapped=[value*100 for value in items]
    indexed=[value*100+index for index,value in enumerate(items)]
    filtered=[value for index,value in enumerate(items) if index<2]
    return [complete_numeric_model.scalar(v) for v in [mapped,indexed,filtered]]+[
        str(index)+':'+str(value) for index,value in enumerate([10,20])]

def take_branch(stack,kinds,keep_arity,height):
    if not 0<=keep_arity<=min(len(stack),len(kinds)) or height<0:
        raise ValueError('Invalid complete ref-branch inputs')
    kept=[0]*keep_arity;kept_kinds=['I32']*keep_arity
    for k in range(keep_arity):
        kept[keep_arity-1-k]=stack.pop();kept_kinds[keep_arity-1-k]=kinds.pop()
    if len(stack)>height:
        stack[:]=stack[:height]+[0]*max(0,height-len(stack))
        kinds[:]=kinds[:height]+['I32']*max(0,height-len(kinds))
    for p in range(keep_arity):stack.append(kept[p]);kinds.append(kept_kinds[p])

def ref_branch_trace():
    stack=[1,2,3,4,5];kinds=['I64']*5;take_branch(stack,kinds,2,1)
    out=[len(stack),*stack,len(kinds)];empty=[];marks=[];take_branch(empty,marks,0,0)
    return out+[len(empty)]

def observations():
    return {'hof_index_param':indexed_callback_trace(),'map_bool_key':bool_map_trace(),
        'array_fill_ref_cross_module':ref_branch_trace(),
        'generic_int_format_shared_core':complete_numeric_model.observations()['int_wrap/generic_int_format_shared_core']}

def port_text(name,text):
    if name!='generic_int_format_shared_core':raise ValueError('Unreviewed explicit scalar-text port')
    before='var mixed = ["n", min, null, true, max]'
    after='var mixed: Array<string> = ["n", min.toString(), "null", true.toString(), max.toString()]'
    if text.count(before)!=1:raise ValueError('Scalar-text port lost the exact complete original statement')
    return text.replace(before,after)

def expected_rows():
    if (set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or
            set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES) or set(_PORTS)!=_PORT_NAMES):
        raise ValueError('Complete collection protocols lost exact identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib'}:
            raise ValueError('Protocol input is not a delivered source identity')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Collection protocol source/authority changed: '+name)
    for name,source in _PROGRAMS.items():
        if Path(source).stem!=name or source not in _INPUTS:raise ValueError('Protocol original identity changed')
    for name,port in _PORTS.items():
        if port not in _INPUTS:raise ValueError('Protocol port is not frozen')
        original=(ROOT/_PROGRAMS[name]).read_bytes().replace(b'\r\n',b'\n').decode('utf-8')
        if (ROOT/port).read_bytes().replace(b'\r\n',b'\n')!=port_text(name,original).encode('utf-8'):
            raise ValueError('Explicit scalar-text port changed the complete original body')
    values=observations()
    if set(values)!=_NAMES:raise ValueError('Protocol model lost a complete program')
    return [{'id':'complete_collection_protocols_'+name,'source':_PORTS.get(name,_PROGRAMS[name]),
        'original_source':_PROGRAMS[name],'required_target':None,
        'expected':{'exit_code':0,'stdout_utf8':''.join(scalar(v)+'\n' for v in values[name]),'stderr_utf8':''},
        'model':'Complete original callback indices,ordered typed bool Map,imported ref roots and exact integer endpoints;only explicit typed scalar text replaces the removed heterogeneous Array.No physical/FI/safety claim.',
        'observation_count':len(values[name])} for name in sorted(_NAMES)]
