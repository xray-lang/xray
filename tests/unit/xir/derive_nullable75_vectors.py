"""Reproduce complete Nullable field models before framing their current peers.

The retired packets are comparisons only; no compiler or runtime writer is read.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from derive_construction72_vectors import frame, header, literal
from derive_role_construction72 import nullable_model, encode_body
from derive_nested_nullable_vectors import nested_packet

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    records = []
    ordinary = []
    for some in (False, True):
        body, _ = encode_body(nullable_model(some), 8, True)
        name = 'nullable_' + ('some' if some else 'none')
        assert frame(body, 27, 72) == literal(HERE / 'xir_nullable72_golden.h', name + '72_golden')
        ordinary.append((name + '75_golden', frame(body, 28, 75)))
    body = nested_packet(65)[64:]
    assert body[-8:] == bytes(8)
    body = body[:-8] + struct.pack('<I', 0) + body[-8:]
    assert frame(body, 27, 72) == literal(HERE / 'xir_nested_nullable72_golden.h', 'nested_nullable72_golden')
    nested = [('nested_nullable75_golden', frame(body, 28, 75))]
    for stem, rows in [('xir_nullable75_golden', ordinary), ('xir_nested_nullable75_golden', nested)]:
        header(HERE / (stem + '.h'), stem.upper() + '_H', rows, args.write)
        records.extend(dict(symbol=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
                       for name, data in rows)
    print(json.dumps(records, indent=2))


if __name__ == '__main__':
    main()
