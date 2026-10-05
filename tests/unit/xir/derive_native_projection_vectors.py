"""Independently encode the fixed C11 policy framing; do not import the owner."""
import hashlib
import json
from pathlib import Path
import re
import struct

source = Path(__file__).with_name('xir_native_projection_expectations.h').read_text(encoding='utf-8')
history = json.loads(Path(__file__).with_name('native_projection_identity_history.json').read_text(encoding='utf-8'))
historical = {'linked': '93bc0b9de2044c6174e9bc27d1f746c6165e7fd8a887e8b86a4e2023acb4a3d9',
              'original': '202299fec76eead7f466d98391f9272a65a9fb33685b630fb2e95c78468bdcb1'}
assert len(history['records']) == 4
for name in ('linked', 'original'):
    prefix = (name + '_source').encode('ascii')
    for identity, versions in (('historical', (23, 61, 19)), ('current', (24, 63, 20))):
        row = next(row for row in history['records'] if row['role'] == name and row['identity'] == identity)
        framing = b'xray:xir-c11-codegen-policy:v1' + struct.pack('<7I', 1, *versions, 25, 28, len(prefix)) + prefix
        assert row['preimage'] == framing.hex() and row['length'] == len(framing)
        assert row['sha256'] == hashlib.sha256(framing).hexdigest()
        if identity == 'historical':
            assert row['sha256'] == historical[name]
    framing = b'xray:xir-c11-codegen-policy:v1' + struct.pack('<7I', 1, 24, 63, 20, 25, 28, len(prefix)) + prefix
    expected = hashlib.sha256(framing).digest()
    match = re.search(name + r'_policy\[32\]=\{([^}]+)\}', source)
    actual = bytes(int(byte, 16) for byte in match[1].split(','))
    assert actual == expected
    print(name, len(framing), expected.hex(), 'PASS')
