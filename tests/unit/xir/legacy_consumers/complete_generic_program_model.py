"""Complete original generic-container and iterator behavior obligations.

These values and source identities are independent expected behavior, not
admission of open-generic display, borrowed arguments or protocol lowering.
Every original body and call remains intact. The obsolete erased-origin
fallback in the old comment is not a permitted current specialization plan.
"""
from frozen_derivation_inputs import resolve as resolve_derivation
from dataclasses import dataclass
from pathlib import Path
import hashlib
ROOT=Path(__file__).resolve().parents[4]
BASE='tests/diff/cases/semantics/'
_NAMES=('generic_container_type_args','generic_iterable_constraint')
_PROGRAMS={name:BASE+name+'.xr' for name in _NAMES}
_AUTHORITIES=('spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md','spec/source/sections/006-5-declarations.md',
 'spec/source/sections/010-9-generics.md','spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/014-13-built-in-functions.md','spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md','stdlib/prelude/builtin_symbols.def',
 'stdlib/types/array.xr','stdlib/types/map.xr','stdlib/types/set.xr','stdlib/types/string.xr',
 'stdlib/types/iterator.xr','src/runtime/value/xvalue_format.c','src/shared/xr_value_format_core.h',
 'src/xir/xxir_value_format.inc.c','src/xir/xxir_format.c')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/010-9-generics.md': '246757d531f91e2d19d2cd18353b5ec8addf05d309e5489c10da37e48be81226',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'src/runtime/value/xvalue_format.c': 'aa47cbf6f46222719eb0595cf031eb7f90379cc11799dd74564b19dbb3650863',
 'src/shared/xr_value_format_core.h': '88e14ed234554322f60c1a8694a920c4e7990dd04b4bed1a4ed8d9517a333207',
 'src/xir/xxir_format.c': '8992dc725b2887b34bb0add0f20657d4082a95ae10b35794f124590d9a961949',
 'src/xir/xxir_value_format.inc.c': '2b36fe3e772b898fffcb3124d206c07acbbb4293d0dbf0f4d4af4bae4d48d528',
 'stdlib/prelude/builtin_symbols.def': '7c7e1749ecae49adc12a579614991f34236c02f61497b439cb61073f9893d18c',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/iterator.xr': '251d08d069d868e8487cae10f92e3ead7b601262a1bbc1d3989fb5871c2c7f94',
 'stdlib/types/map.xr': '49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31',
 'stdlib/types/set.xr': '6c8062693701d60982f550ba8b766b4c20924c9ed36cb0dc146be45f9693db8f',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'tests/diff/cases/semantics/generic_container_type_args.xr': 'f9c48b5c518187c61d97c722d3cee0e95393fa5ec67a1da09bfa3e50e1c7c80e',
 'tests/diff/cases/semantics/generic_iterable_constraint.xr': '64c5612e9b6bf00193efd0ca8c195f17412862f98c23935694c2f693281fbfef'}
I64=('i64',);STRING=('string',);BOOL=('bool',);RANGE=('Range',);JSON_VALUE=('JSON.Value',)
array=lambda item:('Array',item)
slice_type=lambda item:('Slice',item)
mapping=lambda key,value:('Map',key,value)
set_type=lambda item:('Set',item)
iterator=lambda item:('Iterator',item)


@dataclass(frozen=True)
class Value:
    type: tuple
    payload: object


def function_type(parameters,result,*,modes=None,throws=(),suspends=False):
    modes=tuple(modes or ('READ',)*len(parameters))
    if len(modes)!=len(parameters):raise ValueError('Full function signature lost parameter modes')
    return ('Function',tuple(zip(modes,parameters)),result,tuple(throws),suspends)


def instance_key(module,declaration,arguments):
    # This is a semantic source identity, never a compiler id or mangled name.
    if not module or not declaration or not arguments:raise ValueError('Incomplete exact generic instance')
    def contains_parameter(value):
        return type(value) is tuple and (bool(value) and value[0]=='Parameter' or any(contains_parameter(x) for x in value))
    if any(contains_parameter(t) for t in arguments):raise ValueError('Ordinary sealed instance requires concrete type arguments')
    return (module,declaration,tuple(arguments))


