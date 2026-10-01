"""Independent scalar and whole-packet vectors with verified historical framing."""
import argparse
import hashlib
import json
import struct
import re
from pathlib import Path


def packet(contract, schema=21):
    def words(*values):
        return struct.pack('<' + 'I' * len(values), *values)

    # Program linkage, one function and no declaration table.
    body = words(0, 1, 0)
    # n() -> i64; one block, no panic handler or cleanup frontier.
    body += words(1) + b'n' + words(0, 2, 1, 0, 2, 0, 0, 2)
    # CONST_INT has identity2; RETURN has identity25. No value/edge operands.
    body += words(2, 2, 0, 0, 0, 0) + struct.pack('<q', -(1 << 63)) + words(0, 0)
    body += words(25, 0, 0, 0, 0, 0) + struct.pack('<q', 0) + words(0, 0)
    # Empty operands, generics, type/nominal/interface pools, defaults, provenance.
    body += words(0, 0, 0, 0, 0, 0, 0)
    assert len(body) == 157
    header = b'XRCHK\0\0\0' + words(schema, contract, 2, 0) + struct.pack('<Q', len(body))
    assert len(header) == 32
    digest = hashlib.sha256(header + body).digest()
    return header + digest + body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    old = packet(53)
    assert old[32:64].hex() == 'f3c1c94e69901e9c9cd5d5f7cf7357d2f35aa143352ae1ec84a860b52a650dbd'
    previous = packet(54)
    assert previous[32:64].hex() == '546ef2715f595cc3c8e013829bff6b76dc4fbcd263c921c7c2d683fb17bedc24'
    previous55 = packet(55)
    assert previous55[32:64].hex() == 'e1a0d96d358b17400c34000266e978426ecdaf8b0e9e11e833f853a63d87b1c3'
    previous56 = packet(56,22)
    current = packet(57,22)
    assert previous56[32:64].hex() == 'ad6f38d1cd3ce90b74cac518abc41bd987cb6809ab4d86f4049260a790101c34'
    assert old[:8] == current[:8] and old[16:32] == current[16:32] and old[64:] == current[64:]
    directory = Path(__file__).parent
    manifest = json.loads((directory / 'panic_carrier_packet_vectors.json').read_text(encoding='utf-8'))
    for record in manifest['current'] + manifest['retired_unchanged']:
        text = (directory / record['path']).read_text(encoding='utf-8')
        match = re.search(r'static const uint8_t ' + record['name'] + r'\[\] = \{(.*?)\};', text, re.S)
        literal = bytes(int(value,16) for value in re.findall(r'0x[0-9a-fA-F]{2}', match[1]))
        assert literal[:8] == b'XRCHK\0\0\0' and struct.unpack_from('<Q',literal,24)[0] == len(literal)-64
        assert hashlib.sha256(literal[:32]+literal[64:]).digest() == literal[32:64]
        if 'sha256' in record:
            assert hashlib.sha256(literal).hexdigest() == record['sha256']
            assert struct.unpack_from('<II',literal,8) == (record['schema'],record['contract'])
            continue
        assert struct.unpack_from('<II',literal,8) == (22,57)
        assert record['previous_contract'] == 54
        previous_header = bytearray(literal[:32]); struct.pack_into('<II', previous_header, 8, 21,54)
        assert hashlib.sha256(previous_header + literal[64:]).hexdigest() == record['previous_digest']
        assert len(literal) == record['bytes'] and literal[32:64].hex() == record['current_digest']
        assert hashlib.sha256(literal[64:]).hexdigest() == record['body_sha256']
        previous55_header=bytearray(literal[:32]);struct.pack_into('<II',previous55_header,8,21,55)
        assert hashlib.sha256(previous55_header+literal[64:]).hexdigest()==record['previous55_digest']
        historical_header = bytearray(literal[:32]); struct.pack_into('<II',historical_header,8,21,53)
        assert hashlib.sha256(historical_header+literal[64:]).hexdigest() == record['old_digest']
    result = {'schema': 22, 'bytes': 221, 'historical_schema':21,'old_contract': 53, 'current_contract': 57,
        'old_digest': old[32:64].hex(), 'current_digest': current[32:64].hex(),
        'body_sha256': hashlib.sha256(current[64:]).hexdigest(),
        'old_packet_hex': old.hex(), 'current_packet_hex': current.hex(),
        'verified_static_current_vectors': len(manifest['current']),
        'retired_schema20_21_semantic51_52_vectors_preserved': len(manifest['retired_unchanged'])}
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding='utf-8')
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
