"""Independent complete traces for structural values and typed input views."""
from frozen_derivation_inputs import resolve as resolve_derivation
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


def record(fields, schema, *, ignore_unknown=False):
    """Keep nullable omission distinct from a missing required field."""
    if not ignore_unknown and set(fields) - set(schema):
        raise ValueError('unknown structural field')
    result = {}
    for name, (kind, nullable) in schema.items():
        if name not in fields:
            if not nullable:
                raise ValueError('missing required structural field')
            result[name] = None
            continue
        value = fields[name]
        if value is None and nullable:
            result[name] = None
        elif type(value) is not kind:
            raise ValueError('wrong structural field type')
        else:
            result[name] = copy.deepcopy(value)
    return result


def destructure(value, names):
    return {destination: copy.deepcopy(value[source]) for source, destination in names.items()}


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
    point = record(json.loads('{"x":1,"y":2,"z":3}'),
                   {'x': (int, False), 'y': (int, False)}, ignore_unknown=True)
    optional = {'name': (str, False), 'age': (int, True)}
    full = record({'name': 'Ann', 'age': 30}, optional)
    partial = record({'name': 'Bo'}, optional)
    point_fields = destructure({'x': 3, 'y': 4}, {'x': 'x', 'y': 'y'})
    user = {'name': 'Ada', 'age': 37, 'tags': ['a', 'b']}
    bound = destructure(user, {'name': 'who', 'age': 'age', 'tags': 'tags'})
    assigned = destructure({'x': 10, 'y': 20}, {'x': 'px', 'y': 'py'})
    nested = destructure({'id': 1, 'inner': {'k': 99}}, {'id': 'id', 'inner': 'inner'})
    total, last = clobbered_array_loads(4)
    def accepts_options(_options):
        return True
    pairs = decode_pairs(bytes([1, 0, 0, 0, 2, 0, 0, 0]))
    return {
        'object_width_json_as_extra_key': [point['x'], point['y']],
        'object_width_nullable_field_omission': [full['name'], full['age'], partial['name'], partial['age'] is None],
        'object_destructure': [point_fields['x'], point_fields['y'], bound['who'], bound['age'],
                               *bound['tags'], user['name'], user['age'], assigned['px'], assigned['py'],
                               nested['id'], nested['inner']['k']],
        'tbaa_raw_pointer_store_kills_array_load': [total, last],
        'ws_connect_options_type_surface': [accepts_options(None)],
        'string_from_utf8_slice': [bytes([98, 105, 110]).decode('utf-8', errors='strict')],
        'slice_reinterpret_value_struct': [len(pairs), *pairs[0]],
    }


def expected_rows():
    binding = json.loads((Path(__file__).with_name('structural_inputs_bindings.json')).read_text(encoding='utf-8'))
    if (binding['sources'] != PATHS or tuple(binding['authorities']) != AUTHORITIES or
            set(binding['inputs']) != set(PATHS.values()) | set(AUTHORITIES)):
        raise ValueError('complete structural model lost an original or authority')
    for name, digest in binding['inputs'].items():
        data = resolve_derivation(ROOT, name, digest).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('complete structural input changed: ' + name)
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
                     'model': 'Complete independent structural field, retained value, alias and typed byte-view observations.',
                     'observation_count': len(output),
                     'expected': {'exit_code': 0, 'stdout_utf8': ''.join(render(x)+'\n' for x in output),
                                  'stderr_utf8': ''}})
    return rows
