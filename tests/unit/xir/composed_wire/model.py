"""Complete independent field grammars for six fixed inputs and their composition.

The ordinal lists are immutable named facts from the fixed inputs. This module
does not import production code, a product writer, or a native measurement.
Historical decoding is test-only; it adds no compatibility path to the product.
"""
from dataclasses import dataclass
from pathlib import Path
import hashlib
import json
import struct
import sys

sys.dont_write_bytecode = True
TABLES = json.loads(Path(__file__).with_name('ordinals.json').read_text('utf-8'))
COMMON = TABLES['common']


@dataclass(frozen=True)
class Profile:
    schema: int
    semantic: int
    operations: tuple
    lexical: bool = False
    variadic: bool = False
    exported_slots: bool = False
    structural: bool = False
    derivation: bool = False
    materialization: bool = False
    comptime: bool = False
    record: int = 0
    iterator: int = 0
    views: bool = False
    hashes: bool = False
    native_remap: tuple = ()


def operations(*groups):
    return tuple(COMMON + sum((TABLES[group] for group in groups), []))


PROFILES = {
    'P10': Profile(33, 81, operations('cell_append', 'record'), lexical=True,
                   variadic=True, exported_slots=True, structural=True, record=11,
                   hashes=True),
    'P09': Profile(28, 84, operations('cell_append', 'hash'), views=True,
                   hashes=True, native_remap=((6, 10), (7, 11))),
    'P08': Profile(28, 89, operations('cell_append', 'hash', 'generator'), iterator=11,
                   native_remap=((8, 12),)),
    'P11': Profile(30, 75, operations(), materialization=True, comptime=True),
    'P12': Profile(29, 74, operations('parse'), derivation=True,
                   native_remap=((6, 7),)),
    'P13': Profile(32, 84, operations('cell_append', 'math', 'hash'), lexical=True,
                   variadic=True, exported_slots=True, hashes=True),
    'current': Profile(34, 90, operations('cell_append', 'math', 'hash', 'parse',
                                        'record', 'generator'), lexical=True,
                       variadic=True, exported_slots=True, structural=True,
                       derivation=True, materialization=True, comptime=True,
                       record=13, iterator=14, views=True, hashes=True),
}
assert len(PROFILES['current'].operations) == 192
assert len(set(PROFILES['current'].operations)) == 192
IDENTITY_FIELDS = ('module', 'exported', 'nominal_owner', 'member_access',
                   'cleanup_owner', 'promises', 'method_kind', 'test_role',
                   'test_timeout_seconds')


class Fields:
    def __init__(self, data=None):
        self.data, self.at, self.out, self.offsets = data, 0, bytearray(), {}

    def raw(self, path, size, value=b''):
        assert 0 <= size <= 1024 * 1024 * 64, (path, size)
        if self.data is not None:
            assert size <= len(self.data) - self.at, (path, self.at, size, len(self.data))
            value = self.data[self.at:self.at + size]
        else:
            assert len(value) == size, (path, size, len(value))
        self.offsets[path] = (64 + self.at, size)
        self.at += size
        self.out += value
        return value

    def u32(self, path, value=0):
        return struct.unpack('<I', self.raw(path, 4, struct.pack('<I', value)))[0]

    def i64(self, path, value=0):
        return struct.unpack('<q', self.raw(path, 8, struct.pack('<q', value)))[0]

    def blob(self, path, value=b''):
        count = self.u32(path + '.length', len(value))
        return self.raw(path + '.bytes', count, value)

    def rows(self, path, values, record):
        count = self.u32(path + '.count', len(values))
        assert count <= 1048576, (path, count)
        return [record(path + '[' + str(i) + ']', values[i] if self.data is None else None)
                for i in range(count)]

    def vector(self, path, values=()):
        return self.rows(path, values, lambda key, value: self.u32(key, value or 0))


def application(s, key, row=None):
    row = row or {}
    return dict(declaration=s.u32(key + '.declaration', row.get('declaration', 0)),
                arguments=s.vector(key + '.arguments', row.get('arguments', [])))


def applications(s, key, rows=()):
    return s.rows(key, rows, lambda k, v: application(s, k, v))


def constraints(s, p, key, rows, count):
    result = []
    for i in range(count):
        row, k = (rows[i] if s.data is None else {}), key + '[' + str(i) + ']'
        markers = s.u32(k + '.markers', row.get('markers', 0))
        interfaces = applications(s, k + '.interfaces', row.get('interfaces', []))
        structural = []
        if p.structural:
            def requirement(k, value):
                value = value or {}
                return dict(shape=s.u32(k + '.shape', value.get('shape', 0)),
                            policy=s.u32(k + '.policy', value.get('policy', 0)))
            structural = s.rows(k + '.structural', row.get('structural', []), requirement)
        result.append(dict(markers=markers, interfaces=interfaces, structural=structural))
    return result


