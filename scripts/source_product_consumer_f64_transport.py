#!/usr/bin/env python3
"""Independently encode binary64 transport with explicit module and entry authority."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from source_product_consumer_checked_f64 import LEGACY_BITS, artifacts

ROOT = Path(__file__).resolve().parents[1]


def fixture():
    artifacts()  # Check the unchanged original ten-pattern corpus.
    def words(*v):
        return struct.pack('<' + 'I' * len(v), *v)
    def ins(op, type_id, a=0, b=0, immediate=0):
        return words(op, type_id, a, b, 0, 0) + struct.pack('<Q', immediate) + words(0, 0)
    def function(name, first, operands):
        return (words(len(name)) + name + words(1, 13, 13, 1, 0, 2, 0, 0, 2) + first +
                ins(33, 0, 1) + words(len(operands), *operands))
    body = words(0, 4, 1)
    body += function(b'caller', ins(28, 13, 0, 1, 1), [0])
    body += function(b'carrier', ins(18, 13, 0), [])
    body += words(5) + b'entry' + words(0, 2, 1, 0, 2, 0, 0, 2)
    body += ins(2, 2) + ins(33, 0, 0) + words(0)
    body += words(4) + b'init' + words(0, 0, 1, 0, 1, 0, 0, 1)
    body += ins(33, 0) + words(0)
    body += words(1, 0, 0, 0, 2)  # modules, slots, literals, root, canonical entry
    body += words(9) + b'transport' + words(0, 3)  # dependencies, Unit initializer
    for index in range(4):
        body += words(0, 1 if index == 0 else 0, 0, 0, 0, 0, 0, 0, 0)
    body += words(0)  # implementation table
    body += words(0, 0, 0, 0, 0, 0)
    header = b'XRCHK\0\0\0' + words(25, 67, 2, 0) + struct.pack('<Q', len(body))
    data = header + hashlib.sha256(header + body).digest() + body
    lines = ['/* Independent named Checked packet; no production writer input. */',
             'static const uint8_t f64_transport_packet[] = {']
    for offset in range(0, len(data), 16):
        lines.append('    ' + ','.join(f'0x{x:02x}' for x in data[offset:offset + 16]) + ',')
    lines += ['};', 'static const uint64_t f64_transport_bits[] = {']
    lines += [f'    UINT64_C(0x{x:016x}),' for x in LEGACY_BITS]
    lines += ['};', '']
    return '\n'.join(lines), data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    text, data = fixture()
    path = ROOT / 'tests/unit/xir/product_consumers/f64_transport_fixture.h'
    if args.mode == 'write':
        path.write_text(text, encoding='utf8', newline='\n')
    elif path.read_text(encoding='utf8') != text:
        raise ValueError('independent binary64 transport packet or original corpus changed')
    print(json.dumps(dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), original_bits=10)))