def text(value,depth=0):
    kind=value.type[0];payload=value.payload
    if kind=='string':return '"'+payload+'"' if depth else payload
    if kind=='i64':return str(payload)
    if kind=='bool':return str(payload).lower()
    if kind in ('Array','Slice','Set','Tuple'):
        prefix,suffix=('#[',']') if kind=='Set' else ('(',')') if kind=='Tuple' else ('[',']')
        return prefix+', '.join(text(x,depth+1) for x in payload)+(',' if kind=='Tuple' and len(payload)==1 else '')+suffix
    if kind=='Map':return '#{'+', '.join(text(k,depth+1)+': '+text(v,depth+1) for k,v in payload)+'}'
    if kind=='Object':return '{'+', '.join(k+': '+text(v,depth+1) for k,v in payload)+'}'
    if kind=='Range':
        first,last,inclusive=payload;return str(first)+('..=' if inclusive else '..')+str(last)
    raise ValueError('No original value display obligation for this type')


def byte_length(value):return len(value.encode('utf-8'))


def current_closed_display(type):
    kind=type[0]
    if kind in ('i64','string','bool','rune','f32','f64','Unit'):return True
    if kind in ('Array','Nullable'):return current_closed_display(type[1])
    # Open parameters, functions, Map/Set, structural objects and other
    # unadmitted types gain no display proof from the pure expected value.
    return False


def guarded_sum(terms):
    acc=0;steps=[]
    for term in terms:
        before=acc;acc+=term;steps.append((before,term,acc))
    return (0 if acc>100000 else acc),steps


def first_or(items,default):
    if items:return items[0]
    return default


def container_trace():
    module=_PROGRAMS[_NAMES[0]];calls=[]
    integer=lambda value:Value(I64,value);string=lambda value:Value(STRING,value)
    ints=Value(array(I64),tuple(map(integer,[1,2,3])))
    words=Value(array(STRING),tuple(map(string,['ab','cd'])))
    scores=Value(mapping(STRING,I64),((string('k'),integer(1)),))
    letters=Value(set_type(STRING),tuple(map(string,['a','b'])))
    doc=Value(('Object',(('a',I64),)),(('a',integer(1)),))
    interval=Value(RANGE,(1,5,False));number=integer(42);word=string('xy')
    view=Value(slice_type(I64),ints.payload[1:3])
    def call(name,type,result,**facts):
        calls.append({'declaration':name,'type_argument':type,'instance':instance_key(module,name,(type,)),
            'result':result,'facts':facts,'current_program_admission':'OPEN_REQUIRES_WHOLE_SOURCE_MIGRATION_AND_PROOF'})
        return result
    outputs=[]
    for value in [ints,words,scores,letters,doc,interval,number,word]:
        rendered=text(value);outputs.append(call('tagOf',value.type,byte_length(rendered),complete_text=rendered))
    for value in [view,ints]:outputs.append(call('bumpFor',value.type,4+1,original_k=4,argument_inspected=False))
    outputs += [call('firstOr',I64,first_or(ints.payload,integer(0)).payload,nonempty=True),
        call('firstOr',STRING,first_or(words.payload,string('z')).payload,nonempty=True)]
    pair=Value(('Tuple',I64,I64),(integer(1),integer(2)))
    mixed=Value(('Tuple',I64,STRING,BOOL),(integer(1),string('z'),Value(BOOL,True)))
    for value,n in [(ints,3),(pair,3),(mixed,2),(scores,2)]:
        tag=byte_length(text(value));result,steps=guarded_sum(tag+i for i in range(n))
        outputs.append(call('widthOf',value.type,result,n=n,complete_text=text(value),loop_steps=steps,guard=100000))
    fn=function_type((I64,),I64)
    for type in [fn,I64]:
        result,steps=guarded_sum(range(3));outputs.append(call('rounds',type,result,n=3,loop_steps=steps,
            guard=100000,argument_inspected=False,helper_calls=0,erased_origin_fallback_allowed=False))
    return {'outputs':outputs,'calls':calls,'original_arithmetic_loop_iterations':sum(len(x['facts'].get('loop_steps',())) for x in calls),
        'original_view_values':[x.payload for x in view.payload],'helper_invocations':0,
        'current_closed_display_rejected_types':[value.type for value in [scores,letters,doc,interval,pair,mixed]],
        'open_parameter_display_admitted':current_closed_display(('Parameter','T')),
        'current_erased_origin_fallback_allowed':False}


def iterable_element(type):
    kind=type[0]
    if kind in ('Array','Slice','Set','Iterator'):return type[1]
    if kind=='Range':return I64
    if kind=='Map':return type[1]
    return None


