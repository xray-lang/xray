"""Derive lexical-scope packets from complete independent retired field vectors.

No compiler writer or executable is consulted. Original packets stay unchanged;
their complete digest is checked before explicit module fields are extended.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
from checked_scope_wire import receive_scopes, receive_offset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    directory = args.source_root / 'tests/unit/xir'
    records = []
    for source in sorted(directory.rglob('*74_golden.h')):
        text = source.read_text(encoding='utf-8')
        positions = []
        digest = None

        def row(match):
            nonlocal positions, digest
            name = match[1]
            data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', match[2]))
            retired = data if source.name == 'checked_sizing74_golden.h' else None
            if data[:8] == b'XRCHK\0\0\0':
                assert struct.unpack_from('<Q', data, 24)[0] == len(data) - 64
                assert hashlib.sha256(data[:32] + data[64:]).digest() == data[32:64]
                if struct.unpack_from('<II', data, 8) == (28, 74):
                    body, inserted = receive_scopes(data[64:])
                    if not positions:
                        positions = inserted
                    else:
                        assert positions == inserted
                    header = b'XRCHK\0\0\0' + struct.pack('<IIIIQ', 29, 75, 2, 0, len(body))
                    digest = hashlib.sha256(header + body).digest()
                    data = header + digest + body
                    records.append(dict(symbol=name.replace('74', '75'), modules=len(inserted),
                                        bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
            elif name.endswith('74_digest'):
                assert digest is not None
                data = digest
            lines = ['static const uint8_t ' + name.replace('74', '75') + '[] = {']
            lines.extend('    ' + ', '.join('0x%02x' % b for b in data[i:i+12]) + ','
                         for i in range(0, len(data), 12))
            result = '\n'.join(lines) + '\n};'
            if retired is not None:
                result += '\nstatic const uint8_t retired_' + name + '[] = {\n'
                result += ''.join('    ' + ', '.join('0x%02x' % b for b in retired[i:i+12]) + ',\n'
                                  for i in range(0, len(retired), 12)) + '};'
            return result

        text = re.sub(r'static const uint8_t (\w+)\[\] = \{(.*?)\};', row, text, flags=re.S)
        def macro(match):
            name, value = match[1], int(match[2])
            if 'VECTOR_' in name or '_OWN' in name:
                value = receive_offset(value, positions)
            return '#define ' + name.replace('74', '75') + ' ' + str(value) + match[3]
        text = re.sub(r'#define (\w*74\w*) (\d+)(u?)\b', macro, text)
        text = text.replace('74_GOLDEN_H', '75_GOLDEN_H').replace('74_golden.h', '75_golden.h')
        text = text.replace('SCHEMA == 28u', 'SCHEMA == 29u').replace('CONTRACT == 74u', 'CONTRACT == 75u')
        output = source.with_name(source.name.replace('74', '75'))
        if args.write:
            output.write_text(text, encoding='utf-8', newline='\n')
        else:
            assert output.read_text(encoding='utf-8') == text, str(output)
    print(json.dumps(dict(records=records, product_writer_used=False), indent=2))


if __name__ == '__main__':
    main()
