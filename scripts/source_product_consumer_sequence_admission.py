#!/usr/bin/env python3
"""Independently encode String length admission and exact semantic refusals."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

DEST = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'
CASES = [('string_length', 0), ('scalar_receiver', 3), ('boolean_result', 3),
         ('unused_immediate', 1), ('unknown_receiver', 4), ('wrong_return_type', 3),
         ('unused_second_argument', 1), ('unused_target', 1),
         ('unused_type_argument', 1), ('invalid_panic_target', 1)]


def encode(mutation):
    def w(*values): return struct.pack('<' + 'I' * len(values), *values)
    def op(code, kind=0, a=0, b=0, target=0, immediate=0, typearg=0):
        return w(code, kind, a, b, target, 0) + struct.pack('<q', immediate) + w(typearg, 0)
    name = b'string_length'
    parameter = 2 if mutation == 1 else 3
    result = 1 if mutation == 2 else 3 if mutation == 5 else 2
    body = w(0, 1, 0) + w(len(name)) + name + w(1, parameter, result, 1, 0, 2, int(mutation == 9), 0, 2)
    body += op(86, 1 if mutation == 2 else 2, 0xffffffff if mutation == 4 else 0,
               int(mutation == 6), int(mutation == 7), int(mutation == 3), int(mutation == 8))
    body += op(33, a=1) + w(0) + w(0, 0, 0, 0, 0, 0)
    header = b'XRCHK\0\0\0' + w(25, 67, 2, 0) + struct.pack('<Q', len(body))
    return header + hashlib.sha256(header + body).digest() + body


def artifacts():
    code = ['/* Independent authenticated String length packets exercise semantic admission. */']
    manifest = dict(schema=1, identity='Checked25/semantic67', case_count=len(CASES),
                    transitions_per_configuration=40, positive_transitions=4, rejection_transitions=36, cases=[])
    for index, (name, expected) in enumerate(CASES):
        packet = encode(index)
        code.append(f'static const uint8_t sequence_admission_packet_{index}[] = {{')
        for offset in range(0, len(packet), 24):
            code.append('    ' + ','.join(f'0x{x:02x}' for x in packet[offset:offset+24]) + ',')
        code.append('};')
        manifest['cases'].append(dict(ordinal=index, name=name, expected_status=expected,
                                      bytes=len(packet), sha256=hashlib.sha256(packet).hexdigest()))
    code += ['typedef struct SequenceAdmissionCase {',
             '    const char *name; XrXirStatus expected; const uint8_t *bytes; size_t length;',
             '} SequenceAdmissionCase;', 'static const SequenceAdmissionCase sequence_admission_cases[] = {']
    code += [f'    {{"{name}",(XrXirStatus){expected},sequence_admission_packet_{i},sizeof(sequence_admission_packet_{i})}},'
             for i, (name, expected) in enumerate(CASES)]
    code += ['};', '']
    manifest['boundary'] = ('String receiver, I64 result and inactive fields only; old owner flags, '
                            'consumed SSA after OWNER_DROP, BackendIR mutation, FI and whole qualification remain OPEN.')
    return '\n'.join(code), json.dumps(manifest, indent=2) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('write', 'check'))
    args = parser.parse_args()
    for path, content in zip((DEST/'sequence_admission_cases.inc.c', DEST/'sequence_admission_cases.json'), artifacts()):
        if args.action == 'write': path.write_text(content, encoding='utf-8', newline='\n')
        elif path.read_text(encoding='utf-8') != content: raise ValueError(f'stale sequence admission fixture: {path}')
    print('Sequence admission: 10 authentic packets; 40 transitions; 4 positive and 36 refusal expectations per configuration')
