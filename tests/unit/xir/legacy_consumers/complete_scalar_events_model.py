"""Whole scalar/Unicode/range programs, independent of Xray execution."""
from pathlib import Path
import hashlib,math,re
import independent_observation_format

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': 'db71b9a6b69133a2f2254f6d9abb4d6ed425d15af79af0428f176c946c835c6f',
 'spec/source/sections/019-18-error-codes.md': '2c4416085d5d38f37fb0e434d863cba1ff5d5758d44efb29402fa5158282d9ad',
 'src/shared/xr_error_messages.h': '9cb2a61e702eb0f768e4a9e7fb1a1a3d7176cc603dfb955ca1e2415e6e8c615c',
 'stdlib/math/math.xr': '0126a1eb1db1760b5950da3f6be0c47f504f65070afaff3757f7b45f27a86115',
 'stdlib/text/text.xr': '55d2aaa9e524bc1924f6b426b763f9d45e52f926f204ded84c7e8a64289b9aef',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'tests/diff/cases/basic/range_precedence_arithmetic.xr': 'c37b1ac2830edf5acc992dea969e5fd7de8e35f099fb7134e07363dbadc96496',
 'tests/diff/cases/semantics/float_fmt/ieee_arithmetic.xr': '51e893b8a2f4a9c4b53961072cfa694612970e944768ceacbcb694be32e9aa27',
 'tests/diff/cases/semantics/int/contextual_numeric_conversion.xr': 'b43c1f9cbddc4a544f4ac191fa17baf6f204e5ee1a5eea0a9fe31a662a36018b',
 'tests/diff/cases/semantics/int/enum_ordinal_conversion.xr': '8053495fa5ec36bbde938b5dba5fa17c7ff58e45e46e2bfaf6c84049e39ede23',
 'tests/diff/cases/semantics/loop_invariant_rc_value.xr': '40ce7c5f526dae0c5d5f2abe12a8412cb62591d6ca56ef05f125ebe6d3012502',
 'tests/diff/cases/semantics/string/string_surface_declared_methods.xr': '4d478001a610f741e316ea0cd40ac239858094615e649cbb974b90b78ee53526',
 'tests/diff/cases/semantics/unit_enum_return_value.xr': 'bd7be3c00970c2aad0372ff39211c5341b49406cc3e7d9042035112871e5456d',
 'tests/unit/xir/legacy_consumers/fixtures/enum_explicit_ordinal/root.xr': '3983b3d65a20faaeab294d60490e4fd734e4a3a0a8d5f8443217a14cfcce4e1f',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md',
 'spec/source/sections/019-18-error-codes.md',
 'src/shared/xr_error_messages.h',
 'stdlib/math/math.xr',
 'stdlib/text/text.xr',
 'stdlib/types/array.xr',
 'stdlib/types/string.xr',
 'tests/diff/cases/basic/range_precedence_arithmetic.xr',
 'tests/diff/cases/semantics/float_fmt/ieee_arithmetic.xr',
 'tests/diff/cases/semantics/int/contextual_numeric_conversion.xr',
 'tests/diff/cases/semantics/int/enum_ordinal_conversion.xr',
 'tests/diff/cases/semantics/loop_invariant_rc_value.xr',
 'tests/diff/cases/semantics/string/string_surface_declared_methods.xr',
 'tests/diff/cases/semantics/unit_enum_return_value.xr',
 'tests/unit/xir/legacy_consumers/fixtures/enum_explicit_ordinal/root.xr',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_PROGRAMS = {'contextual_numeric_conversion': 'tests/diff/cases/semantics/int/contextual_numeric_conversion.xr',
 'enum_ordinal_conversion': 'tests/diff/cases/semantics/int/enum_ordinal_conversion.xr',
 'ieee_arithmetic': 'tests/diff/cases/semantics/float_fmt/ieee_arithmetic.xr',
 'loop_invariant_rc_value': 'tests/diff/cases/semantics/loop_invariant_rc_value.xr',
 'range_precedence_arithmetic': 'tests/diff/cases/basic/range_precedence_arithmetic.xr',
 'string_surface_declared_methods': 'tests/diff/cases/semantics/string/string_surface_declared_methods.xr',
 'unit_enum_return_value': 'tests/diff/cases/semantics/unit_enum_return_value.xr'}
