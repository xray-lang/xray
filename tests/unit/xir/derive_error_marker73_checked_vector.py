"""Encode the complete ERROR-binder throw definition without a product writer.

The complete previous 71 model reproduces its original 205-byte shape. The
mandatory zero construction denominator is encoded explicitly for 72 and 73.
No compiler executable or emitted packet is a model input.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import struct


MODEL = {
    'linkage': 0,
    'functions': [{'name': 'e', 'parameters': [65536], 'result': 2,
                   'blocks': [[0, 1, 0, 0]],
                   'instructions': [[30, 0, 0, 0, 0, 0, 0, 0, 0]],
                   'operands': []}],
    'declarations': None,
    'generics': [[None, [[2, []]], []]],
    'nodes': [], 'nominals': [], 'interfaces': [], 'defaults': [],
}


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    src = args.source_root / 'tests/unit/xir'
    output = args.output_root / 'tests/unit/xir/xir_error_marker73_golden.h'
    model = load(src / 'derive_callable71_checked_vectors.py', 'error_definition_fields')
    fields = load(src / 'derive_construction72_vectors.py', 'error_definition_framing')
    body71, callables = model.encode_body(MODEL, 0)
    assert not callables and body71[-8:] == bytes(8)
    previous71 = fields.frame(body71, 26, 71)
    assert len(previous71) == 205
    assert struct.unpack_from('<II', previous71, 169) == (0, 2)
    body = body71[:-8] + struct.pack('<I', 0) + body71[-8:]
    previous72 = fields.frame(body, 27, 72)
    current73 = fields.frame(body, 28, 73)
    assert len(current73) == len(previous72) == 209
    assert current73[64:] == previous72[64:]
    assert struct.unpack_from('<II', current73, 169) == (0, 2)
    assert struct.unpack_from('<III', current73, 197) == (0, 0, 0)
    for packet in (previous71, previous72, current73):
        assert struct.unpack_from('<Q', packet, 24)[0] == len(packet) - 64
        assert hashlib.sha256(packet[:32] + packet[64:]).digest() == packet[32:64]
    fields.header(output, 'XIR_ERROR_MARKER73_GOLDEN_H', [
        ('error_marker73_golden', current73),
        ('error_marker72_previous', previous72),
    ], args.write, [
        ('ERROR_MARKER73_KINDS_PRESENT', 169),
        ('ERROR_MARKER73_MARKER', 173),
    ])
    print(json.dumps({'status': 'INDEPENDENT_COMPLETE_ERROR_DEFINITION_MODEL',
                      'previous71_bytes': len(previous71),
                      'current73_bytes': len(current73),
                      'previous72_sha256': hashlib.sha256(previous72).hexdigest(),
                      'current73_sha256': hashlib.sha256(current73).hexdigest(),
                      'body_sha256': hashlib.sha256(body).hexdigest(),
                      'header_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
                      'writer_or_executable_used': False}, indent=2))


if __name__ == '__main__':
    main()
