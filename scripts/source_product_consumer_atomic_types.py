#!/usr/bin/env python3
"""Encode Atomic descriptor and shared type-reference inputs independently."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def cases():
    rows = []
    def add(name, nodes=None, expected=3, inactive=0):
        rows.append(dict(name=name, nodes=nodes or [[7, 0, 2], [7, 0, 1], [7, 0, 13]],
                         expected=expected, inactive=inactive, wire=not inactive))
    add('i64_bool_f64', expected=0)
    add('independent_nodes_reordered', [[7, 0, 13], [7, 0, 1], [7, 0, 2]], 0)
    add('deduplicated_i64_bool_i64_references', [[7, 0, 2], [7, 0, 1]], 0)
    add('duplicate_descriptor', [[7, 0, 2], [7, 0, 1], [7, 0, 2]], 1)
    for name, element in [('unit_element', 0), ('string_element', 3), ('panic_element', 15),
                          ('rune_element', 16), ('i8_element', 5), ('f32_element', 12),
                          ('reserved_element', 4), ('unresolved_element', 65535),
                          ('maximum_element', 4294967295), ('self_edge', 256),
                          ('forward_edge', 258), ('missing_node', 259), ('unbound_span', 65536)]:
        add(name, [[7, 0, element], [7, 0, 1], [7, 0, 13]])
    add('nested_atomic_element', [[7, 0, 2], [7, 0, 256], [7, 0, 13]])
    add('array_element', [[2, 0, 2], [7, 0, 256], [7, 0, 13]])
    add('closed_span_mismatch', [[7, 1, 2], [7, 0, 1], [7, 0, 13]])
    add('unknown_kind', [[99, 0, 2], [7, 0, 1], [7, 0, 13]])
    for index, field in enumerate(('parameters', 'parameter_count', 'result', 'flags',
                                   'nominal_declaration', 'nominal_arguments', 'nominal_argument_count',
                                   'nominal_fields', 'nominal_field_count'), 1):
        add('inactive_' + field, expected=1, inactive=index)
    assert len(rows) == 30 and sum(r['wire'] for r in rows) == 21
    return rows


def encode(row):
    def w(*v): return struct.pack('<' + 'I' * len(v), *v)
    def op(code, kind=0, value=0): return w(code, kind, 0, 0, 0, 0) + struct.pack('<Q', value) + w(0, 0)
    def function(name, parameters, result, instructions):
        return (w(len(name)) + name + w(len(parameters), *parameters, result, 1, 0,
                len(instructions), 0, 0, len(instructions)) + b''.join(instructions) + w(0))
    body = w(0, 2, 0) + function(b'main', [], 2, [op(2, 2, 42), op(33)])
    body += function(b'handles', [256, 257, 256], 0, [op(33)])
    # Absent declarations have no implementation-count word.
    body += w(0, len(row['nodes']), 0, 0)
    body += b''.join(w(*node) for node in row['nodes']) + w(0, 0)
    head = b'XRCHK\0\0\0' + w(25, 67, 2, 0) + struct.pack('<Q', len(body))
    return head + hashlib.sha256(head + body).digest() + body


def artifacts():
    manifest = dict(schema=1, wire_cases=21, built_cases=30, output_modes=2, cases=[])
    lines = ['/* Independent Checked Atomic descriptors and repeated ordinary parameter types. */']
    for i, row in enumerate(cases()):
        packet = encode(row)
        lines.append(f'static const uint8_t atomic_types_packet_{i}[] = {{')
        for off in range(0, len(packet), 32):
            lines.append('    ' + ','.join(f'0x{x:02x}' for x in packet[off:off+32]) + ',')
        lines.append('};')
        manifest['cases'].append(dict(**row, bytes=len(packet), sha256=hashlib.sha256(packet).hexdigest()))
    assert len({r['sha256'] for r in manifest['cases'] if r['wire']}) == 21
    lines += ['typedef struct AtomicTypesCase {',
              '    const char *name; const uint8_t *bytes; size_t length;',
              '    XrXirStatus expected; unsigned inactive, wire, node_count; uint32_t nodes[3][3];',
              '} AtomicTypesCase;', 'static const AtomicTypesCase atomic_types_cases[] = {']
    for i, row in enumerate(manifest['cases']):
        nodes = ','.join('{' + ','.join(str(x)+'u' for x in node) + '}' for node in row['nodes'])
        lines.append(f'    {{"{row["name"]}",atomic_types_packet_{i},sizeof(atomic_types_packet_{i}),'
                     f'{row["expected"]},{row["inactive"]},{int(row["wire"])},{len(row["nodes"])},{{{nodes}}}}},')
    lines += ['};', '']
    manifest['boundary'] = 'Current Built/Checked admission only. Two interned Atomic nodes support i64/bool/i64 references;duplicate node records reject. Old opaque-key canonicalization/copy flags and full FI remain OPEN;no runtime equivalence or retirement.'
    return '\n'.join(lines), manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    header, manifest = artifacts()
    for path, text in {DIR/'atomic_types_cases.h': header, DIR/'atomic_types_cases.json': json.dumps(manifest, indent=2)+'\n'}.items():
        if args.mode == 'write': path.write_text(text, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != text: raise ValueError(f'independent Atomic input differs: {path}')
    print('Atomic types:21 wire/30 Built cases;102 transitions;legacy copy and canonicalization duties OPEN')
