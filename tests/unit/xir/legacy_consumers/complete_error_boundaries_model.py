"""Complete legal bounds and strict decimal parse programs, without execution feedback."""
from pathlib import Path
from decimal import Decimal
import hashlib,math,re

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'spec/source/sections/019-18-error-codes.md': '2c4416085d5d38f37fb0e434d863cba1ff5d5758d44efb29402fa5158282d9ad',
 'src/shared/xr_error_core.h': 'f043eaa99f5ea9abf89b9deefc17d8b10e984ee049d22f1128a6bed7bd90b5e0',
 'src/shared/xr_error_messages.h': '9cb2a61e702eb0f768e4a9e7fb1a1a3d7176cc603dfb955ca1e2415e6e8c615c',
 'src/shared/xr_float_fmt.h': 'dc3f35e72dd96729e5948f98b8103810d1362bad8cec207aabf93db1352f983b',
 'src/shared/xr_string_parse_core.h': 'e96e12444c85ef8d64e2b6ab0baa8b8e761c7f3636049f9f10420fe6184ad5a1',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/f64.xr': '16f8b34079a8ef926027abf5a73c12e2e5601e692c027db00aca622a850cf16c',
 'stdlib/types/i64.xr': '2d9a160b70394a77ec69dccdf7b895ecd5ac7e66548824a835d3eb0a7cd45567',
 'tests/diff/cases/semantics/exception/array_index_set_append_oob.xr': '0bb8b98e2c1723ec84d8cb18011a7986f5aef6917eb14e12153cc44e990e7847',
 'tests/diff/cases/semantics/exception/array_index_set_gap_oob.xr': '6ffcf8339f9fb0507af5a721f9c1514c82dee4373a9b7238162e257ec19a80ad',
 'tests/diff/cases/semantics/exception/array_index_set_negative_oob.xr': '312f28b821ff40229d55b5278cf900daebf86f74e135adda7daab80489390e00',
 'tests/diff/cases/semantics/exception/fixed_array_index_oob_panic_shape.xr': '5cbbcd5337c2e101d9429ea03e7c7c6202282024fbdfd6e6dfd94186a2dcc8e3',
 'tests/diff/cases/semantics/exception/string_number_conversion_panic_shape.xr': 'e7c56c4bb31ab8f064e1da783e5ff766a5c35676d1c8bb8e3bf2258f1b80f0bf'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md',
 'spec/source/sections/019-18-error-codes.md',
 'src/shared/xr_error_core.h',
 'src/shared/xr_error_messages.h',
 'src/shared/xr_float_fmt.h',
 'src/shared/xr_string_parse_core.h',
 'stdlib/types/array.xr',
 'stdlib/types/f64.xr',
 'stdlib/types/i64.xr',
 'tests/diff/cases/semantics/exception/array_index_set_append_oob.xr',
 'tests/diff/cases/semantics/exception/array_index_set_gap_oob.xr',
 'tests/diff/cases/semantics/exception/array_index_set_negative_oob.xr',
 'tests/diff/cases/semantics/exception/fixed_array_index_oob_panic_shape.xr',
 'tests/diff/cases/semantics/exception/string_number_conversion_panic_shape.xr')
_PROGRAMS = {'array_index_set_append_oob': 'tests/diff/cases/semantics/exception/array_index_set_append_oob.xr',
 'array_index_set_gap_oob': 'tests/diff/cases/semantics/exception/array_index_set_gap_oob.xr',
 'array_index_set_negative_oob': 'tests/diff/cases/semantics/exception/array_index_set_negative_oob.xr',
 'fixed_array_index_oob_panic_shape': 'tests/diff/cases/semantics/exception/fixed_array_index_oob_panic_shape.xr',
 'string_number_conversion_panic_shape': 'tests/diff/cases/semantics/exception/string_number_conversion_panic_shape.xr'}
_NAMES=frozenset({'array_index_set_append_oob','array_index_set_gap_oob','array_index_set_negative_oob',
 'fixed_array_index_oob_panic_shape','string_number_conversion_panic_shape'})

class ParseFailure(Exception):
    def __init__(self,variant):self.variant=variant;super().__init__(variant)

