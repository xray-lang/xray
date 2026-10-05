"""Construct complete packets from fixed semantic roles; never read writer bytes."""
from pathlib import Path
import hashlib
import json
import argparse
import sys
import struct

# Frozen append order: original last Tuple FIELD=144, new group=145.
CONST_INT, RETURN, TUPLE_NEW, SLOT_GROUP_INIT = 2, 33, 143, 145
UNIT, I64, TUPLE = 0, 2, 256
def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)
def instruction(op, value_type=UNIT, left=0, right=0, immediate=0):
    return words(op, value_type, left, right, 0, 0) + struct.pack('<Q', immediate) + words(0, 0)
def function(name, result, ops, operands=()):
    encoded = name.encode('ascii')
    return words(len(encoded)) + encoded + words(0, result, 1, 0, len(ops), 0, 0, len(ops)) + \
        b''.join(ops) + words(len(operands), *operands)
def packet(unit):
    init = [instruction(SLOT_GROUP_INIT, immediate=3), instruction(RETURN)] if unit else [
        instruction(CONST_INT, I64, immediate=1), instruction(CONST_INT, I64, immediate=2),
        instruction(TUPLE_NEW, TUPLE, 0, 2), instruction(CONST_INT, I64, immediate=3),
        instruction(CONST_INT, I64, immediate=4), instruction(TUPLE_NEW, TUPLE, 2, 2),
        instruction(SLOT_GROUP_INIT, left=4, right=2, immediate=3), instruction(RETURN)]
    body = words(0, 2, 1)
    body += function('init', UNIT, init, () if unit else (0, 1, 3, 4, 2, 5))
    body += function('main', I64, [instruction(CONST_INT, I64, immediate=41), instruction(RETURN)])
    body += words(1, 3, 0, 0, 1)  # modules, slots, literals, root, entry
    body += words(4) + b'root' + words(0, 0)  # module name, dependencies, initializer
    body += words(0, 0, 0, 0, 0, 0, 0, 0, 0)
    body += words(0, 1, 0, 0, 0, 0, 0, 0, 0)
    body += words(0, UNIT if unit else TUPLE, 0, 0, UNIT, 0, 0, UNIT if unit else TUPLE, 0)
    body += words(0, 0)  # implementations, generic table absent
    body += words(1, 0, 0, 6, 0, 2, I64, I64)  # one Tuple2, span0, two mode-zero fields
    body += words(0, 0)  # defaults/provenance absent
    header = b'XRCHK\0\0\0' + words(25, 65, 2, 0) + struct.pack('<Q', len(body))
    return header + hashlib.sha256(header + body).digest() + body

def derive():
    rows = []
    packets = {}
    lines = ['/* Independent fixed-role wire25 semantic65 SLOT_GROUP_INIT packets. */']
    for name, unit in [('slot_group_mixed65_golden', False), ('slot_group_unit65_golden', True)]:
        raw = packet(unit)
        packets[name] = raw
        lines.append('static const uint8_t ' + name + '[] = {')
        lines += ['    ' + ','.join('0x%02x' % byte for byte in raw[i:i+12]) + ',' for i in range(0, len(raw), 12)]
        lines.append('};')
        rows.append({'name': name, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(),
                     'body_sha256': hashlib.sha256(raw[64:]).hexdigest()})
    return "\n".join(lines) + "\n", rows, packets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--check', type=Path, help='Verify the independent current header without writing.')
    action.add_argument('--output-dir', type=Path, help='Create a new directory with complete packets and their header.')
    args = parser.parse_args()
    header, rows, packets = derive()
    if args.check is not None:
        if args.check.read_text(encoding='utf-8') != header:
            print('independent SLOT_GROUP_INIT header mismatch', file=sys.stderr)
            return 1
    else:
        args.output_dir.mkdir(parents=True, exist_ok=False)
        (args.output_dir / 'slot_group65_golden.h').write_text(header, encoding='utf-8', newline='\n')
        for name, raw in packets.items():
            (args.output_dir / (name + '.xrc')).write_bytes(raw)
        (args.output_dir / 'manifest.json').write_text(json.dumps({
            'oracle': 'fixed role construction, no compiler output',
            'opcodes': {'CONST_INT': 2, 'RETURN': 33, 'TUPLE_NEW': 143, 'SLOT_GROUP_INIT': 145},
            'packets': rows}, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(rows))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