def instruction(s, p, key, row=None):
    row = row or {}
    operation = row.get('op', 'CONST_BOOL')
    numeric = p.operations.index(operation) + 1 if isinstance(operation, str) else operation
    numeric = s.u32(key + '.op', numeric)
    operation = p.operations[numeric - 1] if 0 < numeric <= len(p.operations) else numeric
    result = dict(op=operation)
    for name in ('type', 'arg0', 'arg1', 'target0', 'target1'):
        result[name] = s.u32(key + '.' + name, row.get(name, 0))
    result['immediate'] = s.i64(key + '.immediate', row.get('immediate', 0))
    for name in ('type_arg0', 'type_arg1'):
        result[name] = s.u32(key + '.' + name, row.get(name, 0))
    return result


def function(s, p, key, row=None):
    row = row or {}
    result = dict(name=s.blob(key + '.name', row.get('name', b'')),
                  parameters=s.vector(key + '.parameters', row.get('parameters', [])),
                  result=s.u32(key + '.result', row.get('result', 0)))
    def block(k, value):
        value = value or [0] * 4
        return [s.u32(k + '.' + field, value[i])
                for i, field in enumerate(('first', 'count', 'panic', 'frontier'))]
    result['blocks'] = s.rows(key + '.blocks', row.get('blocks', []), block)
    result['instructions'] = s.rows(key + '.instructions', row.get('instructions', []),
                                    lambda k, v: instruction(s, p, k, v))
    result['operands'] = s.vector(key + '.operands', row.get('operands', []))
    return result


def generated_origin(s, key, row=None):
    row = row or {}
    result = {name: s.blob(key + '.' + name, row.get(name, b''))
              for name in ('module', 'file', 'field_path')}
    for name in ('line', 'column'):
        result[name] = s.u32(key + '.' + name, row.get(name, 0))
    return result


def derivation_policy(s, key, row=None):
    row = row or {}
    result = {name: s.u32(key + '.' + name, row.get(name, 0))
              for name in ('grantor_module', 'target')}
    result['protocol'] = application(s, key + '.protocol', row.get('protocol'))
    for name in ('generator', 'constructor'):
        result[name] = s.u32(key + '.' + name, row.get(name, 0))
    def member(k, value):
        value = value or [0, 0]
        return [s.u32(k + '.field', value[0]), s.u32(k + '.operations', value[1])]
    result['members'] = s.rows(key + '.members', row.get('members', []), member)
    result['origin'] = generated_origin(s, key + '.origin', row.get('origin'))
    return result


def derivation_grant(s, key, row=None):
    row = row or {}
    result = {name: s.u32(key + '.' + name, row.get(name, 0))
              for name in ('function', 'policy', 'source_row', 'root', 'target')}
    result['entry'] = generated_origin(s, key + '.entry', row.get('entry'))
    result['instructions'] = s.rows(key + '.instructions', row.get('instructions', []),
                                    lambda k, v: generated_origin(s, k, v))
    return result


