#!/usr/bin/env python3
"""Encode independent module-place authority, dominance and value-role cases."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def cases():
    rows = []
    def add(name, mode, expected=0, **changes):
        row = dict(name=name, mode=mode, expected=expected, owner=3, caller=3,
                   mutable=1, slot_type=256, place_type=256, index_type=2,
                   index_value_type=2, write_value=1)
        row.update(changes)
        rows.append(row)
    add('root_mutable_cross_write', 'write')
    add('root_const_cross_read', 'read', mutable=0)
    add('root_mutable_cross_read', 'read')
    add('base_const_cross_read', 'read', owner=0, caller=0, mutable=0)
    add('root_mutable_cross_index_write', 'index_write')
    add('root_const_cross_index_read', 'index_read', mutable=0)
    add('root_mutable_value_phi', 'value_phi')
    add('root_mutable_value_call', 'value_call')
    add('root_mutable_value_copy', 'value_copy')
    add('root_const_cross_write', 'write', 1, mutable=0)
    add('root_const_cross_index_write', 'index_write', 1, mutable=0)
    add('base_const_cross_write', 'write', 1, owner=0, caller=0, mutable=0)
    add('base_const_cross_index_write', 'index_write', 1, owner=0, caller=0, mutable=0)
    add('foreign_place', 'read', 1, caller=0)
    add('place_type_mismatch', 'read', 3, place_type=257)
    add('scalar_slot_place', 'read', 3, slot_type=2, place_type=2)
    add('place_copy', 'place_copy', 4)
    add('place_phi', 'place_phi', 4)
    add('place_value_call', 'place_call', 4)
    add('place_return', 'place_return', 4)
    add('write_value_mismatch', 'write', 3, write_value=2)
    add('index_type_mismatch', 'index_read', 3, index_type=1)
    add('index_not_integer', 'index_read', 3, index_value_type=1)
    add('non_dominating_place', 'nondominating', 5)
    add('non_dominating_index', 'nondominating_index', 5)
    add('write_plain_value', 'plain_write', 4)
    assert len(rows) == 26
    return rows


def encode(m):
    def w(*v): return struct.pack('<' + 'I' * len(v), *v)
    def blob(v): return w(len(v)) + v
    def op(code, t=0, a=0, b=0, target=0, other=0, value=0):
        return w(code, t, a, b, target, other) + struct.pack('<Q', value) + w(0, 0)
    def fn(name, result, blocks, operands=(), parameters=()):
        ins = sum(blocks, [])
        data = blob(name) + w(len(parameters), *parameters, result, len(blocks))
        first = 0
        for block in blocks:
            data += w(first, len(block), 0, 0)
            first += len(block)
        return data + w(len(ins)) + b''.join(ins) + w(len(operands), *operands)
    mode = m['mode']
    prefix = [op(72, m['place_type']), op(73, 256),
              op(1 if m['index_value_type'] == 1 else 2, m['index_value_type'])]
    operands = []
    result = 0
    if mode.startswith('nondominating'):
        entry = [op(1, 1, value=1), op(2, 2), op(32, a=0, target=1, other=2)]
        left = [op(72, 256)]
        receiver = 3
        if mode == 'nondominating_index':
            left.append(op(113, 2, a=3, b=1))
            receiver = 4
        left.append(op(31, target=3))
        first = len(entry) + len(left) + 1
        tail = [op(73, 256), op(115, a=receiver, b=1 if mode.endswith('index') else first), op(33)]
        blocks = [entry, left, [op(31, target=3)], tail]
    else:
        if mode.startswith('index_'):
            prefix.append(op(113, m['index_type'], a=0, b=2))
            tail = [op(115, a=3, b=2) if mode == 'index_write' else op(114, m['index_type'], a=3)]
        elif mode in ('value_phi', 'value_call', 'value_copy'):
            prefix.append(op(114, 256, a=0))
            if mode == 'value_phi': operands = [0, 3]; tail = [op(56, 256, b=2)]
            elif mode == 'value_call': operands = [3]; tail = [op(28, b=1, value=6)]
            else: tail = [op(18, 256, a=3)]
        elif mode == 'place_phi': operands = [0, 0]; tail = [op(56, 256, b=2)]
        elif mode == 'place_call': operands = [0]; tail = [op(28, b=1, value=6)]
        elif mode == 'place_copy': tail = [op(18, 256)]
        elif mode == 'place_return': result = 256; tail = []
        elif mode == 'plain_write': tail = [op(115, a=1, b=1)]
        elif mode == 'write': tail = [op(115, a=0, b=m['write_value'])]
        else: tail = [op(114, m['place_type'], a=0)]
        blocks = [prefix + [op(31, target=1)], tail + [op(33)]]
    body = w(0, 7, 1)
    for i in range(4): body += fn(('init' + str(i)).encode(), 0, [[op(33)]])
    body += fn(b'entry', 2, [[op(2, 2, value=42), op(33)]])
    body += fn(b'place_consumer', result, blocks, operands)
    body += fn(b'value_consumer', 0, [[op(33)]], parameters=[256])
    body += w(4, 1, 0, 3, 4)
    for i, (name, deps) in enumerate([(b'base', []), (b'left', [0]), (b'right', [0]), (b'main', [1, 2])]):
        body += blob(name) + w(len(deps), *deps, i)
    for module in (0, 1, 2, 3, 3, m['caller'], m['caller']): body += w(module, 0, 0, 0, 0, 0, 0, 0, 0)
    body += w(m['owner'], m['slot_type'], m['mutable'])
    body += w(0, 0, 2, 0, 0) + w(2, 0, 2) + w(2, 0, 1) + w(0, 0)
    head = b'XRCHK\0\0\0' + w(25, 67, 2, 0) + struct.pack('<Q', len(body))
    shape = dict(blocks=len(blocks), instructions=sum(map(len, blocks)), result=result)
    return head + hashlib.sha256(head + body).digest() + body, shape


def artifacts():
    manifest = dict(schema=1, controls=9, rejects=17, cases=[])
    lines = ['/* Current logical-place roles are distinct from legacy ref-call contracts. */']
    for i, row in enumerate(cases()):
        packet, shape = encode(row)
        lines.append(f'static const uint8_t module_place_packet_{i}[] = {{')
        for off in range(0, len(packet), 64): lines.append('    ' + ','.join(f'0x{x:02x}' for x in packet[off:off+64]) + ',')
        lines.append('};')
        manifest['cases'].append(dict(**row, **shape, bytes=len(packet), sha256=hashlib.sha256(packet).hexdigest()))
    assert len({r['sha256'] for r in manifest['cases']}) == 26
    lines += ['typedef struct ModulePlaceCase {',
              '    const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected;',
              '    unsigned owner, caller, mutable, slot_type, blocks, instructions, result;',
              '} ModulePlaceCase;', 'static const ModulePlaceCase module_place_cases[] = {']
    for i, row in enumerate(manifest['cases']):
        fields = ','.join(str(row[k]) for k in ('expected', 'owner', 'caller', 'mutable', 'slot_type', 'blocks', 'instructions', 'result'))
        lines.append(f'    {{"{row["name"]}",module_place_packet_{i},sizeof(module_place_packet_{i}),{fields}}},')
    lines += ['};', '']
    manifest['boundary'] = 'Admission only. Nine current Array controls and seventeen exact rejects. Legacy scalar branch arguments and ref direct/interface calls remain OPEN; rejecting a place as an ordinary value is not a ref-call replacement.'
    return '\n'.join(lines), manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    header, manifest = artifacts()
    for path, text in {DIR/'module_place_cases.h': header, DIR/'module_place_cases.json': json.dumps(manifest, indent=2)+'\n'}.items():
        if args.mode == 'write': path.write_text(text, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != text: raise ValueError(f'independent module-place input differs: {path}')
    print('Module places:26 independent inputs;9 current controls,17 exact rejects;legacy ref and scalar place duties remain OPEN')