_PORTS = {'enum_ordinal_conversion': 'tests/unit/xir/legacy_consumers/fixtures/enum_explicit_ordinal/root.xr'}
_NAMES=frozenset({'ieee_arithmetic','contextual_numeric_conversion','enum_ordinal_conversion','range_precedence_arithmetic',
 'loop_invariant_rc_value','string_surface_declared_methods','unit_enum_return_value'})
_TRANSFORMS=(('print(Ordinal.Ready as u8)','print(Ordinal.Ready.ordinal as u8)'),
 ('print(ready as i64)','print(ready.ordinal as i64)'),('print(boxed_ready as i64)','print(boxed_ready.ordinal as i64)'))

def round_unsigned_to_precision(value,precision):
    if value<0:raise ValueError('This reviewed model expects unsigned input')
    shift=max(0,value.bit_length()-precision)
    if not shift:return value
    whole,remainder=divmod(value,1<<shift);half=1<<(shift-1)
    return (whole+int(remainder>half or (remainder==half and whole%2)))*(1<<shift)

def float_to_unsigned(value,width):
    if not math.isfinite(value):raise ValueError('numeric conversion is out of range')
    truncated=math.trunc(value)
    if not 0<=truncated<1<<width:raise ValueError('numeric conversion is out of range')
    return truncated

def divide(a,b):
    if b==0:
        if a==0:return float('nan')
        return math.copysign(float('inf'),a)*math.copysign(1.0,b)
    return a/b

def optional_int(raw):
    value=raw.strip()
    if not re.fullmatch(r'[+-]?[0-9]+',value):return None
    number=int(value)
    return number if -(1<<63)<=number<1<<63 else None