def parse_integer(text):
    token=text.strip(' \t\r\n\v\f')
    if re.fullmatch(r'[+-]?[0-9]+',token) is None:raise ParseFailure('InvalidSyntax')
    value=int(token)
    if not -(1<<63)<=value<1<<63:raise ParseFailure('OutOfRange')
    return value

def parse_float(text):
    token=text.strip(' \t\r\n\v\f')
    if re.fullmatch(r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?',token) is None:raise ParseFailure('InvalidSyntax')
    # Only the exact finite sample and unambiguous overflow are observed here.
    # Decimal grammar/scale is independent of the compiler/shared C parser.
    return Decimal(token)

def render_float(value):
    if value.copy_abs()>Decimal(2)**1024:return '-inf' if value.is_signed() else 'inf'
    if value==value.to_integral_value():return format(value.quantize(Decimal(1)),'f')+'.0'
    return format(value,'f')

def checked_index(length,index,fixed=False):
    if 0<=index<length:return None
    return ('fixed array' if fixed else 'array')+' index out of range: '+str(index)+' (length '+str(length)+')',430

def observations():
    values=[]
    for index in [-1,4,5]:values.extend(checked_index(4,index,True))
    rows={'fixed_array_index_oob_panic_shape':values}
    values=[parse_integer('  -7 ')]
    for token,prefix in [('12abc','i64'),('9223372036854775808','overflow')]:
        try:parse_integer(token)
        except ParseFailure as error:values.append(prefix+'.'+error.variant)
        else:raise ValueError('Strict integer parse lost its error')
    values.extend(render_float(parse_float(token)) for token in ['1.5e3','1e400'])
    try:parse_float('1e+')
    except ParseFailure as error:values.append('f64.'+error.variant)
    else:raise ValueError('Strict decimal parse accepted incomplete exponent')
    for parser in [parse_integer,parse_float]:
        try:parser('not-a-number')
        except ParseFailure:values.append(True)
        else:raise ValueError('Optional parse must return an absent result')
    rows['string_number_conversion_panic_shape']=values
    for name,index in [('array_index_set_append_oob',3),('array_index_set_gap_oob',5),('array_index_set_negative_oob',-1)]:
        message,code=checked_index(3,index)
        rows[name]=['[Uncaught Panic] E'+str(code).zfill(4)+': '+message]
    rows.update({name:trace['outputs'] for name,trace in map_boundary_traces().items()})
    if set(rows)!=_NAMES:raise ValueError('Error boundary model lost a whole program')
    return rows

def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES):raise ValueError('Error boundary identity set changed')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib','src'}:raise ValueError('Error boundary input is not delivered')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Error boundary source or authority changed: '+name)
    rows=[]
    for name,values in sorted(observations().items()):
        source=_PROGRAMS[name]
        if Path(source).stem!=name or source not in _INPUTS:raise ValueError('Error boundary exact source identity changed')
        uncaught=name.startswith('array_index_set_')
        text=''.join((str(v).lower() if type(v) is bool else str(v))+'\n' for v in values)
        rows.append({'id':'complete_error_boundaries_'+name,'source':source,'original_source':source,'required_target':None,
         'expected':{'exit_code':1 if uncaught else 0,'stdout_utf8':'' if uncaught else text,'stderr_utf8':text if uncaught else ''},
         'model':'Complete original source: independent bounds/code/message/top-level panic formatting or strict decimal grammar/typed failure;no old expected or product-output oracle.',
         'observation_count':len(values)})
        if name in _MAP_PROGRAMS:
            rows[-1]['required_semantic_trace']=map_boundary_traces()[name]
            rows[-1]['model']='Complete unchanged strict Map and optional/nullable presence programs;spec E0431 and fixed diagnostic acceptance text,all print continuations and catch order preserved. Static/prehashed storage is a required product gate,not established by the independent value model.'
    return rows


_MAP_PROGRAMS={name:'tests/diff/cases/semantics/collections/'+name+'.xr' for name in (
    'map_index_missing_key_panic','map_set_static_missing_key_prehashed')}
