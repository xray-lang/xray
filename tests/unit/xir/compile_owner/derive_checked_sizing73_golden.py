#!/usr/bin/env python3
"""Derive complete scalar Checked28/73 fields without a product writer.

Historical24/63,25/64,25/65,26/71 and27/72 packets remain exact comparison
inputs. The28/73 scalar body has the same explicit mandatory zero counts as
27/72; only the two header identities and their full digest change.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def packet(name, schema, semantic, return_op, construction):
    def instruction(op, value_type, immediate):
        return words(op, value_type, 0, 0, 0, 0) + struct.pack('<Q', immediate) + words(0, 0)
    # Program linkage, one function, absent declarations and exact counted name.
    body = words(0, 1, 0, len(name)) + name
    # Zero formals, I64 result, one complete block and two instructions.
    body += words(0, 2, 1, 0, 2, 0, 0, 2)
    body += instruction(2, 2, 42) + instruction(return_op, 0, 0)
    # Zero operands; absent generics; zero type, nominal and interface counts.
    body += words(0, 0, 0, 0, 0)
    # The current dense construction denominator is zero, never omitted.
    if construction:
        body += words(0)
    # Zero defaults and ABSENT provenance are independent wire fields.
    body += words(0, 0)
    prefix = b'XRCHK\0\0\0' + words(schema, semantic, 2, 0) + struct.pack('<Q', len(body))
    return prefix + hashlib.sha256(prefix + body).digest() + body


def literal(path, name):
    match = re.search(r'\b' + re.escape(name) + r'\s*\[(\d*)\]\s*=\s*\{(.*?)\};',
                      path.read_text(encoding='utf-8'), re.S)
    assert match, (path, name)
    data = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', match[2]))
    assert not match[1] or len(data) == int(match[1])
    assert struct.unpack_from('<Q', data, 24)[0] == len(data) - 64
    assert hashlib.sha256(data[:32] + data[64:]).digest() == data[32:64]
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    directory = args.source_root / 'tests/unit/xir/compile_owner'
    checked = (args.source_root / 'src/xir/xxir_checked.h').read_text(encoding='utf-8')
    assert re.search(r'XR_XIR_CHECKED_SCHEMA\s+28u', checked)
    assert re.search(r'XR_XIR_CHECKED_CONTRACT\s+73u', checked)
    ops = re.findall(r'^XR_XIR_OP\((\w+)',
                     (args.source_root / 'src/xir/xxir_ops.def').read_text(encoding='utf-8'), re.M)
    assert ops[1] == 'CONST_INT' and ops[32] == 'RETURN'
    lines = ['/*', ' * xray - Lightweight typed scripting with native concurrency',
             ' * https://www.xray-lang.org', ' *',
             ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>',
             ' * Licensed under the MIT License', ' *',
             ' * checked_sizing73_golden.h - Independent complete scalar wire fields',
             ' */', '#ifndef CHECKED_SIZING73_GOLDEN_H', '#define CHECKED_SIZING73_GOLDEN_H',
             '#include <stdint.h>',
             '_Static_assert(XR_XIR_CHECKED_SCHEMA == 28u && XR_XIR_CHECKED_CONTRACT == 73u &&',
             '    XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33 && XR_XIR_I64 == 2 && XR_XIR_UNIT == 0,',
             '    "Independent scalar wire field identities changed");']
    records = []
    historical = []
    for suffix, name in [('golden', b'main'), ('embedded_golden', b'm\0in'), ('empty_golden', b'')]:
        for filename, prefix, schema, semantic, ret, construction in [
            ('checked_sizing_cases.h', 'sizing_', 24, 63, 25, False),
            ('checked_sizing64_golden.h', 'sizing64_', 25, 64, 33, False),
            ('checked_sizing65_golden.h', 'sizing65_', 25, 65, 33, False),
            ('checked_sizing71_golden.h', 'sizing71_', 26, 71, 33, False),
            ('checked_sizing72_golden.h', 'sizing72_', 27, 72, 33, True)]:
            old = literal(directory / filename, prefix + suffix)
            assert old == packet(name, schema, semantic, ret, construction), (filename, suffix)
            historical.append(dict(symbol=prefix + suffix, bytes=len(old),
                                   sha256=hashlib.sha256(old).hexdigest()))
        current = packet(name, 28, 73, 33, True)
        previous = literal(directory / 'checked_sizing72_golden.h', 'sizing72_' + suffix)
        assert current[64:] == previous[64:] and len(current) == 224 + len(name)
        symbol = 'sizing73_' + suffix
        lines.append('static const uint8_t ' + symbol + '[] = {')
        lines.extend('    ' + ', '.join('0x%02x' % v for v in current[i:i+12]) + ','
                     for i in range(0, len(current), 12))
        lines.append('};')
        records.append(dict(symbol=symbol, name_hex=name.hex(), bytes=len(current),
                            body_bytes=len(current)-64, sha256=hashlib.sha256(current).hexdigest(),
                            digest=current[32:64].hex(), old_body_equal=True))
    lines.append('#endif // CHECKED_SIZING73_GOLDEN_H')
    text = '\n'.join(lines) + '\n'
    target = args.output_root / 'tests/unit/xir/compile_owner/checked_sizing73_golden.h'
    if args.write:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding='utf-8', newline='\n')
    else:
        assert target.read_text(encoding='utf-8') == text
    print(json.dumps(dict(status='INDEPENDENT_FIELDS_ONLY_NO_C_RUN',
                         historical_exact_vectors=historical, current_vectors=records,
                         product_writer_used=False, C_execution='NOT_RUN'), indent=2))

if __name__ == '__main__':
    main()