def declarations(s, p, key, row, functions):
    row = row or {}
    modules = s.u32(key + '.modules.count', len(row.get('modules', [])))
    slots = s.u32(key + '.slots.count', len(row.get('slots', [])))
    literals = s.u32(key + '.literals.count', len(row.get('literals', [])))
    result = {name: s.u32(key + '.' + name, row.get(name, 0)) for name in ('root', 'entry')}
    result['modules'] = []
    for i in range(modules):
        value = row['modules'][i] if s.data is None else {}
        k = key + '.modules[' + str(i) + ']'
        item = dict(name=s.blob(k + '.name', value.get('name', b'')),
                    dependencies=s.vector(k + '.dependencies', value.get('dependencies', [])),
                    initializer=s.u32(k + '.initializer', value.get('initializer', 0)), lexical_scope=0)
        if p.lexical:
            item['lexical_scope'] = s.u32(k + '.lexical_scope', value.get('lexical_scope', 0))
        result['modules'].append(item)
    result['identities'] = []
    for i in range(functions):
        value = row['identities'][i] if s.data is None else {}
        k = key + '.identities[' + str(i) + ']'
        item = {name: s.u32(k + '.' + name, value.get(name, 0)) for name in IDENTITY_FIELDS}
        item['variadic'] = s.u32(k + '.variadic', value.get('variadic', 0)) if p.variadic else 0
        item['derivation'] = s.u32(k + '.derivation', value.get('derivation', 0)) if p.derivation else 0
        result['identities'].append(item)
    result['slots'] = []
    for i in range(slots):
        value = row['slots'][i] if s.data is None else {}
        k = key + '.slots[' + str(i) + ']'
        fields = ('module', 'type', 'mutable', 'exported', 'name') if p.exported_slots else ('module', 'type', 'mutable')
        item = {name: s.u32(k + '.' + name, value.get(name, 0)) for name in fields}
        item.setdefault('exported', 0)
        item.setdefault('name', 0)
        result['slots'].append(item)
    result['literals'] = [s.blob(key + '.literals[' + str(i) + ']', row['literals'][i] if s.data is None else b'')
                          for i in range(literals)]
    def implementation(k, value):
        value = value or {}
        item = dict(nominal=s.u32(k + '.nominal', value.get('nominal', 0)))
        item['policy'] = s.u32(k + '.policy', value.get('policy', 0)) if p.derivation else 0
        item['interface'] = application(s, k + '.interface', value.get('interface'))
        def binding(k, value):
            value = value or {}
            return dict(requirement=application(s, k + '.requirement', value.get('requirement')),
                        member=s.u32(k + '.member', value.get('member', 0)),
                        function=s.u32(k + '.function', value.get('function', 0)))
        item['bindings'] = s.rows(k + '.bindings', value.get('bindings', []), binding)
        return item
    result['implementations'] = s.rows(key + '.implementations', row.get('implementations', []), implementation)
    result['policies'], result['grants'] = [], []
    if p.derivation:
        result['policies'] = s.rows(key + '.policies', row.get('policies', []), lambda k, v: derivation_policy(s, k, v))
        result['grants'] = s.rows(key + '.grants', row.get('grants', []), lambda k, v: derivation_grant(s, k, v))
    return result


def generic(s, p, key, row=None):
    row = row or {}
    count = s.u32(key + '.parameters', len(row.get('constraints', [])))
    present = s.u32(key + '.kinds_present', int(row.get('kinds') is not None))
    assert present in (0, 1), (key, present)
    kinds = [s.u32(key + '.kinds[' + str(i) + ']', row['kinds'][i] if s.data is None else 0)
             for i in range(count)] if present else None
    return dict(kinds=kinds, constraints=constraints(s, p, key + '.constraints', row.get('constraints', []), count),
                arguments=s.vector(key + '.arguments', row.get('arguments', [])))


def node(s, p, key, row=None):
    row = row or {}
    kind = row.get('kind', 'ARRAY')
    kinds = {1: 'CALLABLE', 2: 'ARRAY', 3: 'CELL', 4: 'NOMINAL', 5: 'NULLABLE',
             6: 'TUPLE', 7: 'ATOMIC', 8: 'TASK'}
    if p.hashes:
        kinds.update({9: 'MAP', 10: 'SET'})
    if p.views:
        kinds.update({11: 'SLICE', 12: 'MUT_SLICE'})
    if p.record:
        kinds[p.record] = 'RECORD'
    if p.iterator:
        kinds[p.iterator] = 'ITERATOR'
    kind_id = next(i for i, name in kinds.items() if name == kind) if isinstance(kind, str) else kind
    kind_id = s.u32(key + '.kind', kind_id)
    assert kind_id in kinds, ('closed type kind', key, kind_id, s.at)
    kind = kinds[kind_id]
    result = dict(kind=kind, span=s.u32(key + '.span', row.get('span', 0)))
    if kind == 'CALLABLE':
        def parameter(k, value):
            value = value or [0, 0]
            return [s.u32(k + '.type', value[0]), s.u32(k + '.mode', value[1])]
        result['parameters'] = s.rows(key + '.parameters', row.get('parameters', []), parameter)
        result['result'] = s.u32(key + '.result', row.get('result', 0))
        result['flags'] = s.u32(key + '.flags', row.get('flags', 0))
    elif kind in ('TUPLE', 'RECORD'):
        result['fields'] = s.vector(key + '.fields', row.get('fields', []))
        if kind == 'RECORD':
            result['names'] = [s.blob(key + '.names[' + str(i) + ']', row['names'][i] if s.data is None else b'')
                               for i in range(len(result['fields']))]
    elif kind == 'NOMINAL':
        result['declaration'] = s.u32(key + '.declaration', row.get('declaration', 0))
        result['arguments'] = s.vector(key + '.arguments', row.get('arguments', []))
        result['fields'] = s.vector(key + '.fields', row.get('fields', []))
    else:
        result['element'] = s.u32(key + '.element', row.get('element', 0))
        if kind == 'MAP':
            result['result'] = s.u32(key + '.result', row.get('result', 0))
    return result