_MAP_NAMES=frozenset({'map_index_missing_key_panic','map_set_static_missing_key_prehashed'})
_MAP_INPUTS={"tests/diff/cases/semantics/collections/map_index_missing_key_panic.xr":"da740a26e3a592a62251b4114cea9d09c603e82e871855743a1cd5fa37da9f8f","tests/diff/cases/semantics/collections/map_set_static_missing_key_prehashed.xr":"e8e3b30e380386e0afc58578202fe50102d4fa12b82ae5850af53dcf6196eac7","stdlib/types/map.xr":"49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31","stdlib/types/set.xr":"6c8062693701d60982f550ba8b766b4c20924c9ed36cb0dc146be45f9693db8f","src/runtime/xerror_codes.h":"c57c353946012402931711b2b736013c8b5208fcb5de22370a230cc75e0f8a56","src/aot/xrt_coll.h":"a9607a14e582818c5c56e9bff9f5ac4fba91daed7c741d2bcd0631fdd1f8e96e"}
_PROGRAMS.update(_MAP_PROGRAMS)
_NAMES=_NAMES|_MAP_NAMES
_INPUTS.update(_MAP_INPUTS)
_REQUIRED_INPUTS=_REQUIRED_INPUTS+tuple(name for name in _MAP_INPUTS if name not in _REQUIRED_INPUTS)


class MissingMapKey(Exception):
    code=431
    message='Map key not found'


class StringKeyMap:
    """Presence and nullable payload are distinct; no target hash layout modeled."""
    def __init__(self,items):
        if any(type(key) is not str for key in items):raise ValueError('Only original string key identities are reviewed')
        self.items=dict(items)
    def contains(self,key):return key in self.items
    def optional(self,key):return self.items.get(key)
    def strict(self,key):
        if key not in self.items:raise MissingMapKey()
        return self.items[key]


def _caught_missing(mapping,code_first=False):
    outputs=[];bad_continuation=False
    try:
        # Evaluating the print argument must finish before any output is emitted.
        outputs.append(mapping.strict('missing'))
        bad_continuation=True;outputs.append('bad-index')
    except MissingMapKey as panic:
        outputs.extend([panic.code,panic.message] if code_first else [panic.message,panic.code])
    else:raise ValueError('Missing strict Map index did not interrupt its complete print continuation')
    if bad_continuation:raise ValueError('Original bad-index continuation executed')
    return outputs,bad_continuation


def map_boundary_traces():
    if set(_MAP_PROGRAMS)!=_MAP_NAMES:raise ValueError('Complete strict Map boundary program missing')
    scores=StringKeyMap({'ada':7,'lin':9});before=dict(scores.items)
    outputs=[scores.strict('ada')];caught,bad=_caught_missing(scores);outputs+=caught
    outputs.append(scores.optional('missing') is None)
    payloads=StringKeyMap({'present-null':None,'present-value':5})
    present=payloads.contains('present-null')
    if present:outputs.append(payloads.strict('present-null') is None)
    outputs.append(payloads.optional('absent') is None)
    if not present or scores.items!=before or outputs!=[7,'Map key not found',431,True,True,True]:
        raise ValueError('Complete optional/nullable strict Map boundary requirement changed')
    ordinary={'outputs':outputs,'bad_index_continuation_executed':bad,'stored_null_is_present':present,
        'stored_null_payload':payloads.strict('present-null'),'absent_is_present':payloads.contains('absent'),
        'scores_after_caught_panic':dict(scores.items),'nullable_payloads_after_reads':dict(payloads.items)}
    static_scores=StringKeyMap({'ada':7,'lin':9});seen=frozenset(['ada','lin'])
    output=[static_scores.optional('missing') is None,'missing' in seen,static_scores.strict('ada'),'lin' in seen]
    caught,bad=_caught_missing(static_scores,code_first=True);output+=caught
    if output!=[True,False,7,True,431,'Map key not found'] or static_scores.items!=before:
        raise ValueError('Complete top-level readonly Map/Set strict lookup requirement changed')
    static={'outputs':output,'bad_index_continuation_executed':bad,'scores_after_caught_panic':dict(static_scores.items),
        'seen_after_reads':sorted(seen),'original_top_level_const_map_entries':2,'original_top_level_const_set_entries':2,
        'actual_static_prehashed_emission_or_native_readonly_storage':'OPEN_REQUIRES_REAL_VM_C_NATIVE_AND_RESOURCE_QUALIFICATION'}
    return {'map_index_missing_key_panic':ordinary,'map_set_static_missing_key_prehashed':static}