def optional_float(raw):
    value=raw.strip()
    if not re.fullmatch(r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?',value):return None
    return float(value)

def render(value):
    if value is None:return 'null'
    return independent_observation_format.render({'expected':value})

def numeric_array(values):return '['+', '.join(str(value) for value in values)+']'

def choose(value):return ('Color',('Red','Green','Blue')[0 if value==0 else 1 if value==1 else 2])
def describe(value):return {'Red':100,'Green':200,'Blue':300}[value[1]]

def observations():
    rows={};positive=divide(1.0,0.0);negative=divide(-1.0,0.0);nan_runtime=divide(0.0,0.0);nan_folded=divide(0.0,0.0)
    rows['ieee_arithmetic']=[math.isfinite(positive),positive>1.0,math.isfinite(negative),negative< -1.0,
     math.isnan(nan_runtime),math.isnan(nan_runtime),math.isnan(nan_folded),math.isnan(nan_folded),math.isfinite(divide(1.0,0.0)),math.isfinite(divide(1.0,0.0))]
    maximum=(1<<64)-1;byte_value=255;widened=byte_value;mask=0x85EBCA77;take_u64=lambda value:value
    values=[widened==255,mask==2246822519,take_u64(1)==1,maximum%(1<<32)==4294967295,
     maximum%(1<<32)==4294967295,(-1)%(1<<8)==255,round_unsigned_to_precision(16777217,24)==16777216,
     round_unsigned_to_precision(maximum,53)==1<<64,float_to_unsigned(-0.99,8)==0]
    for value,width in [(256.0,8),(float('nan'),64)]:
        try:float_to_unsigned(value,width)
        except ValueError as error:values.extend([str(error),422])
        else:raise ValueError('The required numeric range failure disappeared')
    rows['contextual_numeric_conversion']=values
    ordinal=lambda declarations,name:declarations.index(name)
    rows['enum_ordinal_conversion']=[ordinal(['Idle','Ready'],'Ready')%(1<<8),ordinal(['Idle','Ready'],'Ready'),ordinal(['Payload','Ready'],'Ready')]
    array=[10,20,30,40];n=3
    rows['range_precedence_arithmetic']=[len(range(1,2*3)),len(range(1,2+3)),len(range(1,2*3+1)),len(range(0,n*2+1)),*[array[i] for i in range(0,len(array)-1)]]
    def bounded(value):return 0 if value>100000 else value
    def width_loop(value,n):
        total=0
        for index in range(n):total+=len(str(value))+index
        return bounded(total)
    def concat_loop(prefix,n):
        total=0
        for index in range(n):total+=len(prefix+'-tail')
        return bounded(total)
    def array_loop(a,b,n):
        total=0
        for index in range(n):total+=len([a,b])
        return bounded(total)
    def nested_loop(value,n):
        total=0
        for i in range(n):
            for j in range(n):total+=len(str(value))
        return bounded(total)
    rows['loop_invariant_rc_value']=[width_loop(7,3),width_loop(1234,4),concat_loop('ab',3),array_loop(1,2,4),nested_loop(42,3)]

    value='héllo';values=[len(value),len(value.encode()),ord(value[0]),ord(value[1]),value.encode()[0],ord('é'),ord('x'),'abc'[::-1],'aé😀'[::-1],optional_int('42'),optional_int(' -7 '),optional_int('x'),optional_float('3.5'),optional_float('nope'),len('')==0,len('a')==0,'self']
    ordinary=bytes([104,105]);accented=bytes([104,195,169]);invalid=bytes([255]);values.extend([ordinary.decode('utf-8'),accented.decode('utf-8')])
    try:invalid.decode('utf-8')
    except UnicodeDecodeError:values.extend([True,'Utf8Error.InvalidUtf8'])
    else:raise ValueError('Strict UTF-8 validation failed to reject')
    values.extend([invalid.decode('utf-8',errors='replace'),numeric_array(list(ordinary)),chr(ord('é')),chr(ord('🙂')),'|'.join(['a','β','中']),''.join(['a','β','中'])])
    sample='Aé中'.encode();values.extend(sample[start:end].decode('utf-8') for start,end in [(0,1),(1,3),(3,6)])
    try:sample[2:3].decode('utf-8')
    except UnicodeDecodeError:values.extend([True,'StringSliceError.InvalidByteRange'])
    else:raise ValueError('A byte slice through a scalar boundary was accepted')
    values.append(numeric_array([1,2,3]));rows['string_surface_declared_methods']=values
    rows['unit_enum_return_value']=[choose(0)==('Color','Red'),choose(1)==('Color','Green'),choose(2)==('Color','Blue'),describe(('Color','Green')),describe(choose(0)),describe(choose(2)),('Color','Green')==('Color','Green'),describe(choose(1))]
    if set(rows)!=_NAMES:raise ValueError('Scalar model lost a complete program')
    return rows

def expected_rows():
    if (set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES
            or len(set(_PROGRAMS.values()))!=len(_NAMES) or set(_PORTS)!={'enum_ordinal_conversion'}):raise ValueError('Scalar model lost exact identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib','src'}:raise ValueError('Scalar model input is not delivered')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Scalar source/authority changed: '+name)
    for source in [*_PROGRAMS.values(),*_PORTS.values()]:
        if source not in _INPUTS:raise ValueError('Scalar model lost a source binding')
    text=(ROOT/_PROGRAMS['enum_ordinal_conversion']).read_bytes().replace(b'\r\n',b'\n').decode()
    for before,after in _TRANSFORMS:
        if text.count(before)!=1:raise ValueError('Ordinal port lost the exact original operation')
        text=text.replace(before,after)
    if (ROOT/_PORTS['enum_ordinal_conversion']).read_bytes().replace(b'\r\n',b'\n')!=text.encode():raise ValueError('Ordinal port changed more than explicit ordinal selection')
    values=observations();rows=[]
    for name,source in sorted(_PROGRAMS.items()):
        if Path(source).stem!=name:raise ValueError('Scalar original identity changed')
        rows.append({'id':'complete_scalar_events_'+name,'source':_PORTS.get(name,source),'original_source':source,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':''.join(render(v)+'\n' for v in values[name]),'stderr_utf8':''},
            'model':'Complete original arithmetic/Unicode/range/call program,explicit enum ordinal port only. Independent exact rounding,typed failure and UTF-8 events;loop outputs are not physical owner proof.',
            'observation_count':len(values[name])})
    return rows