def nominal(s, p, key, row=None):
    row = row or {}
    result = {name: s.blob(key + '.' + name, row.get(name, b'')) for name in ('module', 'name')}
    for name in ('exported', 'kind', 'flags', 'native_id'):
        result[name] = s.u32(key + '.' + name, row.get(name, 0))
    result['fingerprint'] = s.raw(key + '.fingerprint', 32, row.get('fingerprint', bytes(32)))
    count = s.u32(key + '.parameters', len(row.get('constraints', [])))
    result['constraints'] = constraints(s, p, key + '.constraints', row.get('constraints', []), count)
    def field(k, value):
        value = value or {}
        return dict(name=s.blob(k + '.name', value.get('name', b'')),
                    type=s.u32(k + '.type', value.get('type', 0)),
                    flags=s.u32(k + '.flags', value.get('flags', 0)))
    result['fields'] = s.rows(key + '.fields', row.get('fields', []), field)
    def variant(k, value):
        value = value or {}
        return dict(name=s.blob(k + '.name', value.get('name', b'')),
                    begin=s.u32(k + '.begin', value.get('begin', 0)),
                    count=s.u32(k + '.count', value.get('count', 0)))
    result['variants'] = s.rows(key + '.variants', row.get('variants', []), variant)
    return result


def interface(s, p, key, row=None):
    row = row or {}
    result = {name: s.blob(key + '.' + name, row.get(name, b'')) for name in ('module', 'name')}
    result['exported'] = s.u32(key + '.exported', row.get('exported', 0))
    count = s.u32(key + '.parameters', len(row.get('constraints', [])))
    result['constraints'] = constraints(s, p, key + '.constraints', row.get('constraints', []), count)
    result['parents'] = applications(s, key + '.parents', row.get('parents', []))
    def method(k, value):
        value = value or {}
        item = dict(name=s.blob(k + '.name', value.get('name', b'')),
                    signature=s.u32(k + '.signature', value.get('signature', 0)),
                    receiver=s.u32(k + '.receiver', value.get('receiver', 0)))
        count = s.u32(k + '.own_parameters', len(value.get('constraints', [])))
        item['constraints'] = constraints(s, p, k + '.constraints', value.get('constraints', []), count)
        return item
    result['methods'] = s.rows(key + '.methods', row.get('methods', []), method)
    return result


def fixed_row(s, key, row, names):
    row = row or {}
    return {name: s.u32(key + '.' + name, row.get(name, 0)) for name in names}


def provenance(s, p, key, row, depth, functions):
    row = row or {}
    kind = s.u32(key + '.kind', row.get('kind', 0))
    assert kind in (0, 1, 2), (key, kind)
    result = dict(kind=kind)
    if kind == 1:
        def contract(k, value):
            value = value or {}
            item = dict(parameters=s.rows(k + '.parameters', value.get('parameters', []),
                        lambda k, v: fixed_row(s, k, v, ('kind', 'uses'))))
            item['constant_mask'] = s.u32(k + '.constant_mask', value.get('constant_mask', 0))
            for name, fields in (('terms', ('kind', 'index')), ('values', ('instruction', 'mode', 'declared_type')),
                                 ('bindings', ('family', 'instruction', 'parameter', 'value'))):
                item[name] = s.rows(k + '.' + name, value.get(name, []),
                                   lambda k, v, fields=fields: fixed_row(s, k, v, fields))
            return item
        result['contracts'] = s.rows(key + '.contracts', row.get('contracts', []), contract)
        assert len(result['contracts']) == functions, (key, functions)
    elif kind == 2:
        result['source'] = module(s, p, key + '.source', row.get('source'), depth + 1)
        def origin(k, value):
            value = value or {}
            return dict(function=s.u32(k + '.function', value.get('function', 0)),
                        arguments=s.vector(k + '.arguments', value.get('arguments', [])),
                        effect_arguments=s.rows(k + '.effect_arguments', value.get('effect_arguments', []),
                             lambda k, v: fixed_row(s, k, v, ('parameter', 'type'))))
        result['origins'] = s.rows(key + '.origins', row.get('origins', []), origin)
        assert len(result['origins']) == functions, (key, functions)
        result['proofs'] = s.rows(key + '.proofs', row.get('proofs', []),
                             lambda k, v: fixed_row(s, k, v, ('family', 'caller', 'instruction',
                                                           'callee', 'parameter', 'actual_value')))
    return result


