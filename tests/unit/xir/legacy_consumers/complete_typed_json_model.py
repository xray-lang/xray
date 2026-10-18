"""Independent typed JSON construction and complete original program observations."""
from frozen_derivation_inputs import resolve as resolve_derivation
from pathlib import Path
import hashlib
import json

ROOT=Path(__file__).resolve().parent.parents[3]
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'src/shared/xr_json_type.h': 'fd7a05ef7c2d56e605df6f28edd240a27f1ed15838a1e3eada63160f7697ce7e',
 'stdlib/types/json.xr': '9b4904f85a13bea98b04c616185183f921ed09761bde1cb48d751784fef48161',
 'tests/diff/cases/semantics/json/json_decode_nullable_nested_object.xr': '9c79da06fe511f64d450c40729bdc90547550a1be08ade20c36101188daf3089',
 'tests/diff/cases/semantics/json/json_decode_object_loop_reuse.xr': '275a294385b638084fe96526e157495765a6bd0327b8ab6a5a216f35e9a4ff8d',
 'tests/diff/cases/semantics/json/json_decode_type_matrix.xr': 'e48ca2490509d8e7d09e9a0e8d13b33916b9674a6f542e16f31d6275af3bb52e',
 'tests/diff/cases/semantics/json/json_decode_typed_fields.xr': 'cfe3c0a9e1d4936f3abc5060018a78d1a5f2da15954d897a1196418da4681e15',
 'tests/diff/cases/semantics/json/json_derive_decode.xr': 'bf7ffaf8e219b492ff5a0c34c8d59e8dca65bb708f6e8e5339941b97143b2851',
 'tests/diff/cases/semantics/json/json_derive_struct_encode.xr': '74e960c816adc2824d2d930dd936469fa16a9aea58ba605607002374dc1e9ca8',
 'tests/diff/cases/semantics/json/json_namespace_value_scalar_target.xr': '629e5464f39c60f70765b0d1b67ef3ce422149ccdea919e76ac996d7a428107a',
 'tests/diff/cases/semantics/json/json_parse_typed_direct.xr': '58ecba9b9f528135bdd7deca8d9b564988757477f4ce3e27c7040178e0819413',
 'tests/diff/cases/semantics/json/object_json_codec_bridge.xr': '88ea9528257ae6c3588c9fcb97ebaff2eb1c91ea6f1205d8f68db61133d3f33b'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/016-15-standard-library.md',
 'src/shared/xr_json_type.h',
 'stdlib/types/json.xr',
 'tests/diff/cases/semantics/json/json_decode_nullable_nested_object.xr',
 'tests/diff/cases/semantics/json/json_decode_object_loop_reuse.xr',
 'tests/diff/cases/semantics/json/json_decode_type_matrix.xr',
 'tests/diff/cases/semantics/json/json_decode_typed_fields.xr',
 'tests/diff/cases/semantics/json/json_derive_decode.xr',
 'tests/diff/cases/semantics/json/json_derive_struct_encode.xr',
 'tests/diff/cases/semantics/json/json_namespace_value_scalar_target.xr',
 'tests/diff/cases/semantics/json/json_parse_typed_direct.xr',
 'tests/diff/cases/semantics/json/object_json_codec_bridge.xr')
_PROGRAMS = {'json_decode_nullable_nested_object': 'tests/diff/cases/semantics/json/json_decode_nullable_nested_object.xr',
 'json_decode_object_loop_reuse': 'tests/diff/cases/semantics/json/json_decode_object_loop_reuse.xr',
 'json_decode_type_matrix': 'tests/diff/cases/semantics/json/json_decode_type_matrix.xr',
 'json_decode_typed_fields': 'tests/diff/cases/semantics/json/json_decode_typed_fields.xr',
 'json_derive_decode': 'tests/diff/cases/semantics/json/json_derive_decode.xr',
 'json_derive_struct_encode': 'tests/diff/cases/semantics/json/json_derive_struct_encode.xr',
 'json_namespace_value_scalar_target': 'tests/diff/cases/semantics/json/json_namespace_value_scalar_target.xr',
 'json_parse_typed_direct': 'tests/diff/cases/semantics/json/json_parse_typed_direct.xr',
 'object_json_codec_bridge': 'tests/diff/cases/semantics/json/object_json_codec_bridge.xr'}
_NAMES=frozenset({'json_decode_nullable_nested_object','json_decode_object_loop_reuse',
    'json_decode_type_matrix','json_decode_typed_fields','json_derive_decode',
    'json_derive_struct_encode','json_namespace_value_scalar_target','json_parse_typed_direct','object_json_codec_bridge'})
INVALID=object()


def nullable(schema):return ('nullable',schema)
def array(schema):return ('array',schema)
def mapping(schema):return ('map',schema)


