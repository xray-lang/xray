"""Independent full-program error channels and static cleanup event model."""
from pathlib import Path
import hashlib

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'src/shared/xr_error_messages.h': '9cb2a61e702eb0f768e4a9e7fb1a1a3d7176cc603dfb955ca1e2415e6e8c615c',
 'tests/diff/cases/semantics/exception/array_class_throw_cleanup_lifetime.xr': 'af3842d5ee1663c94b93671d0edee7850e3767a936f86cd3c009234b214eeeba',
 'tests/diff/cases/semantics/exception/core_assertion_channels.xr': 'f091e1aace0dc1577ba8e7382f53dea7a2d06153a990c6f932325091f8ce71f3',
 'tests/diff/cases/semantics/exception/defer_nested_function_call.xr': '2321c17bc3eba654f8f8fc42c555236f1172e78374e86cfc340db0f3b623e34e',
 'tests/diff/cases/semantics/exception/div_by_zero.xr': '08959557d13b4bd77004bc9fbc96940ebf9e2b73ad4915f61f450bc6bb4fb6e1',
 'tests/diff/cases/semantics/exception/nested_defer.xr': '589b86092b61fa5336260a0ab4a61a9f31f15bf4c559f7a3a9e96ce468b04f0a',
 'tests/diff/cases/semantics/exception/panic_edge_owner_cleanup.xr': '1861982b7bffd551ca16fd285e4c308437edb5083d20e03ec8878b02ce441e35',
 'tests/diff/cases/semantics/exception/try_catch.xr': '236b77b4ce53ab2ee25464f7771e513876ee74bdb10b9fd8193f4993f1136a60',
 'tests/diff/cases/semantics/exception/uncaught_top_level_error.xr': 'b8b3d4b85410a28e7b3adbb5df44ad675f51e3afca1498b4f298ed84867aa2b0'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'src/shared/xr_error_messages.h',
 'tests/diff/cases/semantics/exception/array_class_throw_cleanup_lifetime.xr',
 'tests/diff/cases/semantics/exception/core_assertion_channels.xr',
 'tests/diff/cases/semantics/exception/defer_nested_function_call.xr',
 'tests/diff/cases/semantics/exception/div_by_zero.xr',
 'tests/diff/cases/semantics/exception/nested_defer.xr',
 'tests/diff/cases/semantics/exception/panic_edge_owner_cleanup.xr',
 'tests/diff/cases/semantics/exception/try_catch.xr',
 'tests/diff/cases/semantics/exception/uncaught_top_level_error.xr')
_PROGRAMS = {'array_class_throw_cleanup_lifetime': 'tests/diff/cases/semantics/exception/array_class_throw_cleanup_lifetime.xr',
 'core_assertion_channels': 'tests/diff/cases/semantics/exception/core_assertion_channels.xr',
 'defer_nested_function_call': 'tests/diff/cases/semantics/exception/defer_nested_function_call.xr',
 'div_by_zero': 'tests/diff/cases/semantics/exception/div_by_zero.xr',
 'nested_defer': 'tests/diff/cases/semantics/exception/nested_defer.xr',
 'panic_edge_owner_cleanup': 'tests/diff/cases/semantics/exception/panic_edge_owner_cleanup.xr',
 'try_catch': 'tests/diff/cases/semantics/exception/try_catch.xr',
 'uncaught_top_level_error': 'tests/diff/cases/semantics/exception/uncaught_top_level_error.xr'}
_NAMES=frozenset({'array_class_throw_cleanup_lifetime','core_assertion_channels',
 'defer_nested_function_call','div_by_zero','nested_defer','panic_edge_owner_cleanup',
 'try_catch','uncaught_top_level_error'})

class TypedError(Exception):
    def __init__(self,enum,variant,**payload):
        self.enum,self.variant,self.payload=enum,variant,payload

class Panic(Exception):pass

def assertion_channel(required,action):
    try:action()
    except TypedError:
        if required!='error':raise Panic('assertion channel mismatch')
        return
    except Panic:
        if required!='panic':raise Panic('assertion channel mismatch')
        return
    raise Panic('assertion action returned normally')

