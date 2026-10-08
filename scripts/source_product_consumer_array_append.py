#!/usr/bin/env python3
"""Encode nine scalar Array appends independently of the production codec."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'
CASES = [('nine_appends', 0, 0), ('boolean_element', 1, 3), ('value_receiver', 2, 4),
         ('scalar_push_result', 3, 3), ('boolean_argument', 4, 3), ('missing_value', 5, 4),
         ('inactive_immediate', 6, 1), ('unused_target', 7, 1), ('unused_type_argument', 8, 1),
         ('wrong_length_result', 9, 3)]


def encode(mutation):
    def w(*values): return struct.pack('<' + 'I' * len(values), *values)
    def op(code, kind=0, a=0, b=0, target=0, immediate=0, typearg=0):
        return w(code, kind, a, b, target, 0) + struct.pack('<q', immediate) + w(typearg, 0)
    name = b'append_nine'
    body = w(0, 1, 0) + w(len(name)) + name + w(1, 1 if mutation == 4 else 2, 2, 1, 0, 13, 0, 0, 13)
    body += op(73, 256) + op(34, 256, 1)
    for index in range(9):
        changed = mutation if index == 0 else 0
        body += op(76, 2 if changed == 3 else 0, 1 if changed == 2 else 2,
                   4294967295 if changed == 5 else 0, int(changed == 7), int(changed == 6), int(changed == 8))
    body += op(77, 1 if mutation == 9 else 2, 1) + op(33, a=12) + w(0)
    body += w(0, 1, 0, 0, 2, 0, 1 if mutation == 1 else 2, 0, 0)
    header = b'XRCHK\0\0\0' + w(25, 67, 2, 0) + struct.pack('<Q', len(body))
    return header + hashlib.sha256(header + body).digest() + body


def artifacts():
    lines = ['/* Independent Checked packets preserve nine scalar Array append operations. */']
    manifest = dict(schema=1, identity='Checked25/semantic67', functions=1, appends_per_function=9, cases=[])
    for index, (name, mutation, expected) in enumerate(CASES):
        packet = encode(mutation)
        lines.append(f'static const uint8_t array_append_packet_{index}[] = {{')
        for offset in range(0, len(packet), 32):
            lines.append('    ' + ','.join(f'0x{x:02x}' for x in packet[offset:offset+32]) + ',')
        lines.append('};')
        manifest['cases'].append(dict(name=name, mutation=mutation, expected=expected,
                                     bytes=len(packet), sha256=hashlib.sha256(packet).hexdigest()))
    lines += ['typedef struct ArrayAppendCase {',
              '    const char *name; const uint8_t *bytes; size_t length; unsigned mutation; XrXirStatus expected;',
              '} ArrayAppendCase;', 'static const ArrayAppendCase array_append_cases[] = {']
    for index, row in enumerate(manifest['cases']):
        lines.append(f'    {{"{row["name"]}",array_append_packet_{index},sizeof(array_append_packet_{index}),'
                     f'{row["mutation"]},{row["expected"]}}},')
    lines += ['};', '']
    manifest['boundary'] = ('Admission only. Original explicit owner bits and read/move parameter modes have no '
                            'identical current encoding. Managed String fixture and runtime length nine stay OPEN.')
    assert len(lines) <= 3000
    return '\n'.join(lines), manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    code, manifest = artifacts()
    for path, value in {DIR/'array_append_cases.inc.c': code,
                        DIR/'array_append_cases.json': json.dumps(manifest, indent=2)+'\n'}.items():
        if args.mode == 'write': path.write_text(value, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != value: raise ValueError(f'independent Array append input differs: {path}')
    print('Array append: nine operations;10 Built/10 wire inputs;40 transitions;whole legacy responsibility OPEN')
