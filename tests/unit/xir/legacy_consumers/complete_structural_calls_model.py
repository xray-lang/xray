"""Complete structural calls, typed narrowing, hash keys and unused signatures.

The model uses logical fields and explicit JSON decoding, not physical field
ordinals or erased object predicates. Two declaration-only programs retain
signature admission duties without claiming an argument/body ever executed.
Class key copies stop at identity; no ABI, generic lowering, dispatch, borrow,
lifetime, resource, safety or provider qualification follows from these values.
"""
from dataclasses import dataclass
from pathlib import Path
import hashlib,json,re
import complete_typed_json_model as codec
from independent_observation_format import render
ROOT=Path(__file__).resolve().parents[4]
_PROGRAMS={
 'generic_width_direct_ordinal':'tests/diff/cases/semantics/objects/generic_width_direct_ordinal.xr',
 'object_shape_test_and_narrow':'tests/diff/cases/semantics/types/object_shape_test_and_narrow.xr',
 'hashable_contract':'tests/diff/cases/semantics/oop/hashable_contract.xr',
 'array_member_declared_selector_refused':'tests/diff/cases/semantics/collections/array_member_declared_selector_refused.xr',
 '1471_default_read_rest_param':'tests/regression/13_types/1471_default_read_rest_param.xr',
 '1472_default_read_destructure_param':'tests/regression/13_types/1472_default_read_destructure_param.xr'}
_NAMES=frozenset(_PROGRAMS)
_IDENTITIES=tuple(_PROGRAMS.items())
GOLDEN=_PROGRAMS['object_shape_test_and_narrow']+'.expected'
_AUTHORITIES=('spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/006-5-declarations.md','spec/source/sections/008-7-scoping-and-name-resolution.md',
 'spec/source/sections/014-13-built-in-functions.md','spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md','stdlib/types/map.xr','stdlib/types/set.xr','stdlib/types/json.xr',
 'src/shared/xr_json_type.h','tests/unit/xir/legacy_consumers/complete_typed_json_model.py',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/008-7-scoping-and-name-resolution.md': 'ec46ac53d1f2886e2ff2a5ebf3d5b7bd7e7d99ccf472b8ad64ca4b75f8370fc7',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'src/shared/xr_json_type.h': 'fd7a05ef7c2d56e605df6f28edd240a27f1ed15838a1e3eada63160f7697ce7e',
 'stdlib/types/json.xr': '9b4904f85a13bea98b04c616185183f921ed09761bde1cb48d751784fef48161',
 'stdlib/types/map.xr': '49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31',
 'stdlib/types/set.xr': '6c8062693701d60982f550ba8b766b4c20924c9ed36cb0dc146be45f9693db8f',
 'tests/diff/cases/semantics/objects/generic_width_direct_ordinal.xr': '3d865fcd62b556cefef18a632cd33fd24b6c652eedec8eb63bc47f3545c08247',
 'tests/diff/cases/semantics/oop/hashable_contract.xr': '565be502da48d3e7ff398ecc3dc41e38938af86aba7e53a2c882931a12bb9d6a',
 'tests/diff/cases/semantics/types/object_shape_test_and_narrow.xr': 'a44a8f2ce1319e6ba8a1f83320c4b9449d4465d796b6846ea2ec3d1f13432222',
 'tests/diff/cases/semantics/types/object_shape_test_and_narrow.xr.expected': '53b7ab5c867cc121b0468f54d0880e3a22f987db5ba178e90bff346918fbdefe',
 'tests/regression/13_types/1471_default_read_rest_param.xr': '363eef017e2a79197fbd24c786533e3167ad5605951a9578ad43ef9d6e1079be',
 'tests/regression/13_types/1472_default_read_destructure_param.xr': 'c1675c7e68842ac4c7d4fe075d94dda9d17270294f02b5abbba9b09353a7caf3',
 'tests/unit/xir/legacy_consumers/complete_typed_json_model.py': '637c68dfd50a2bf44b228f6975c40e430dd5a645538e0643c35696dbcc8086d4',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}


def satisfies(value,schema,exact=True):
    if type(value) is not dict:return False
    if exact and set(value)!=set(schema) or not set(schema)<=set(value):return False
    return all(codec.decode(value[name],kind) is not codec.INVALID for name,kind in schema.items())


