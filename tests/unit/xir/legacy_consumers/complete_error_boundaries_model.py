"""Complete legal bounds and strict decimal parse programs, without execution feedback."""
from pathlib import Path
from decimal import Decimal
import hashlib,math,re

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '84e641b43472a119c2e1ef1cfb89c4fb0b9cffb3d47d85f21e99bf0cd00b056e',
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
    return rows
