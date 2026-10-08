#!/usr/bin/env python3
"""Encode Array descriptor inputs independently of the production Checked codec."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def cases():
    rows = []
    def add(name, nodes=None, expected=3, inactive=0):
        rows.append(dict(name=name, nodes=nodes or [[2, 0, 3], [2, 0, 256], [2, 0, 15]],
                         expected=expected, inactive=inactive, wire=not inactive))
    add('string_nested_and_panic', expected=0)
    add('independent_nodes_reordered', [[2, 0, 15], [2, 0, 3], [2, 0, 257]], 0)
    add('legacy_nested_panic_reject', [[2, 0, 15], [2, 0, 256], [2, 0, 3]])
    for name, element in [('unit_element', 0), ('reserved_element', 4), ('unresolved_element', 65535),
                          ('maximum_element', 4294967295), ('self_edge', 256), ('forward_edge', 258),
                          ('missing_node', 259), ('unbound_span', 65536)]:
        add(name, [[2, 0, element], [2, 0, 256], [2, 0, 15]])
    add('cyclic_edges', [[2, 0, 257], [2, 0, 256], [2, 0, 15]])
    add('duplicate_descriptor', [[2, 0, 3], [2, 0, 3], [2, 0, 15]], 1)
    add('closed_span_mismatch', [[2, 1, 3], [2, 0, 256], [2, 0, 15]])
    add('unknown_kind', [[99, 0, 3], [2, 0, 256], [2, 0, 15]])
    add('cell_element', [[3, 0, 3], [2, 0, 256], [2, 0, 15]])
    for index, field in enumerate(('parameters', 'parameter_count', 'result', 'flags',
                                   'nominal_declaration', 'nominal_arguments', 'nominal_argument_count',
                                   'nominal_fields', 'nominal_field_count'), 1):
        add('inactive_' + field, expected=1, inactive=index)
    assert len(rows) == 25 and sum(r['wire'] for r in rows) == 16
    return rows


def encode(row):
    def w(*v): return struct.pack('<' + 'I' * len(v), *v)
    def op(code, kind=0, value=0): return w(code, kind, 0, 0, 0, 0) + struct.pack('<Q', value) + w(0, 0)
    function = w(4) + b'main' + w(0, 2, 1, 0, 2, 0, 0, 2) + op(2, 2, 42) + op(33) + w(0)
    # An absent declarations table has no implementation-count word.
    body = w(0, 1, 0) + function + w(0, 3, 0, 0)
    body += b''.join(w(*node) for node in row['nodes']) + w(0, 0)
    head = b'XRCHK\0\0\0' + w(25, 67, 2, 0) + struct.pack('<Q', len(body))
    return head + hashlib.sha256(head + body).digest() + body


def artifacts():
    manifest = dict(schema=1, wire_cases=16, built_cases=25, output_modes=2, cases=[])
    lines = ['/* Independently encoded Checked Array descriptors; no legacy ownership flags. */']
    for i, row in enumerate(cases()):
        packet = encode(row)
        lines.append(f'static const uint8_t array_types_packet_{i}[] = {{')
        for off in range(0, len(packet), 32):
            lines.append('    ' + ','.join(f'0x{x:02x}' for x in packet[off:off+32]) + ',')
        lines.append('};')
        manifest['cases'].append(dict(**row, bytes=len(packet), sha256=hashlib.sha256(packet).hexdigest()))
    lines += ['typedef struct ArrayTypesCase {',
              '    const char *name; const uint8_t *bytes; size_t length;',
              '    XrXirStatus expected; unsigned inactive, wire; uint32_t nodes[3][3];',
              '} ArrayTypesCase;', 'static const ArrayTypesCase array_types_cases[] = {']
    for i, row in enumerate(manifest['cases']):
        nodes = ','.join('{' + ','.join(str(x)+'u' for x in node) + '}' for node in row['nodes'])
        lines.append(f'    {{"{row["name"]}",array_types_packet_{i},sizeof(array_types_packet_{i}),'
                     f'{row["expected"]},{row["inactive"]},{int(row["wire"])},{{{nodes}}}}},')
    lines += ['};', '']
    manifest['boundary'] = 'Built admission and Checked wire only. Legacy nested PanicInfo rejection stays strict; old key-order canonicalization and copy flags have no direct representation. No whole-function retirement.'
    return '\n'.join(lines), manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    header, manifest = artifacts()
    for path, text in {DIR/'array_types_cases.h': header, DIR/'array_types_cases.json': json.dumps(manifest, indent=2)+'\n'}.items():
        if args.mode == 'write': path.write_text(text, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != text: raise ValueError(f'independent Array input differs: {path}')
    print('Array types:16 wire/25 Built cases;82 transitions;legacy copy and canonicalization duties OPEN')
