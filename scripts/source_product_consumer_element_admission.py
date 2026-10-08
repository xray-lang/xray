#!/usr/bin/env python3
"""Independently encode exact scalar Array get/set admission cases."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'tests/unit/xir/product_consumers'
SPEC = importlib.util.spec_from_file_location('element_frames', ROOT / 'scripts/source_product_consumer_element_place_frames.py')
FRAMES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FRAMES)
CASES = [
    ('cell_place', 0, None), ('value_read', 0, (6, 8, 1)),
    ('get_result_u64', 3, (6, 4, 11)), ('get_scalar_receiver', 3, (6, 8, 0)),
    ('get_array_index', 3, (6, 12, 1)), ('set_value_receiver', 4, ('operand', 1, 1)),
    ('set_scalar_receiver', 3, ('operand', 1, 0)), ('set_array_index', 3, ('operand', 2, 1)),
    ('set_array_element', 3, ('operand', 3, 1)), ('set_non_unit_result', 3, (5, 4, 2)),
    ('cell_place_wrong_result', 3, (3, 4, 2)), ('set_short_group', 1, (5, 12, 2)),
    ('set_group_out_of_range', 1, (5, 8, 4)), ('get_unknown_index', 4, (6, 12, 0xffffffff))]


def packets():
    base = FRAMES.encode()
    first_op = 64 + 12 + 4 + len(b'element_place') + 32
    first_operand = first_op + 8 * 40 + 4
    result = []
    for name, expected, change in CASES:
        wire = bytearray(base)
        if change:
            index, field, value = change
            offset = first_operand + field * 4 if index == 'operand' else first_op + index * 40 + field
            struct.pack_into('<I', wire, offset, value)
            wire[32:64] = hashlib.sha256(wire[:32] + wire[64:]).digest()
        result.append((name, expected, bytes(wire)))
    return result


def artifacts():
    rows = packets()
    code = ['/* Independent Checked packets retain authentic hashes for semantic rejection. */']
    for i, (_, _, wire) in enumerate(rows):
        code.append(f'static const uint8_t element_admission_packet_{i}[] = {{')
        for offset in range(0, len(wire), 24):
            code.append('    ' + ','.join(f'0x{x:02x}' for x in wire[offset:offset+24]) + ',')
        code.append('};')
    code += ['typedef struct ElementAdmissionCase {',
             '    const char *name; XrXirStatus expected; const uint8_t *bytes; size_t length;',
             '} ElementAdmissionCase;', 'static const ElementAdmissionCase element_admission_cases[] = {']
    code += [f'    {{"{name}",(XrXirStatus){expected},element_admission_packet_{i},sizeof(element_admission_packet_{i})}},'
             for i, (name, expected, _) in enumerate(rows)]
    code += ['};', '']
    manifest = dict(schema=1, identity='Checked25/semantic67', case_count=14,
        transitions_per_configuration=56, positive_transitions=8, rejection_transitions=48,
        cases=[dict(ordinal=i, name=name, expected_status=expected, bytes=len(wire),
                    sha256=hashlib.sha256(wire).hexdigest(), change=CASES[i][2])
               for i, (name, expected, wire) in enumerate(rows)],
        boundary='Scalar predicate components only; old element place, read-parameter representation, '
                 'loan, owner exchange, cleanup CFG and full qualification remain OPEN.')
    return '\n'.join(code), json.dumps(manifest, indent=2) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('write', 'check'))
    args = parser.parse_args()
    for path, content in zip((DEST/'element_admission_cases.inc.c', DEST/'element_admission_cases.json'), artifacts()):
        if args.action == 'write': path.write_text(content, encoding='utf-8', newline='\n')
        elif path.read_text(encoding='utf-8') != content: raise ValueError(f'stale element admission fixture: {path}')
    print('Element admission: 14 authentic packets; 56 transitions; 8 positive and 48 refusal expectations per configuration')