def project(value,name,required):
    if name not in required:raise ValueError('Generic definition cannot access an unconstrained field')
    if not satisfies(value,required,exact=False):raise ValueError('Concrete shape does not satisfy exact required field types')
    return value[name]


def describe(value):
    if satisfies(value,{'kind':'string','n':'i64'}):return 'tagged:'+str(value['n'])
    if satisfies(value,{'name':'string','age':'i64'}):return 'user:'+value['name']
    return 'neither'


@dataclass(frozen=True)
class Token:
    identity: str
    value: int
    def equal(self,other):return type(other) is Token and self.value==other.value
    def hash_value(self):return self.value
    def owned_copy(self):return Token(self.identity,self.value)


class KeyTable:
    def __init__(self,hasher=lambda value:value.hash_value()):self.buckets={};self.hasher=hasher
    def set(self,key,value):
        chain=self.buckets.setdefault(self.hasher(key),[])
        for i,(present,_) in enumerate(chain):
            if present.equal(key):chain[i]=(present,value);return
        chain.append((key,value))
    def get(self,key):
        return next((value for present,value in self.buckets.get(self.hasher(key),[]) if present.equal(key)),None)
    def contains(self,key):return any(present.equal(key) for present,_ in self.buckets.get(self.hasher(key),[]))


_DECLARED_NAME='array_member_declared_selector_refused'
_INPUTS[_PROGRAMS[_DECLARED_NAME]]='883b0955fe6a6e138a11e2219bdebec065a5640ab49cdb821d2f39d403d94d08'


def declared_i64(value):
    if type(value) is not int or not -(1<<63)<=value<(1<<63):
        raise ValueError('Declared method model requires an exact finite i64')
    return value


@dataclass(frozen=True)
class DeclaredBoxRead:
    """Read projection of the original receiver; not a class ownership model."""
    total: int=4
    def __post_init__(self):declared_i64(self.total)
    def fill(self,value,start,end):
        return declared_i64(self.total+declared_i64(value)+declared_i64(start)+declared_i64(end))
    def contains(self,value):return self.total==declared_i64(value)


def declared_selector_trace():
    receiver=DeclaredBoxRead()
    spread=receiver.fill(1,2,3)
    holds=receiver.contains(7)
    direct=receiver.contains(4)
    return {'observations':[spread,holds,direct],'nominal_receiver':'Box','initial_total':4,
        'helper_calls':['spread(Box)','holds(Box)'],'declared_method_calls':[
            {'caller':'spread','method':'Box.fill','arguments':[1,2,3]},
            {'caller':'holds','method':'Box.contains','arguments':[7]},
            {'caller':'main','method':'Box.contains','arguments':[4]}],
        'total_after_calls':receiver.total,'original_main_calls':1,
        'container_operations':0,'receiver_writes':0,
        'internal_intrinsic_or_ABI_identity':'NOT_INFERRED_FROM_THREE_OUTPUTS'}


