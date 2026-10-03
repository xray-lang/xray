"""Independently encode the fixed C11 policy framing; do not import the owner."""
import hashlib
from pathlib import Path
import re
import struct

source = Path(__file__).with_name('xir_native_projection_expectations.h').read_text(encoding='utf-8')
for name in ('linked', 'original'):
    prefix = (name + '_source').encode('ascii')
    framing = b'xray:xir-c11-codegen-policy:v1' + struct.pack('<7I', 1, 23, 59, 18, 23, 28, len(prefix)) + prefix
    expected = hashlib.sha256(framing).digest()
    match = re.search(name + r'_policy\[32\]=\{([^}]+)\}', source)
    actual = bytes(int(byte, 16) for byte in match[1].split(','))
    assert actual == expected
    print(name, len(framing), expected.hex(), 'PASS')
