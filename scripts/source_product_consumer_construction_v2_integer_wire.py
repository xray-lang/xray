#!/usr/bin/env python3
"""Encode complete integer wire fixtures without calling the product codec."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

TYPES = [('i8', 5), ('u8', 8), ('i16', 6), ('u16', 9),
         ('i32', 7), ('u32', 10), ('i64', 2), ('u64', 11)]
CASES = [('matrix', 0, 0), ('boolean_operand', 1, 3), ('boolean_result', 2, 3),
         ('nonzero_immediate', 3, 1), ('unused_second_argument', 4, 1),
         ('unused_target', 5, 1), ('inactive_type_argument', 6, 1),
         ('wrong_return_type', 7, 3)]


def encode(mutation):
    def words(*values):
        return struct.pack('<' + 'I' * len(values), *values)

    def instruction(op, kind=0, arg0=0, arg1=0, target0=0, immediate=0, typearg0=0):
        return (words(op, kind, arg0, arg1, target0, 0) +
                struct.pack('<q', immediate) + words(typearg0, 0))

    # Each complete function has one parameter, one block and two instructions.
    # Value0 is the argument; value1 is its conversion, which the return consumes.
    body = words(0, 64, 0)
    for index, ((source, source_type), (target, target_type)) in enumerate(
            (a, b) for a in TYPES for b in TYPES):
        name = ('conv_' + source + '_' + target).encode('ascii')
        changed = mutation if index == 0 else 0
        parameter = 1 if changed == 1 else source_type
        result = 1 if changed in (2, 7) else target_type
        conversion_type = 1 if changed == 2 else target_type
        body += words(len(name)) + name + words(1, parameter, result, 1, 0, 2, 0, 0, 2)
        body += instruction(62, conversion_type, arg1=int(changed == 4),
                            target0=int(changed == 5), immediate=int(changed == 3),
                            typearg0=int(changed == 6))
        body += instruction(33, arg0=1) + words(0)
    # Dense construction count is a real field even with no nominal types.
    # Generics, type nodes, nominals, interfaces, construction, defaults, evidence.
    body += words(0, 0, 0, 0, 0, 0, 0)
    header = b'XRCHK\0\0\0' + words(27, 72, 2, 0) + struct.pack('<Q', len(body))
    return header + hashlib.sha256(header + body).digest() + body


def artifacts():
    lines = ['/* Independently encoded Checked integer graphs with dense construction facts. */']
    manifest = {'schema': 1, 'identity': 'Checked27/semantic72',
                'product_codec_called': False, 'product_execution': 'NOT_RUN',
                'pairs': [{'name': 'conv_' + a + '_' + b, 'source': at, 'target': bt}
                          for a, at in TYPES for b, bt in TYPES], 'cases': []}
    for index, (name, mutation, expected) in enumerate(CASES):
        packet = encode(mutation)
        assert packet[:8] == b'XRCHK\0\0\0'
        assert struct.unpack_from('<IIIIQ', packet, 8) == (27, 72, 2, 0, len(packet) - 64)
        assert packet[32:64] == hashlib.sha256(packet[:32] + packet[64:]).digest()
        assert packet[-28:] == bytes(28)
        lines.append(f'static const uint8_t construction_v2_integer_packet_{index}[] = {{')
        for offset in range(0, len(packet), 32):
            lines.append('    ' + ','.join(f'0x{value:02x}' for value in packet[offset:offset + 32]) + ',')
        lines.append('};')
        manifest['cases'].append({'name': name, 'mutation': mutation, 'expected': expected,
                                  'bytes': len(packet), 'sha256': hashlib.sha256(packet).hexdigest(),
                                  'construction_count_offset': len(packet) - 12})
    lines += ['typedef struct ConstructionV2IntegerWireCase {',
              '    const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected;',
              '} ConstructionV2IntegerWireCase;',
              'static const ConstructionV2IntegerWireCase construction_v2_integer_wire_cases[] = {']
    for index, row in enumerate(manifest['cases']):
        lines.append(f'    {{"{row["name"]}", construction_v2_integer_packet_{index}, '
                     f'sizeof(construction_v2_integer_packet_{index}), {row["expected"]}}},')
    lines += ['};', '']
    manifest['boundary'] = ('Independent test encoder only: complete Built-in integer graphs with zero-row construction. '
                            'No product parser, validator or authority substitute; no nominal, generic, '
                            'effect-template or specialised-instance wire coverage claimed.')
    return '\n'.join(lines), json.dumps(manifest, indent=2) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    header, manifest = artifacts()
    for name, text in [('construction_v2_integer_wire_cases.inc.c', header),
                       ('construction_v2_integer_wire_cases.json', manifest)]:
        path = args.output_dir / name
        if args.mode == 'write':
            path.write_text(text, encoding='utf-8', newline='\n')
        elif path.read_text(encoding='utf-8') != text:
            raise ValueError(f'independent current integer fixture differs: {path}')
    print('Independent integer wire fixtures: 64pairs; 8exact outcomes; real dense construction count; product NOT_RUN')