def decode(value,schema,ignore_unknown=False):
    if schema=='any':return value
    if type(schema) is tuple:
        kind,element=schema
        if kind=='nullable':return None if value is None else decode(value,element,ignore_unknown)
        if kind=='array':
            if type(value) is not list:return INVALID
            result=[decode(item,element,ignore_unknown) for item in value]
            return INVALID if any(item is INVALID for item in result) else result
        if kind=='map':
            if type(value) is not dict or any(type(k) is not str for k in value):return INVALID
            result={k:decode(v,element,ignore_unknown) for k,v in value.items()}
            return INVALID if any(v is INVALID for v in result.values()) else result
        raise ValueError('Unknown independent JSON schema')
    if type(schema) is dict:
        if type(value) is not dict or (not ignore_unknown and set(value)-set(schema)):return INVALID
        result={}
        for field,field_schema in schema.items():
            if field not in value:
                if type(field_schema) is tuple and field_schema[0]=='nullable':result[field]=None;continue
                return INVALID
            result[field]=decode(value[field],field_schema,ignore_unknown)
            if result[field] is INVALID:return INVALID
        return result
    if schema=='i64':return value if type(value) is int and -(1<<63)<=value<(1<<63) else INVALID
    if schema=='f64':return value if type(value) is float else INVALID
    if schema=='string':return value if type(value) is str else INVALID
    if schema=='bool':return value if type(value) is bool else INVALID
    raise ValueError('Unknown independent JSON primitive')


def parse(text,schema,ignore_unknown=False):
    value=decode(json.loads(text),schema,ignore_unknown)
    if value is INVALID:raise ValueError('Required independent typed JSON construction failed')
    return value


def compact(value):return json.dumps(value,ensure_ascii=False,separators=(',',':'))


def render(value):
    if type(value) is bool:return str(value).lower()
    if type(value) in (str,int,float):return str(value)
    raise ValueError('Complete JSON traces require explicit scalar observations')


