#!/usr/bin/env python3
"""Encode fixed scalar packets from complete fields, without a compiler writer.

The three counted-name models include embedded NUL and empty-name wire cases.
Old63/64/65 literals are comparison inputs; they are never rewritten.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct


def fixed_program(name: bytes, schema: int, semantic: int, return_op: int) -> bytes:
    def u32(*values: int) -> bytes:
        return struct.pack('<' + 'I' * len(values), *values)

    def instruction(op: int, value_type: int, immediate: int) -> bytes:
        return u32(op, value_type, 0, 0, 0, 0) + struct.pack('<Q', immediate) + u32(0, 0)

    body = u32(0, 1, 0, len(name)) + name
    body += u32(0, 2, 1, 0, 2, 0, 0, 2)
    body += instruction(2, 2, 42) + instruction(return_op, 0, 0)
    # Operands, generics, three type-table counts, defaults, ABSENT provenance.
    body += u32(0, 0, 0, 0, 0, 0, 0)
    prefix = b'XRCHK\0\0\0' + u32(schema, semantic, 2, 0) + struct.pack('<Q', len(body))
    return prefix + hashlib.sha256(prefix + body).digest() + body


def literal(source: str, symbol: str) -> bytes:
    match = re.search(r'static const uint8_t ' + re.escape(symbol) + r'\[(\d*)\]\s*=\s*\{(.*?)\};', source, re.S)
    assert match, symbol
    data = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', match[2]))
    assert not match[1] or len(data) == int(match[1])
    return data


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, default=here.parents[3])
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    old = args.source_root / 'tests/unit/xir/compile_owner'
    checked = (args.source_root / 'src/xir/xxir_checked.h').read_text('utf-8')
    assert re.search(r'XR_XIR_CHECKED_SCHEMA\s+26u', checked)
    assert re.search(r'XR_XIR_CHECKED_CONTRACT\s+71u', checked)
    ops = re.findall(r'^XR_XIR_OP\((\w+)', (args.source_root / 'src/xir/xxir_ops.def').read_text('utf-8'), re.M)
    assert ops[1] == 'CONST_INT' and ops[32] == 'RETURN' and len(ops) == 151
    rows = [('golden', b'main'), ('embedded_golden', b'm\0in'), ('empty_golden', b'')]
    output = ['/* Independent complete Checked26/semantic71 scalar field models. */',
              '#ifndef CHECKED_SIZING71_GOLDEN_H', '#define CHECKED_SIZING71_GOLDEN_H', '#include <stdint.h>']
    evidence = []
    for suffix, name in rows:
        for filename, prefix, schema, semantic, ret in [
                ('checked_sizing_cases.h', 'sizing_', 24, 63, 25),
                ('checked_sizing64_golden.h', 'sizing64_', 25, 64, 33),
                ('checked_sizing65_golden.h', 'sizing65_', 25, 65, 33)]:
            previous = fixed_program(name, schema, semantic, ret)
            assert literal((old / filename).read_text('utf-8'), prefix + suffix) == previous
        packet = fixed_program(name, 26, 71, 33)
        assert len(packet) == 220 + len(name) and packet[-4:] == b'\0'*4
        assert fixed_program(name, 25, 65, 33)[64:] == packet[64:]
        symbol = 'sizing71_' + suffix
        output.append('static const uint8_t ' + symbol + '[' + str(len(packet)) + '] = {')
        output.extend('    ' + ', '.join('0x%02x' % b for b in packet[i:i+12]) + ',' for i in range(0, len(packet), 12))
        output.append('};')
        evidence.append({'symbol': symbol, 'name_hex': name.hex(), 'bytes': len(packet),
                         'sha256': hashlib.sha256(packet).hexdigest(), 'digest': packet[32:64].hex()})
    output.append('#endif')
    text = '\n'.join(output) + '\n'
    target = here / 'checked_sizing71_golden.h'
    if args.write:
        target.write_text(text, encoding='utf-8', newline='\n')
    else:
        assert target.read_text('utf-8') == text
    print(json.dumps({'status': 'INDEPENDENT_COMPLETE_SIZING_26_71_FIELDS',
                      'previous_complete_vectors_reproduced': 9, 'vectors': evidence,
                      'production_writer_used': False, 'Xray_build_or_test_run': False}, indent=2))


if __name__ == '__main__':
    main()
