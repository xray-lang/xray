"""Independent 23/59 framing; preserve complete 22/58 inputs as rejection evidence."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
from derive_semantic61_migration import semantic61_packet

DIRECTORY = Path(__file__).parent
ARRAY = re.compile(r'(?:static\s+)?const\s+uint8_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', re.S)


def literal(text, name):
    match = next(match for match in ARRAY.finditer(text) if match[1] == name)
    return bytes(int(value, 16) for value in re.findall(r'0x[0-9a-fA-F]{2}', match[2]))


def upgrade(previous):
    assert previous[:8] == b'XRCHK\0\0\0'
    assert struct.unpack_from('<II', previous, 8) == (22, 58)
    assert struct.unpack_from('<Q', previous, 24)[0] == len(previous)-64
    assert hashlib.sha256(previous[:32]+previous[64:]).digest() == previous[32:64]
    body = previous[64:]
    at = 0

    def word():
        nonlocal at
        value = struct.unpack_from('<I', body, at)[0]
        at += 4
        return value

    def skip(size):
        nonlocal at
        assert size <= len(body)-at
        at += size

    word()  # Linkage.
    functions, declarations = word(), word()
    for _ in range(functions):
        skip(word())  # Name.
        skip(4*word())  # Parameter types.
        word()  # Result.
        skip(16*word())  # Blocks.
        skip(40*word())  # Instructions.
        skip(4*word())  # Operands.
    if declarations:
        modules = word()
        skip(16)  # Slots, literals, root, entry.
        for _ in range(modules):
            skip(word())
            skip(4*word())
            word()  # Initializer.
        start = at
        skip(28*functions)
        # These fixed unspecialized vectors have no nested provenance module.
        # Their remaining payload is unchanged and the real reader verifies it.
        assert body[-4:] == bytes(4)
        identities = b''.join(body[start+28*f:start+28*(f+1)]+bytes(8) for f in range(functions))
        body = body[:start]+identities+body[at:]
    header = bytearray(previous[:32])
    struct.pack_into('<II', header, 8, 23, 59)
    struct.pack_into('<Q', header, 24, len(body))
    return bytes(header)+hashlib.sha256(header+body).digest()+body


def historical_vector(path, name):
    rows = json.loads((DIRECTORY/'test_roles_packet_vectors.json').read_text(encoding='utf-8'))
    row = next(row for row in rows if row['path'] == path and row['name'] == name)
    old = bytes.fromhex(row['previous58_hex'])
    current = literal((DIRECTORY/path).read_text(encoding='utf-8'), name)
    historical59 = upgrade(old)
    assert hashlib.sha256(historical59).hexdigest() == row['current59_sha256']
    assert current == semantic61_packet(historical59)
    return old


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    manifest = DIRECTORY/'test_roles_packet_vectors.json'
    if args.write:
        assert not manifest.exists(), 'preserved historical bytes must not be overwritten'
        records = json.loads((DIRECTORY/'equal57_ordinary_vectors.json').read_text(encoding='utf-8'))
        records += [dict(path='xir_nullable_golden.h', name=name) for name in ('nullable_none_golden', 'nullable_some_golden')]
        records += [dict(path=f'xir_assert_{name}_golden.h', name=f'assert_{name}_golden') for name in ('equal', 'panics')]
        rows = []
        for record in records:
            path = DIRECTORY/record['path']
            text = path.read_text(encoding='utf-8')
            match = next(match for match in ARRAY.finditer(text) if match[1] == record['name'])
            previous = literal(text, record['name'])
            current = upgrade(previous)
            rows.append(dict(path=path.name, name=record['name'], previous58_hex=previous.hex(),
                             previous58_sha256=hashlib.sha256(previous).hexdigest(), current59_sha256=hashlib.sha256(current).hexdigest()))
            content = '\n'+'\n'.join('    '+','.join(f'0x{x:02x}' for x in current[i:i+12])+',' for i in range(0,len(current),12))+'\n'
            path.write_text(text[:match.start(2)]+content+text[match.end(2):], encoding='utf-8', newline='\n')
        manifest.write_text(json.dumps(rows, indent=2)+'\n', encoding='utf-8', newline='\n')
    rows = json.loads(manifest.read_text(encoding='utf-8'))
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
    for index, row in enumerate(rows):
        previous = historical_vector(row['path'], row['name'])
        if args.output:
            (args.output/f'{index:02d}-old.chk').write_bytes(previous)
            (args.output/f'{index:02d}-current.chk').write_bytes(semantic61_packet(upgrade(previous)))
    print(f'{len(rows)} independent role-zero packets: complete old22/58 and historical23/59 preserved; current23/61 verified')


if __name__ == '__main__':
    main()
