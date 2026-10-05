#!/usr/bin/env python3
"""Check independent historical and current Source layout framing."""
from pathlib import Path
import hashlib
import re
import struct

DOMAIN = b'xray:xir-lowered-layout:v1'
HISTORICAL = '4e4205a7fe9b388761a34c975f9cf7f55bc8a163510baffaf700fd6b06e37a23'
CURRENT = '5325dff66ebc0db50f20dda2ed30b002e4b6fbbdac04c95a5889d755407a789a'

# String identity has one parameter and one void RETURN slot. Unit init has
# one void RETURN slot. I64 entry has one scalar slot and one void RETURN.
# Every result uses the 16-byte boxed ABI, including Unit.
FUNCTIONS = [
    0, 1, 2, 8, 16, 8, 1, 0, 0, 0, 0xffffffff, 16, 8, 0,
    1, 0, 1, 0, 16, 8, 0, 0, 0, 0xffffffff,
    2, 0, 2, 8, 16, 8, 0, 0, 0, 0, 0xffffffff,
]

def preimage(value, call, program):
    words = [1, 1, value, call, program, 2, 3, 1] + FUNCTIONS
    assert len(words) == 43
    data = DOMAIN + b''.join(struct.pack('<I', word) for word in words)
    assert len(data) == 198
    return data

def main():
    historical = preimage(20, 25, 28)
    current = preimage(21, 26, 29)
    assert hashlib.sha256(historical).hexdigest() == HISTORICAL
    assert hashlib.sha256(current).hexdigest() == CURRENT
    assert historical[:34] == current[:34] and historical[46:] == current[46:]
    header = Path(__file__).with_name('xir_source_product_identity.h').read_text()
    match = re.search(r'static const uint8_t expected\[\]=\{([^}]+)\}', header)
    assert match
    actual = bytes(int(word.strip(), 16) for word in match.group(1).split(','))
    assert actual.hex() == CURRENT and actual.hex() != HISTORICAL
    print('Source layout: immutable 198-byte historical KAT and independent current 43-word framing PASS')

if __name__ == '__main__':
    main()