def observations():
    traces={}
    address={'city':'string','zip':'i64'};profile={'name':'string','address':nullable(address)}
    present=parse('{"name":"Dana","address":{"city":"Hangzhou","zip":310000}}',profile)
    null_address=parse('{"name":"Dana","address":null}',profile)
    traces['json_decode_nullable_nested_object']=[present is INVALID,present['address'] is None,present['address']['city'],present['address']['zip'],
        null_address is INVALID,null_address['address'] is None,
        decode(json.loads('{"name":"Dana"}'),profile) is INVALID,
        decode(json.loads('{"name":"Dana","address":42}'),profile) is INVALID,
        decode(json.loads('{"name":"Dana","address":{"city":"Hangzhou","zip":"bad"}}'),profile) is INVALID]
    source=json.loads('{"id":7,"active":true}');total=0
    for _ in range(3):total+=decode(source,{'id':'i64','active':'bool'})['id']
    traces['json_decode_object_loop_reuse']=[total]

    scalars={'i':'i64','f':'f64','s':'string','b':'bool'}
    valid={'i':1,'f':2.5,'s':'hi','b':True};result=decode(valid,scalars)
    values=[result is INVALID,result['i'],result['f'],result['s'],result['b']]
    values.append(decode(valid|{'x':9},scalars) is INVALID)
    for field,bad in [('i',1.5),('f',2),('s',3),('b',1),('i',True)]:values.append(decode(valid|{field:bad},scalars) is INVALID)
    values += [decode({k:v for k,v in valid.items() if k!='b'},scalars) is INVALID,decode(valid|{'i':None},scalars) is INVALID]
    nested={'id':'i64','inner':scalars};result=decode({'id':7,'inner':valid},nested)
    values += [result is INVALID,result['inner']['i'],decode({'id':7,'inner':{k:v for k,v in valid.items() if k!='b'}},nested) is INVALID,decode({'id':7,'inner':42},nested) is INVALID]
    items={'items':array('any')};result=decode({'items':[1,'a',None,{'k':2}]},items)
    values += [result is INVALID,len(result['items']),decode({'items':5},items) is INVALID]
    optional={'name':'string','note':nullable('string')}
    values += [decode(v,optional) is INVALID for v in [{'name':'x','note':None},{'name':'x','note':'n'},{'name':'x'}]]
    values += [decode({'v':v},{'v':'any'}) is INVALID for v in [None,[1,2],{'a':1},True]]
    item={'id':'i64','label':'string'};containers={'ints':array('i64'),'items':array(item),'scores':mapping('i64'),'buckets':mapping(array('i64'))}
    result=parse('{"ints":[1,2,3],"items":[{"id":7,"label":"a"},{"id":8,"label":"b"}],"scores":{"ada":9,"lin":8},"buckets":{"odd":[1,3],"even":[2,4]}}',containers)
    values += [result is INVALID,*[len(result[k]) for k in containers]]
    for field,bad in [('ints',[1,True]),('items',[{'id':7,'label':9}]),('scores',{'ada':True}),('buckets',{'odd':[1,False]})]:
        base={'ints':[],'items':[],'scores':{},'buckets':{}};base[field]=bad;values.append(decode(base,containers) is INVALID)
    traces['json_decode_type_matrix']=values

    user={'name':'string','age':'i64','active':'bool','score':'f64'}
    data={'name':'Ada','age':37,'active':True,'score':9.5};result=decode(data|{'extra':1},user,True)
    values=[result is INVALID,result['name'],result['age'],result['active'],result['score']]
    for field,bad in [('name',1),('age',37.0),('active',1),('score',9)]:values.append(decode(data|{field:bad},user) is INVALID)
    result=decode({'payload':None},{'payload':'any'});values += [result is INVALID,result['payload'] is None]
    result=decode({'items':[1,True,None,{'x':2}]},items);values += [result is INVALID,len(result['items']),decode({'items':1},items) is INVALID]
    traces['json_decode_typed_fields']=values

    user={'name':'string','age':'i64'};envelope={'user':user,'tags':array('string'),'scores':mapping('i64')}
    parsed=parse('{"name":"Ada","age":37}',user);decoded=decode(json.loads('{"name":"Grace","age":42}'),user)
    values=[parsed['name'],parsed['age'],decoded is INVALID,decoded['name'],decoded['age']]
    nested=parse('{"user":{"name":"Lin","age":29},"tags":["compiler","vm"],"scores":{"a":9}}',envelope)
    values += [nested['user']['name'],nested['tags'][1],nested['scores']['a'],decode({'name':'bad','age':True},user) is INVALID]
    point={'x':'i64','y':'i64'};box={'point':point,'label':'string'}
    parsed_point=parse('{"x":3,"y":4}',point);parsed_box=parse('{"point":{"x":8,"y":9},"label":"nested"}',box)
    values += [parsed_point['x'],parsed_point['y'],parsed_box['point']['x'],parsed_box['label'],parsed_box['point']['x'],parsed_box['label'],compact(parsed_box)]
    decoded=decode(json.loads('{"point":{"x":11,"y":12},"label":"decoded"}'),box)
    values += [decoded is INVALID,decoded['point']['y'],decoded['label'],decode({'point':{'x':1},'label':'bad'},box) is INVALID]
    packet={'state':'Ready','note':None};values += [packet['state'],packet['note'] is None,packet['state'],compact(packet)]
    traces['json_derive_decode']=values
    traces['json_derive_struct_encode']=[compact({'i':42,'f':1.5,'b':True,'s':'hi'}),compact({'a':7,'inner':{'v':9}}),compact([{'v':1},{'v':2}])]
    traces['json_namespace_value_scalar_target']=[1,2.5,True,-7]

    geo={'lat':'f64','verified':'bool'};address={'city':'string','geo':geo}
    profile={'name':'string','age':'i64','address':address,'tags':array('any'),'ids':array('i64'),'items':array(item),'scores':mapping('i64'),'buckets':mapping(array('i64'))}
    result=parse('{"ignored":{"deep":[1,2,3]},"buckets":{"odd":[1,3],"even":[2,4]},"scores":{"ada":9},"items":[{"label":"a","id":7}],"ids":[4,5,6],"tags":["ops",3,null],"address":{"geo":{"verified":true,"lat":30.25},"city":"Hangzhou"},"age":37,"name":"Ada"}',profile,True)
    traces['json_parse_typed_direct']=[result['name'],result['age'],result['address']['city'],result['address']['geo']['lat'],result['address']['geo']['verified'],*[len(result[k]) for k in ['tags','ids','items','scores','buckets']]]
    schema={'name':'string','age':'i64','address':{'city':'string','zip':'i64'}}
    original={'name':'Ada','age':37,'address':{'city':'Hangzhou','zip':310000}}
    encoded=json.loads(compact(original));decoded=decode(encoded,schema)
    traces['object_json_codec_bridge']=[encoded['name'],encoded['age'],encoded['address']['city'],encoded['address']['zip'],decoded is INVALID,decoded['name'],decoded['age'],decoded['address']['city'],decoded['address']['zip'],compact(decoded)]
    if set(traces)!=_NAMES:raise ValueError('Typed JSON model lost a complete program')
    return traces


def expected_rows():
    if (set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS))
            or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES)):
        raise ValueError('Typed JSON model lost a reviewed input or program')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','src','stdlib'}:
            raise ValueError('Typed JSON inputs require delivered source identities')
        if hashlib.sha256(resolve_derivation(ROOT, name, digest).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Typed JSON source or authority changed: '+name)
    if not set(_PROGRAMS.values())<=set(_INPUTS):raise ValueError('Typed JSON model lost exact source bindings')
    traces=observations()
    return [{'id':'complete_typed_json_'+name,'source':source,'original_source':source,'required_target':None,
        'expected':{'exit_code':0,'stderr_utf8':'','stdout_utf8':''.join(render(v)+'\n' for v in traces[name])},
        'model':'Full unchanged original typed JSON source; independent schema validation/construction and serialization trace, without constructor effects. No allocation/FI/physical qualification from output.',
        'observation_count':len(traces[name])} for name,source in sorted(_PROGRAMS.items())]