def observations():
    required={'name':'string'}
    concrete=[{'name':'ada','age':1},{'name':'bob','zzz':2},{'age':3,'name':'cat'},{'id':4,'name':'dot'}]
    names=[project(value,'name',required) for value in concrete]
    width={'observations':[''.join(names)],'concrete_fields_in_written_order':[list(x) for x in concrete],
        'all_original_field_values':concrete,'required_generic_fields':required,'original_projection_calls':4,
        'dot_and_constant_bracket_share_name_lookup':True,'no_physical_ordinal_assumed':True}
    schema={'name':'string','age':'i64'}
    inputs=[json.loads(text) for text in ['{"name":"Alice","age":30}','{"name":"Bob"}',
        '{"name":"Carol","age":7,"extra":true}','{"name":"Dan","age":"not an int"}']]
    ok,missing,extra,wrong=inputs
    narrowed=codec.decode(ok,schema)
    values=['ok decodes User='+str(narrowed is not codec.INVALID).lower(),
        'missing decodes User='+str(codec.decode(missing,schema) is not codec.INVALID).lower(),
        'extra rejects by default='+str(codec.decode(extra,schema) is codec.INVALID).lower(),
        'extra ignores explicitly='+str(codec.decode(extra,schema,ignore_unknown=True) is not codec.INVALID).lower(),
        'wrongKind decodes User='+str(codec.decode(wrong,schema) is not codec.INVALID).lower(),
        'narrowed='+narrowed['name']+'/'+str(narrowed['age']),
        'scalar decodes User='+str(codec.decode(42,schema) is not codec.INVALID).lower(),
        describe({'kind':'t','n':9}),describe({'name':'Eve','age':1})]
    shape={'observations':values,'JSON_schema_boundary_inputs':inputs,'exact_static_union_shapes':[
        {'kind':'t','n':9},{'name':'Eve','age':1}],'unknown_fields_default':'REJECT','explicit_ignore_retains_known_fields':True}
    token=Token('original-token',7);table=KeyTable();table.set(token,99)
    values=[table.contains(token),table.get(token)]
    if table.contains(token):values.append(table.get(token))
    seen=KeyTable();seen.set(token,True);values.append(seen.contains(token))
    temporary=token.owned_copy();values.append(temporary.value)
    hashed={'observations':values,'original_hash_result':token.hash_value(),'Map_and_Set_key_identity':token.identity,
        'explicit_copy_temporary_identity':temporary.identity,'original_escaped_root_moved':False,
        'temporary_owner_moved_into_generic':True,'copy_stops_at_class_identity':True,
        'original_helper_bodies':['Token.init','Token.operator==','Token.hash','passHashable<T:Hashable>']}
    return {'generic_width_direct_ordinal':width,'object_shape_test_and_narrow':shape,'hashable_contract':hashed,
        _DECLARED_NAME:declared_selector_trace(),
        '1471_default_read_rest_param':{'observations':[],'declaration':{'name':'bad','parameter_mode':'READ',
            'variadic':True,'element_type':'i64','parameter_pattern':'xs','invocations':0,'body_empty':True,
            'rest_storage_representation':'NOT_INFERRED_FROM_UNUSED_DECLARATION'}},
        '1472_default_read_destructure_param':{'observations':[],'declaration':{'name':'bad','parameter_mode':'READ',
            'variadic':False,'parameter_pattern':'[x]','declared_type':'[i64;1]','invocations':0,'body_empty':True,
            'runtime_destructure_qualification':'NOT_EXECUTED_BY_ORIGINAL'}}}


def expected_rows():
    if (set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values())|{GOLDEN} or set(_PROGRAMS)!=_NAMES or len(_PROGRAMS)!=6 or tuple(_PROGRAMS.items())!=_IDENTITIES):
        raise ValueError('Complete structural-call source or authority identities changed')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Structural complete source or normative input changed: '+path)
    derived=observations();rows=[]
    for name,source in _PROGRAMS.items():
        trace=derived[name];text=(ROOT/source).read_text(encoding='utf-8')
        if name.startswith(('1471_','1472_')):
            if len(re.findall(r'\bbad\s*\(',text))!=1 or not re.search(r'\{\s*\}\s*$',text):
                raise ValueError('Unused complete signature gained or lost a call/body')
        stdout=''.join(render({'expected':x})+'\n' for x in trace['observations'])
        if name=='object_shape_test_and_narrow' and (ROOT/GOLDEN).read_bytes().replace(b'\r\n',b'\n')!=stdout.encode():
            raise ValueError('Independent complete typed narrowing differs from preserved original golden')
        rows.append({'id':'complete_structural_calls_'+name,'source':source,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':stdout,'stderr_utf8':''},'observation_count':len(trace['observations']),
            'required_semantic_trace':{k:v for k,v in trace.items() if k!='observations'},
            'model':'Complete unchanged original width-constrained field calls,explicit JSON/exact union narrowing,custom hash/equality and identity-stopping class copy,or unused READ signature. Empty declarations do not execute arguments. No generic specialization/ordinal ABI/trait dispatch/ownership/lifetime/resource/provider qualification.'})
        if name==_DECLARED_NAME:
            rows[-1]['model']='Complete unchanged original Box field default,declared fill/contains bodies,spread/holds helpers,main and all three prints;independent receiver arithmetic and exact i64 equality. Same spelling does not grant container semantics. No internal intrinsic stamp,ABI,class ownership,FI,lifetime,safety or provider qualification inferred.'
    return rows
