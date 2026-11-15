"""Retained nominal field/nullable/JSON and typed input observations.

Field copies require an actual declaration identity. JSON schema decoding is an
independent boundary; it grants no nominal type, ownership or product proof.
Removed object-destructure syntax is not modeled as a current language feature.
"""
from task342_responsibility_migration import verify_input
from dataclasses import dataclass, fields
from pathlib import Path
import copy
import hashlib
import json

ROOT = Path(__file__).resolve().parents[4]
AUTHORITIES = (
    'spec/source/sections/003-2-type-system.md',
    'spec/source/sections/004-3-expressions.md',
    'spec/source/sections/006-5-declarations.md',
    'spec/source/sections/015-14-built-in-type-methods.md',
    'spec/source/sections/018-17-compilation-pipeline.md',
    'stdlib/types/string.xr', 'stdlib/mem/mem.xr', 'stdlib/ws/ws.xr', 'src/xir/xxir_value.h',
)
PATHS = {
    'object_width_json_as_extra_key': 'tests/diff/cases/semantics/object/object_width_json_as_extra_key.xr',
    'object_width_nullable_field_omission': 'tests/diff/cases/semantics/object/object_width_nullable_field_omission.xr',
    'object_destructure': 'tests/diff/cases/semantics/object/object_destructure.xr',
    'tbaa_raw_pointer_store_kills_array_load': 'tests/diff/cases/semantics/optimizer/tbaa_raw_pointer_store_kills_array_load.xr',
    'ws_connect_options_type_surface': 'tests/diff/cases/semantics/stdlib/ws_connect_options_type_surface.xr',
    'string_from_utf8_slice': 'tests/diff/cases/semantics/slice/string_from_utf8_slice.xr',
    'slice_reinterpret_value_struct': 'tests/diff/cases/semantics/slice/slice_reinterpret_value_struct.xr',
}


def decode_schema_fields(fields, schema, *, ignore_unknown=False):
    """Independent JSON schema duty, separate from nominal declaration identity."""
    if type(fields) is not dict:raise ValueError('JSON object required')
    if not ignore_unknown and set(fields) - set(schema):raise ValueError('unknown JSON field')
    result={}
    for name,(kind,nullable) in schema.items():
        if name not in fields:
            if not nullable:raise ValueError('missing required JSON field')
            result[name]=None
            continue
        value=fields[name]
        if value is None and nullable:result[name]=None
        elif type(value) is not kind or (kind is int and not -(1<<63)<=value<(1<<63)):
            raise ValueError('wrong JSON field type')
        else:result[name]=copy.deepcopy(value)
    return result


def i64(value):
    if type(value) is not int or not -(1<<63)<=value<(1<<63):raise ValueError('i64 field required')
    return value


@dataclass
class Point:
    x: int
    y: int
    def __post_init__(self):i64(self.x);i64(self.y)


@dataclass
class Opt:
    name: str
    age: int | None=None
    def __post_init__(self):
        if type(self.name) is not str:raise ValueError('Opt.name requires string')
        if self.age is not None:i64(self.age)


@dataclass
class User:
    name: str
    age: int
    tags: list
    def __post_init__(self):
        if type(self.name) is not str or type(self.tags) is not list or any(type(x) is not str for x in self.tags):
            raise ValueError('User fields require declared types')
        i64(self.age)


@dataclass
class InnerValue:
    k: int
    def __post_init__(self):i64(self.k)


@dataclass
class OuterValue:
    id: int
    inner: InnerValue
    def __post_init__(self):
        i64(self.id)
        if type(self.inner) is not InnerValue:raise ValueError('InnerValue declaration identity required')


def field_copy(value,*names):
    if type(value) not in (Point,Opt,User,InnerValue,OuterValue):
        raise ValueError('nominal declaration identity required')
    declared={field.name for field in fields(value)}
    if any(name not in declared for name in names):raise ValueError('undeclared nominal field')
    return tuple(copy.deepcopy(getattr(value,name)) for name in names)


def clobbered_array_loads(count):
    storage = bytearray(8)
    storage[0] = 1
    pointer = memoryview(storage)
    total = 0
    for index in range(count):
        pointer[0] = index + 100
        total += storage[0]
    return total, storage[0]


def decode_pairs(storage):
    if len(storage) % 8:
        raise ValueError('incomplete fixed-layout Pair')
    return [(int.from_bytes(storage[index:index+4], 'little'),
             int.from_bytes(storage[index+4:index+8], 'little'))
            for index in range(0, len(storage), 8)]


def observations():
    decoded=decode_schema_fields(json.loads('{"x":1,"y":2,"z":3}'),
        {'x':(int,False),'y':(int,False)},ignore_unknown=True)
    point=Point(**decoded)
    full,partial=Opt('Ann',30),Opt('Bo')
    x,y=field_copy(Point(3,4),'x','y')
    user=User('Ada',37,['a','b']);who,age,tags=field_copy(user,'name','age','tags')
    px,py=field_copy(Point(10,20),'x','y')
    identity,inner=field_copy(OuterValue(1,InnerValue(99)),'id','inner')
    total,last=clobbered_array_loads(4)
    def accepts_options(_options):return True
    pairs=decode_pairs(bytes([1,0,0,0,2,0,0,0]))
    return {
        'object_width_json_as_extra_key':[point.x,point.y],
        'object_width_nullable_field_omission':[full.name,full.age,partial.name,partial.age is None],
        'object_destructure':[x,y,who,age,*tags,user.name,user.age,px,py,identity,inner.k],
        'tbaa_raw_pointer_store_kills_array_load':[total,last],
        'ws_connect_options_type_surface':[accepts_options(None)],
        'string_from_utf8_slice':[bytes([98,105,110]).decode('utf-8',errors='strict')],
        'slice_reinterpret_value_struct':[len(pairs),*pairs[0]],
    }


def expected_rows():
    binding = json.loads((Path(__file__).with_name('structural_inputs_bindings.json')).read_text(encoding='utf-8'))
    if (binding['sources'] != PATHS or tuple(binding['authorities']) != AUTHORITIES or
            set(binding['inputs']) != set(PATHS.values()) | set(AUTHORITIES)):
        raise ValueError('complete structural model lost an original or authority')
    for name,digest in binding['inputs'].items():verify_input(ROOT,name,digest)
    rows = []
    values = observations()
    if set(values) != set(PATHS):
        raise ValueError('complete structural model lost an observation program')
    for name, output in values.items():
        def render(value):
            if value is None:
                return 'null'
            return str(value).lower() if type(value) is bool else str(value)
        rows.append({'id': 'complete_structural_inputs_' + name,
                     'source': PATHS[name], 'original_source': PATHS[name],
                     'original_run_corpus': True,
                     'required_target': 'windows-x86_64' if name == 'slice_reinterpret_value_struct' else None,
                     'model': 'Retained independent nominal field-copy,nullable omission,explicit JSON schema,alias and typed byte-view observations. Historical object/destructure names grant no current structural language or product qualification.',
                     'observation_count': len(output),
                     'expected': {'exit_code': 0, 'stdout_utf8': ''.join(render(x)+'\n' for x in output),
                                  'stderr_utf8': ''}})
    return rows