def observations():
    result={}
    def parse_function():raise TypedError('ParseError','Invalid',code=42)
    def parse_sections():
        values=[]
        # Evaluate the argument before committing an Array append.
        values.append(parse_function())
        return values
    caught=False
    try:parse_sections()
    except TypedError as error:caught=error.enum=='ParseError' and error.variant=='Invalid' and error.payload['code']==42
    result['array_class_throw_cleanup_lifetime']=[caught]

    counts={'action':0,'message':0}
    def message():counts['message']+=1;return 'evaluated'
    def typed_action():counts['action']+=1;raise TypedError('AssertionProbeError','Expected')
    def panic_action():counts['action']+=1;raise Panic('division by zero')
    def plain_typed():raise TypedError('AssertionProbeError','Expected')
    def plain_panic():raise Panic('division by zero')
    # Conditions/action expressions and messages evaluate in source order;
    # messages are evaluated on successful assertions too (spec 13.3).
    condition=True;message();assert condition
    left,right=7,7;message();assert left==right
    action=typed_action;message();assertion_channel('error',action)
    action=panic_action;message();assertion_channel('panic',action)
    for required,wrong in [('error',plain_panic),('panic',plain_typed),('error',lambda:None),('panic',lambda:None)]:
        assertion_channel('panic',lambda required=required,wrong=wrong:assertion_channel(required,wrong))
    assert counts=={'action':2,'message':4}
    result['core_assertion_channels']=['assertion-channels-ok']

    events=[]
    def worker():
        try:events.append('body')
        finally:events.append('nested-cleanup')
    worker();events.append('done');result['defer_nested_function_call']=events
    result['div_by_zero']=[]

    events=[]
    def inner():
        try:
            events.append('inner-body');raise TypedError('AppErr','Msg',s='boom')
        finally:events.append('inner-defer')
    def outer():
        try:
            try:inner();return 'no-throw'
            except TypedError as error:return 'caught:'+error.payload['s']
        finally:events.append('outer-defer')
    value=outer();events.extend([value,'done']);result['nested_defer']=events

    owner=[1,2,3];events=[owner[0]]
    try:
        if 9>=len(owner):raise Panic('out of bounds')
        events.append(owner[9])
    except Panic:events.extend([owner[1],'caught'])
    result['panic_edge_owner_cleanup']=events

    events=[]
    try:
        events.append('try');raise TypedError('AppErr','Msg',s='boom')
        events.append('unreachable')
    except TypedError as error:events.append('caught:'+error.payload['s'])
    events.append('after');result['try_catch']=events
    result['uncaught_top_level_error']=['before']
    if set(result)!=_NAMES:raise ValueError('Complete exception model lost a program')
    return result

def termination(name):
    if name=='div_by_zero':return 1,'[Uncaught Panic] E0420: division by zero\n'
    if name=='uncaught_top_level_error':return 1,'[Uncaught Error] TopErr.Failed\n'
    return 0,''

def expected_rows():
    if (set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS))
            or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES)):
        raise ValueError('Exception model lost reviewed source identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','src'}:
            raise ValueError('Exception model requires delivered source identities')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Exception source or authority changed: '+name)
    for name,path in _PROGRAMS.items():
        if path!='tests/diff/cases/semantics/exception/'+name+'.xr' or path not in _INPUTS:
            raise ValueError('Exception program identity differs from its original source')
    values=observations();rows=[]
    for name,source in sorted(_PROGRAMS.items()):
        code,stderr=termination(name)
        rows.append({'id':'complete_exception_'+name,'source':source,'original_source':source,'required_target':None,
            'expected':{'exit_code':code,'stdout_utf8':''.join((str(v).lower() if type(v) is bool else str(v))+'\n' for v in values[name]),'stderr_utf8':stderr},
            'model':'Complete original error/cleanup body; independent typed-error/panic separation and ordered cleanup events. Observable owners are not physical reclamation evidence.',
            'observation_count':len(values[name])})
    return rows
