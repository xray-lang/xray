#!/usr/bin/env python3
"""Derive complete scalar Checked30/76 packets from explicit wire fields."""
from pathlib import Path
import argparse
import hashlib
import json
import re

from derive_checked_sizing74_golden import literal, packet


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    root = args.source_root
    directory = root / 'tests/unit/xir/compile_owner'
    checked = (root / 'src/xir/xxir_checked.h').read_text(encoding='utf-8')
    assert re.search(r'XR_XIR_CHECKED_SCHEMA\s+30u', checked)
    assert re.search(r'XR_XIR_CHECKED_CONTRACT\s+76u', checked)
    ops = re.findall(r'^XR_XIR_OP\((\w+)',
                     (root / 'src/xir/xxir_ops.def').read_text(encoding='utf-8'), re.M)
    assert ops[1] == 'CONST_INT' and ops[32] == 'RETURN'
    lines = ['/*', ' * xray - Lightweight typed scripting with native concurrency',
             ' * https://www.xray-lang.org', ' *',
             ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>',
             ' * Licensed under the MIT License', ' *',
             ' * checked_sizing76_golden.h - Independent complete scalar wire fields',
             ' */', '#ifndef CHECKED_SIZING76_GOLDEN_H', '#define CHECKED_SIZING76_GOLDEN_H',
             '#include <stdint.h>',
             '_Static_assert(XR_XIR_CHECKED_SCHEMA == 30u && XR_XIR_CHECKED_CONTRACT == 76u &&',
             '    XR_XIR_CONST_INT == 2 && XR_XIR_RETURN == 33 && XR_XIR_I64 == 2 && XR_XIR_UNIT == 0,',
             '    "Independent scalar wire field identities changed");']
    records = []
    for suffix, name in [('golden', b'main'), ('embedded_golden', b'm\0in'), ('empty_golden', b'')]:
        previous = literal(directory / 'checked_sizing75_golden.h', 'sizing75_' + suffix)
        assert previous == packet(name, 29, 75, 33, True)
        current = packet(name, 30, 76, 33, True)
        assert current[64:] == previous[64:]
        symbol = 'sizing76_' + suffix
        lines.append('static const uint8_t ' + symbol + '[] = {')
        lines.extend('    ' + ', '.join('0x%02x' % v for v in current[i:i+12]) + ','
                     for i in range(0, len(current), 12))
        lines.append('};')
        records.append(dict(symbol=symbol, bytes=len(current),
                            previous_exact_sha256=hashlib.sha256(previous).hexdigest(),
                            sha256=hashlib.sha256(current).hexdigest(),
                            digest=current[32:64].hex()))
    lines.append('#endif // CHECKED_SIZING76_GOLDEN_H')
    text = '\n'.join(lines) + '\n'
    target = directory / 'checked_sizing76_golden.h'
    if args.write:
        target.write_text(text, encoding='utf-8', newline='\n')
    else:
        assert target.read_text(encoding='utf-8') == text
    print(json.dumps(dict(status='INDEPENDENT_FIELDS_ONLY_NO_C_RUN',
                         vectors=records, product_writer_used=False), indent=2))


if __name__ == '__main__':
    main()
