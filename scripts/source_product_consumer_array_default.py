#!/usr/bin/env python3
"""Encode typed Array default expansion independently of the production codec."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'
CASES = [('default_u8', 0, 0), ('boolean_count', 1, 3), ('string_element_u8_fill', 2, 3),
         ('scalar_array_result', 3, 3), ('missing_count', 4, 4), ('missing_fill', 5, 4),
         ('inactive_immediate', 6, 1), ('unused_target', 7, 1), ('unused_type_argument', 8, 1),
         ('wrong_length_result', 9, 3)]


def encode(mutation):
    def w(*values): return struct.pack('<' + 'I' * len(values), *values)
    def op(code, kind=0, a=0, b=0, target=0, immediate=0, typearg=0):
        return w(code, kind, a, b, target, 0) + struct.pack('<q', immediate) + w(typearg, 0)
    name = b'default_count'
    body = w(0, 1, 0) + w(len(name)) + name + w(1, 1 if mutation == 1 else 2, 2, 1, 0, 4, 0, 0, 4)
    body += op(2, 8)
    body += op(133, 2 if mutation == 3 else 256, 4294967295 if mutation == 4 else 0,
               4294967295 if mutation == 5 else 1, int(mutation == 7), int(mutation == 6), int(mutation == 8))
    body += op(77, 1 if mutation == 9 else 2, 2) + op(33, a=3) + w(0)
    body += w(0, 1, 0, 0, 2, 0, 3 if mutation == 2 else 8, 0, 0)
    header = b'XRCHK\0\0\0' + w(25, 67, 2, 0) + struct.pack('<Q', len(body))
    return header + hashlib.sha256(header + body).digest() + body


def artifacts():
    lines = ['/* Independent Checked packets preserve count and explicit zero-fill operands. */']
    manifest = dict(schema=1, identity='Checked25/semantic67', functions=1, instructions=4, cases=[])
    for index, (name, mutation, expected) in enumerate(CASES):
        packet = encode(mutation)
        lines.append(f'static const uint8_t array_default_packet_{index}[] = {{')
        for offset in range(0, len(packet), 32):
            lines.append('    ' + ','.join(f'0x{x:02x}' for x in packet[offset:offset+32]) + ',')
        lines.append('};')
        manifest['cases'].append(dict(name=name, mutation=mutation, expected=expected,
                                     bytes=len(packet), sha256=hashlib.sha256(packet).hexdigest()))
    lines += ['typedef struct ArrayDefaultCase {',
              '    const char *name; const uint8_t *bytes; size_t length; unsigned mutation; XrXirStatus expected;',
              '} ArrayDefaultCase;', 'static const ArrayDefaultCase array_default_cases[] = {']
    for index, row in enumerate(manifest['cases']):
        lines.append(f'    {{"{row["name"]}",array_default_packet_{index},sizeof(array_default_packet_{index}),'
                     f'{row["mutation"]},{row["expected"]}}},')
    lines += ['};', '']
    manifest['boundary'] = ('Admission of explicit typed zero plus ARRAY_REPEAT only. This does not prove Source '
                            'default-initializer rejection for String, legacy owner bits or runtime outcomes.')
    assert len(lines) <= 3000
    return '\n'.join(lines), manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    code, manifest = artifacts()
    for path, value in {DIR/'array_default_cases.inc.c': code,
                        DIR/'array_default_cases.json': json.dumps(manifest, indent=2)+'\n'}.items():
        if args.mode == 'write': path.write_text(value, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != value: raise ValueError(f'independent Array default input differs: {path}')
    print('Array default: typed zero;10 Built/10 wire inputs;40 transitions;whole legacy responsibility OPEN')