def lengthable(type):return type[0] in ('Array','Slice','Set','Map','Range','string')


class Cursor:
    def __init__(self,values):self.values=tuple(values);self.position=0;self.next_calls=0
    def next(self):
        self.next_calls+=1
        if self.position==len(self.values):return None
        item=self.values[self.position];self.position+=1;return item
    def __iter__(self):
        while True:
            value=self.next()
            if value is None:return
            yield value


def up_to(limit):
    i=1;values=[];conditions=[]
    while True:
        conditions.append(i<=limit)
        if not conditions[-1]:break
        values.append(i);i+=1
    return values,conditions


def reduce_items(items,*,keys=False,indexed=False):
    n=0;steps=[]
    for index,item in enumerate(items):
        before=n;n += byte_length(item) if keys else item+index if indexed else item
        steps.append((index,item,before,n))
    return n,steps


def iterable_trace():
    module=_PROGRAMS[_NAMES[1]];calls=[];outputs=[]
    map_type=mapping(STRING,I64);json_object_type=mapping(STRING,JSON_VALUE)
    generated,conditions=up_to(4);cursor=Cursor(generated)
    def total(type,items):
        if iterable_element(type)!=I64:raise ValueError('Original total requires Iterable<i64>')
        result,steps=reduce_items(items);record('total',type,result,loop_steps=steps)
    def record(name,type,result,**facts):
        outputs.append(result);calls.append({'declaration':name,'type_argument':type,'instance':instance_key(module,name,(type,)),
            'result':result,'facts':facts,'current_protocol_source_admission':'OPEN'})
    for type,items in [(array(I64),[1,2,3]),(set_type(I64),[1,2,3]),(RANGE,range(1,5)),(RANGE,range(2,6)),(iterator(I64),cursor)]:
        total(type,items)
    for type,items in [(map_type,['aa','bbb']),(json_object_type,['a','bb'])]:
        if iterable_element(type)!=STRING:raise ValueError('Original keyLengths requires Iterable<string>')
        result,steps=reduce_items(items,keys=True);record('keyLengths',type,result,loop_steps=steps)
    result,steps=reduce_items([10,20],indexed=True);record('indexedSum',array(I64),result,loop_steps=steps)
    for type,result in [(array(I64),3),(STRING,byte_length('hey')),(map_type,2),(json_object_type,2),(RANGE,len(range(1,4))),(RANGE,len(range(2,6)))]:
        if not lengthable(type):raise ValueError('Original sizeOf requires Lengthable')
        record('sizeOf',type,result)
    return {'outputs':outputs,'calls':calls,'original_for_body_iterations':sum(len(x['facts'].get('loop_steps',())) for x in calls),
        'generator_values':generated,'generator_while_conditions':conditions,'generator_next_calls':cursor.next_calls,
        'generator_consumed_values':cursor.position,'generator_arithmetic_iterations':len(generated),
        'JSON_Object_canonical_type':json_object_type,'JSON_Value_is_iterable':iterable_element(JSON_VALUE) is not None,
        'JSON_Value_is_lengthable':lengthable(JSON_VALUE),'Channel_is_iterable':iterable_element(('Channel',I64)) is not None}


def traces():return {_NAMES[0]:container_trace(),_NAMES[1]:iterable_trace()}


def _validate():
    if set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()) or len(_PROGRAMS)!=2:
        raise ValueError('Complete generic source/authority identities changed')
    for path,digest in _INPUTS.items():
        if hashlib.sha256(resolve_derivation(ROOT, path, digest).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete generic original or authority changed: '+path)


def expected_rows():
    _validate();values=traces()
    return [{'id':'complete_generic_program_'+name,'source':_PROGRAMS[name],'required_target':None,
        'expected':{'exit_code':0,'stdout_utf8':''.join(str(value)+'\n' for value in values[name]['outputs']),'stderr_utf8':''},
        'observation_count':len(values[name]['outputs']),'required_semantic_trace':values[name],
        'model':'Complete original generic behavior with every container/tuple/Slice/function type and loop retained. Independent source-declaration/full-type instance keys and iterable reductions do not grant current open-generic display,static protocols,borrow ABI,erased fallback or actual monomorphization. All complete source migration/FI/resource/lifetime/safety/provider obligations remain OPEN.'}
        for name in _NAMES]
