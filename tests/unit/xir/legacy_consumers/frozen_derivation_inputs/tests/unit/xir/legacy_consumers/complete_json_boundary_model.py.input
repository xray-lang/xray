"""Independent complete JSON encoding/active-arm values, with no error-shape invention."""
from pathlib import Path
import hashlib
from complete_typed_json_model import decode,INVALID

ROOT=Path(__file__).resolve().parents[4]
_BOUNDARY_PROGRAMS={'json_encode_boundary': 'tests/diff/cases/semantics/json/json_encode_boundary.xr', 'json_kind_and_object_type': 'tests/diff/cases/semantics/json/json_kind_and_object_type.xr'}
_PROGRAMS=dict(_BOUNDARY_PROGRAMS)
_NAMES=frozenset(_PROGRAMS)
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677', 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d', 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380', 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e', 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa', 'src/shared/xr_json_type.h': 'fd7a05ef7c2d56e605df6f28edd240a27f1ed15838a1e3eada63160f7697ce7e', 'stdlib/types/json.xr': '9b4904f85a13bea98b04c616185183f921ed09761bde1cb48d751784fef48161', 'tests/unit/xir/legacy_consumers/complete_typed_json_model.py': '637c68dfd50a2bf44b228f6975c40e430dd5a645538e0643c35696dbcc8086d4', 'src/runtime/object/builtins/xjson_builtins.c': '4f5136538da8ccacdd3a057cfc1a159704ab37eade59daed9e7f80af87ecaa15', 'src/shared/xr_type_names_core.h': '6aa744164143552bdb9f83edbd28ca39ed91da3430277eb7b19c40ea41881b61', 'src/shared/xr_type_names.def': '337c1c37cd780f190fbe43f1a3d4471d26a0614d461188bd8dde92b4a9d2210f', 'src/runtime/value/xtype_format.c': '03651ec8a776d5203b9d3bdc96aaa005fbe0e1ef217eddae1807850933e61314', 'src/runtime/value/xvalue_typeid.c': 'bce18622bb58a6883fd5d2915dd3da683ba11d85d7d95f949af13a7ac36a0e5a', 'src/runtime/mem/xobj_header.h': '224eb199809250531ed900f0693182a28d7b0bd20784f6ac0e607921fed541da', 'tests/diff/cases/semantics/json/json_encode_boundary.xr': '926bc8231df4e1428c342a1519fa223ab1a480b50f6bc2629ce45d8042ebb3aa', 'tests/diff/cases/semantics/json/json_kind_and_object_type.xr': '83664cec499ebd05c3b708cc84de9cc4ae4068b90f89f6014ceb7fd8f36591ab'}
_REQUIRED_INPUTS=('spec/source/sections/003-2-type-system.md', 'spec/source/sections/006-5-declarations.md', 'spec/source/sections/014-13-built-in-functions.md', 'spec/source/sections/015-14-built-in-type-methods.md', 'spec/source/sections/016-15-standard-library.md', 'src/shared/xr_json_type.h', 'stdlib/types/json.xr', 'tests/unit/xir/legacy_consumers/complete_typed_json_model.py', 'src/runtime/object/builtins/xjson_builtins.c', 'src/shared/xr_type_names_core.h', 'src/shared/xr_type_names.def', 'src/runtime/value/xtype_format.c', 'src/runtime/value/xvalue_typeid.c', 'src/runtime/mem/xobj_header.h', 'tests/diff/cases/semantics/json/json_encode_boundary.xr', 'tests/diff/cases/semantics/json/json_kind_and_object_type.xr')

_INPUTS.update({'src/runtime/object/xjson_serde.c': '5ed2dcf59b03a663ef6447b57660bc970b93bbd1b193c8b2cec8112a173fc3e2', 'src/runtime/object/xset.c': '3fc9ba34af0862b3409407b3da10f4f0a2c9ea13e461e7913d9853e087eb499d'})
_REQUIRED_INPUTS += ('src/runtime/object/xjson_serde.c', 'src/runtime/object/xset.c')

class ExactObject:
    """Keep structural identity separate until the explicit JSON boundary."""
    def __init__(self,fields):
        fields=list(fields)
        if any(type(key) is not str for key,_ in fields) or len({key for key,_ in fields})!=len(fields):raise ValueError('Invalid exact structural fields')
        self.fields=fields


class InsertionSet:
    """Finite typed Set fixture values in stable insertion order."""
    def __init__(self,values):
        self.values=[];seen=set()
        for value in values:
            if type(value) not in (bool,int,str):raise ValueError('Unreviewed independent Set element')
            identity=(type(value),value)
            if identity not in seen:seen.add(identity);self.values.append(value)


