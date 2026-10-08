#!/usr/bin/env python3
"""Encode a minimal typed element store and read independently of the codec."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def encode():
    def w(*values): return struct.pack('<' + 'I' * len(values), *values)
    def op(code, kind=0, a=0, b=0, immediate=0):
        return w(code, kind, a, b, 0, 0) + struct.pack('<q', immediate) + w(0, 0)
    name = b'element_place'
    body = w(0, 2, 1) + w(len(name)) + name + w(0, 2, 1, 0, 8, 0, 0, 8)
    body += op(2, 2, immediate=42) + op(73, 256, 0, 1)
    body += op(59, 257, 1) + op(71, 256, 2) + op(2, 2)
    body += op(75, 0, 1, 3) + op(74, 2, 3, 4) + op(33, a=6)
    body += w(4, 0, 3, 4, 0)
    body += w(4) + b'init' + w(0, 0, 1, 0, 1, 0, 0, 1) + op(33) + w(0)
    body += w(1, 0, 0, 0, 0, 4) + b'root' + w(0, 1)
    body += w(0, 1, 0, 0, 0, 0, 0, 0, 0) + w(*([0]*9)) + w(0)
    body += w(0, 2, 0, 0, 2, 0, 2, 3, 0, 256, 0, 0)
    header = b'XRCHK\0\0\0' + w(25, 67, 2, 0) + struct.pack('<Q', len(body))
    return header + hashlib.sha256(header + body).digest() + body


def artifacts():
    packet = encode()
    code = ['/* Independent scalar Array element place Checked frame. */',
            'static const uint8_t element_place_frame[] = {']
    for offset in range(0, len(packet), 24):
        code.append('    ' + ','.join(f'0x{x:02x}' for x in packet[offset:offset+24]) + ',')
    code += ['};', '']
    manifest = dict(schema=1, identity='Checked25/semantic67', bytes=len(packet),
                    sha256=hashlib.sha256(packet).hexdigest(), functions=2, instructions=[8, 1],
                    types=['Array<i64>', 'Cell<Array<i64>>'], operands=[0, 3, 4, 0],
                    ordinary_truncations=len(packet), reframed_truncations=len(packet)-64,
                    digest_corruptions=len(packet)-32, trailing_frames=1,
                    refusal_transitions_per_configuration=2*(3*len(packet)-95),
                    boundary='Checked format and detached admission only; old decoder record caps, FI and loan semantics remain OPEN.')
    return '\n'.join(code), json.dumps(manifest, indent=2)+'\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    for path, content in zip((DIR/'element_place_frame.inc.c', DIR/'element_place_frame.json'), artifacts()):
        if args.mode == 'write': path.write_text(content, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != content: raise ValueError(f'independent frame differs: {path}')
    print(f'Element place frame: {len(encode())} bytes; all ordinary and reframed truncations; exact digest and trailing refusals')