def module(s, p, key='module', row=None, depth=0):
    assert depth <= 1, ('recursive evidence depth', depth)
    row = row or {}
    result = dict(linkage=s.u32(key + '.linkage', row.get('linkage', 0)))
    functions = s.u32(key + '.functions.count', len(row.get('functions', [])))
    present = s.u32(key + '.declarations.present', int(row.get('declarations') is not None))
    assert present in (0, 1), (key, present)
    result['functions'] = [function(s, p, key + '.functions[' + str(i) + ']',
                                    row['functions'][i] if s.data is None else None) for i in range(functions)]
    result['declarations'] = declarations(s, p, key + '.declarations', row.get('declarations'), functions) if present else None
    present = s.u32(key + '.generics.present', int(row.get('generics') is not None))
    assert present in (0, 1), (key, present)
    result['generics'] = [generic(s, p, key + '.generics[' + str(i) + ']',
                                  row['generics'][i] if s.data is None else None) for i in range(functions)] if present else None
    nodes = s.u32(key + '.nodes.count', len(row.get('nodes', [])))
    nominals = s.u32(key + '.nominals.count', len(row.get('nominals', [])))
    interfaces = s.u32(key + '.interfaces.count', len(row.get('interfaces', [])))
    for name, count, reader in (('nodes', nodes, node), ('nominals', nominals, nominal), ('interfaces', interfaces, interface)):
        result[name] = [reader(s, p, key + '.' + name + '[' + str(i) + ']',
                               row[name][i] if s.data is None else None) for i in range(count)]
    def constructor(k, value):
        value = value or {}
        return dict(default=s.u32(k + '.default', value.get('default', 0)),
                    fields=s.vector(k + '.fields', value.get('fields', [])))
    result['construction'] = s.rows(key + '.construction', row.get('construction', []), constructor)
    assert len(result['construction']) == nominals, (key, len(result['construction']), nominals)
    for name, enabled, fields in (('materialization', p.materialization, ('caller', 'instruction', 'function', 'nominal')),
                                  ('comptime_sites', p.comptime, ('caller', 'instruction', 'helper'))):
        result[name] = s.rows(key + '.' + name, row.get(name, []),
                             lambda k, v, fields=fields: fixed_row(s, k, v, fields)) if enabled else []
    def comptime_binding(k, value):
        value = value or {}
        return dict(site=s.u32(k + '.site', value.get('site', 0)),
                    arguments=s.vector(k + '.arguments', value.get('arguments', [])),
                    provider=s.u32(k + '.provider', value.get('provider', 0)))
    result['comptime_bindings'] = s.rows(key + '.comptime_bindings', row.get('comptime_bindings', []),
                                        comptime_binding) if p.comptime else []
    result['defaults'] = s.rows(key + '.defaults', row.get('defaults', []),
                               lambda k, v: fixed_row(s, k, v, ('owner_kind', 'owner', 'ordinal', 'function')))
    result['provenance'] = provenance(s, p, key + '.provenance', row.get('provenance'), depth, functions)
    return result


def decode(packet, label):
    p = PROFILES[label]
    assert packet[:8] == b'XRCHK\0\0\0'
    assert struct.unpack_from('<4I', packet, 8) == (p.schema, p.semantic, 2, 0)
    assert struct.unpack_from('<Q', packet, 24)[0] == len(packet) - 64
    assert hashlib.sha256(packet[:32] + packet[64:]).digest() == packet[32:64]
    s = Fields(packet[64:])
    result = module(s, p)
    assert s.at == len(packet) - 64, ('unconsumed fields', label, s.at, len(packet) - 64)
    assert bytes(s.out) == packet[64:]
    return result, s.offsets


def encode(value, label='current'):
    p, s = PROFILES[label], Fields()
    module(s, p, row=value)
    body = bytes(s.out)
    header = b'XRCHK\0\0\0' + struct.pack('<4IQ', p.schema, p.semantic, 2, 0, len(body))
    return header + hashlib.sha256(header + body).digest() + body, s.offsets


def compose(value, label):
    """Only native declaration identities need semantic renaming in the tree."""
    import copy
    result = copy.deepcopy(value)
    mapping = dict(PROFILES[label].native_remap)
    def visit(item):
        for declaration in item['nominals']:
            declaration['native_id'] = mapping.get(declaration['native_id'], declaration['native_id'])
        if item['provenance']['kind'] == 2:
            visit(item['provenance']['source'])
    visit(result)
    return result
