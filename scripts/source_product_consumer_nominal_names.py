#!/usr/bin/env python3
"""Encode original display-name byte obligations in complete nominal Checked packets."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIR = ROOT / 'tests/unit/xir/product_consumers'


def packet(type_name, variant_name):
    def words(*v):
        return struct.pack('<' + 'I' * len(v), *v)
    def blob(value):
        return words(len(value)) + value
    def ins(op, type_id, value=0):
        return words(op, type_id, 0, 0, 0, 0) + struct.pack('<Q', value) + words(0, 0)
    body = words(0, 2, 1)
    body += blob(b'init') + words(0, 0, 1, 0, 1, 0, 0, 1) + ins(33, 0) + words(0)
    body += blob(b'entry') + words(0, 2, 1, 0, 2, 0, 0, 2) + ins(2, 2, 42) + ins(33, 0) + words(0)
    body += words(1, 0, 0, 0, 1) + blob(b'root') + words(0, 0)
    body += words(*([0] * 18)) + words(0)  # two function identities, no implementations
    body += words(0, 0, 1, 0)  # no generics; zero type nodes, one nominal, no interfaces
    body += blob(b'root')
    type_offset = 64 + len(body) + 4
    body += blob(type_name) + words(0, 1, 0, 0) + bytes(32)  # enum, no native identity
    body += words(0, 1) + blob(b'value') + words(2, 0)  # one I64 payload field
    body += words(2)
    variant_offset = 64 + len(body) + 4
    body += blob(variant_name) + words(0, 1) + blob(b'Empty') + words(1, 0)
    body += words(0, 0)  # defaults, provenance
    head = b'XRCHK\0\0\0' + words(25, 67, 2, 0) + struct.pack('<Q', len(body))
    return head + hashlib.sha256(head + body).digest() + body, type_offset, variant_offset


def artifacts():
    rows = [('ascii', b'TopErr', b'Failed', 'XR_XIR_OK'),
            ('unicode', '错误'.encode(), '失败'.encode(), 'XR_XIR_OK'),
            ('type_boundary', b'a' * 4096, b'Failed', 'XR_XIR_OK'),
            ('variant_boundary', b'TopErr', b'a' * 4096, 'XR_XIR_OK')]
    bad = [('empty', b''), ('newline', b'line\nbreak'), ('del', b'\x7f'),
           ('continuation', b'\x80'), ('overlong', b'\xc0\x80'), ('too_long', b'a' * 4097),
           ('nul', b'Top\0Err')]
    for location in ('type', 'variant'):
        for name, value in bad:
            status = 'XR_XIR_BUDGET' if name == 'too_long' else 'XR_XIR_BAD_STRUCTURE'
            rows.append((location + '_' + name, value if location == 'type' else b'TopErr',
                         value if location == 'variant' else b'Failed', status))
    lines = ['/* Complete independent Checked inputs; expectations retain original name defenses. */']
    manifest = dict(schema=1, packets=18, controls=4, rejects=14, cases=[])
    for i, (name, type_name, variant_name, expected) in enumerate(rows):
        data, type_offset, variant_offset = packet(type_name, variant_name)
        lines.append(f'static const uint8_t nominal_name_packet_{i}[] = {{')
        for offset in range(0, len(data), 16):
            lines.append('    ' + ','.join(f'0x{x:02x}' for x in data[offset:offset + 16]) + ',')
        lines.append('};')
        manifest['cases'].append(dict(name=name, expected=expected, bytes=len(data),
            sha256=hashlib.sha256(data).hexdigest(), type_offset=type_offset, type_length=len(type_name),
            variant_offset=variant_offset, variant_length=len(variant_name)))
    lines += ['typedef struct NominalNameCase {', '    const char *name; const uint8_t *bytes; size_t length;',
              '    XrXirStatus expected; size_t type_offset, type_length, variant_offset, variant_length;',
              '} NominalNameCase;', 'static const NominalNameCase nominal_name_cases[] = {']
    for i, row in enumerate(manifest['cases']):
        lines.append(f'    {{"{row["name"]}", nominal_name_packet_{i}, sizeof(nominal_name_packet_{i}), '
                     f'{row["expected"]}, {row["type_offset"]}, {row["type_length"]}, '
                     f'{row["variant_offset"]}, {row["variant_length"]}}},')
    lines += ['};', '']
    return '\n'.join(lines), manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    header, manifest = artifacts()
    for path, text in {DIR/'nominal_name_cases.h': header,
                       DIR/'nominal_name_cases.json': json.dumps(manifest, indent=2)+'\n'}.items():
        if args.mode == 'write':
            path.write_text(text, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != text:
            raise ValueError(f'independent nominal name fixture differs: {path}')
    print('Nominal name obligations: 18 complete packets, 4 controls, 14 original byte/limit rejects')