def encode_boundary(value):
    if isinstance(value,ExactObject):return {key:encode_boundary(item) for key,item in value.fields}
    if isinstance(value,InsertionSet):return [encode_boundary(item) for item in value.values]
    if type(value) is dict:
        if any(type(key) is not str for key in value):raise ValueError('JSON Map keys must be strings')
        return {key:encode_boundary(item) for key,item in value.items()}
    if type(value) is list:return [encode_boundary(item) for item in value]
    if value is None or type(value) in (bool,str):return value
    if type(value) is int and -(1<<63)<=value<(1<<63):return value
    if type(value) is float:
        import math
        if math.isfinite(value):return value
    raise ValueError('Unreviewed independent encodable value')


def json_arm(value):
    if value is None:return 'null'
    names={bool:'bool',int:'int',float:'float',str:'string',list:'array',dict:'object'}
    if type(value) not in names:raise ValueError('Value has not crossed the JSON boundary')
    return names[type(value)]


def runtime_family(value):
    if isinstance(value,ExactObject):return 'object'
    arm=json_arm(value)
    return {'array':'Array','object':'Map','int':'i64','float':'f64'}.get(arm,arm)


def positive_path_value(root,path,schema):
    """Only successful require calls; no public failure payload is invented."""
    current=root
    for segment in path:
        if type(segment) is str and type(current) is dict and segment in current:current=current[segment]
        elif type(segment) is int and type(current) is list and 0<=segment<len(current):current=current[segment]
        else:raise ValueError('Successful original path is absent or crosses a wrong kind')
    result=decode(current,schema)
    if result is INVALID:raise ValueError('Successful original path has the wrong target type')
    return result


def boundary_observations():
    user=ExactObject([('name','Bob'),('scores',[10,20]),('nested',ExactObject([('ok',True)]))])
    encoded=encode_boundary(user)
    values=[runtime_family(encoded),positive_path_value(encoded,['name'],'string'),
        positive_path_value(encoded,['scores',1],'i64'),positive_path_value(encoded,['nested','ok'],'bool')]
    encoded_map=encode_boundary({'a':1,'b':ExactObject([('tag','bee')])})
    values += [runtime_family(encoded_map),positive_path_value(encoded_map,['a'],'i64'),positive_path_value(encoded_map,['b','tag'],'string')]
    numeric_key=encode_boundary({'7':ExactObject([('tag','seven')])})
    values.append(positive_path_value(numeric_key,['7','tag'],'string'))
    array_value=encode_boundary(InsertionSet([1,2,3]))
    values += [len(array_value),array_value[0],array_value[2]]
    structural=ExactObject([('name','Ada'),('age',37)]);dynamic=encode_boundary(structural)
    family_values=[runtime_family(structural),runtime_family(dynamic),'JSON.Value',
        runtime_family(structural)=='object',runtime_family(dynamic)=='object']
    domain_values=[None,True,7,1.5,'x',encode_boundary([1,True,None]),dynamic]
    arms=[json_arm(value) for value in domain_values]
    family_values += arms+[json_arm(value)==wanted for value,wanted in zip(domain_values,['null','bool','int','float','string','array','object'])]
    return {'json_encode_boundary':values,'json_kind_and_object_type':family_values}


def boundary_semantic_traces():
    return {'json_encode_boundary':{'explicit_composite_boundary_calls':4,
        'exact_structural_object_becomes_Map_object_arm':True,'positive_require_paths':[['name'],['scores',1],['nested','ok'],['a'],['b','tag'],['7','tag']],
        'Set_insertion_order_materialized_as_Array':[1,2,3],'string_numeric_key_not_array_index':True,
        'error_channels_not_exercised_or_qualified':True,'runtime_encoding_allocations_ownership_and_provider_qualified':False},
        'json_kind_and_object_type':{'exact_structural_type_name':'object','dynamic_object_active_type_name':'Map',
        'static_domain_type_name':'JSON.Value','typeOf_object_comparisons':[True,False],
        'JSON_kind_names':['null','bool','int','float','string','array','object'],
        'domain_predicate_results':[True]*7,'kind_object_arm_is_Map_not_structural_object':True,
        'public_full_type_queries_runtime_tags_and_provider_qualified':False}}


def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or _PROGRAMS!=_BOUNDARY_PROGRAMS:
        raise ValueError('JSON boundary model lost a complete source or normative input')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','src','stdlib'}:raise ValueError('JSON boundary input lost exact delivered identity')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('JSON boundary source or normative input changed: '+name)
    values=boundary_observations();traces=boundary_semantic_traces()
    if set(values)!=_NAMES or set(traces)!=_NAMES:raise ValueError('JSON boundary model lost a whole original')
    return [{'id':'complete_json_boundary_'+name,'source':path,'original_source':path,'required_target':None,
        'expected':{'exit_code':0,'stderr_utf8':'','stdout_utf8':''.join((str(value).lower() if type(value) is bool else str(value))+'\n' for value in values[name])},
        'model':'Complete unchanged JSON encoding and active-arm/type queries;independent structural-to-Map materialization,positive typed paths,Set insertion order and disjoint seven-kind predicates. No parse/path error-channel shape or real encoding/FI/resource/lifetime/provider qualification invented.',
        'observation_count':len(values[name]),'required_semantic_trace':traces[name]} for name,path in sorted(_PROGRAMS.items())]
