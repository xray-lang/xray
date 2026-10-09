"""Encode named construction facts and recursive origins without a product writer."""
from pathlib import Path
import argparse
import copy
import hashlib
import importlib.util
import json
import struct
import sys

sys.dont_write_bytecode = True

def instruction(op, result=0, args=(0, 0), immediate=0):
    return [op, result, *args, 0, 0, immediate, 0, 0]

def function(name, result, instructions, operands=()):
    return dict(name=name, parameters=[], result=result,
                blocks=[[0, len(instructions), 0, 0]],
                instructions=instructions, operands=list(operands))

def model():
    # Stable opcode identities: CONST_INT2, ADD_INT25, CALL28, RETURN33,
    # STRUCT_NEW82 and STRUCT_GET83, each guarded independently by the C fixture.
    functions = [
        function('init', 0, [instruction(33)]),
        function('entry', 2, [instruction(28, 256, immediate=2),
                             instruction(83, 2), instruction(83, 2, immediate=1),
                             instruction(25, 2, (1, 2)), instruction(33, args=(3, 0))]),
        function('make', 256, [instruction(28, 2, immediate=3),
                              instruction(28, 2, immediate=4),
                              instruction(82, 256, (0, 2)), instruction(33, args=(2, 0))], [0, 1]),
        function('a', 2, [instruction(2, 2, immediate=19), instruction(33)]),
        function('b', 2, [instruction(2, 2, immediate=23), instruction(33)])]
    identities = [[0, 0, 0, 0, 0, 0, 0, 0, 0],
                  [0, 1, 0, 0, 0, 0, 0, 0, 0],
                  [0, 1, 1, 0, 0, 0, 3, 0, 0],
                  [0, 1, 1, 0, 0, 0, 4, 0, 0],
                  [0, 1, 1, 0, 0, 0, 4, 0, 0]]
    return dict(linkage=0, functions=functions,
                declarations=dict(modules=[['alpha', [], 0]], identities=identities,
                                  slots=[], literals=[], root=0, entry=1),
                generics=[[None, [], []] for _ in functions],
                nodes=[[4, 0, 0, [], []]],
                nominals=[['alpha', 'Pair', 1, 0, 0, 0, '00' * 32, [],
                           [['a', 2, 0], ['b', 2, 0]], []]],
                interfaces=[], defaults=[])

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    source = args.source_root / 'tests/unit/xir/derive_callable71_checked_vectors.py'
    spec = importlib.util.spec_from_file_location('root_explicit_fields', source)
    encoder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(encoder)
    words = lambda *values: struct.pack('<' + 'I' * len(values), *values)
    original = model()
    body, _ = encoder.encode_body(original, 0)
    assert body[-8:] == bytes(8)
    owner = words(1, 3, 2, 4, 5)  # one nominal; make+1, two fields, a+1,b+1.
    checked_body = body[:-8] + owner + body[-8:]
    checked_offset = 64 + len(body) - 8
    instance = copy.deepcopy(original)
    instance['generics'] = None
    instance['nodes'][0][4] = [2, 2]
    projected, _ = encoder.encode_body(instance, 0)
    assert projected[-8:] == bytes(8)
    zeros = words(1, 0, 2, 0, 0)
    prefix = projected[:-8] + zeros + words(0, 2)
    recursive_offset = 64 + len(prefix) + len(body) - 8
    origins = words(5) + b''.join(words(f, 0, 0) for f in range(5)) + words(0)
    instance_body = prefix + checked_body + origins
    def frame(payload, schema=27, semantic=72):
        header = b'XRCHK\0\0\0' + words(schema, semantic, 2, 0) + struct.pack('<Q', len(payload))
        return header + hashlib.sha256(header + payload).digest() + payload
    rows = [('root_construction72_checked', frame(checked_body)),
            ('root_construction72_instance', frame(instance_body)),
            ('root_construction71_previous', frame(body, 26, 71))]
    lines = ['/*',
             ' * xray - Lightweight typed scripting with native concurrency',
             ' * https://www.xray-lang.org',
             ' *',
             ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>',
             ' * Licensed under the MIT License',
             ' *',
             ' * root_construction72_goldens.h - Independent construction and origin vectors',
             ' *',
             ' * KEY CONCEPT:',
             ' *   Explicit named fields define all bytes without invoking a product writer.',
             ' */',
             '#ifndef ROOT_CONSTRUCTION72_GOLDENS_H', '#define ROOT_CONSTRUCTION72_GOLDENS_H',
             '#include <stdint.h>']
    for name, data in rows:
        lines.append('static const uint8_t ' + name + '[] = {')
        lines.extend('    ' + ', '.join('0x%02x' % b for b in data[i:i+12]) + ','
                     for i in range(0, len(data), 12))
        lines.append('};')
    lines += ['#define ROOT_CONSTRUCTION72_CHECKED_OWNER %du' % checked_offset,
              '#define ROOT_CONSTRUCTION72_RECURSIVE_OWNER %du' % recursive_offset,
              '#endif // ROOT_CONSTRUCTION72_GOLDENS_H']
    text = '\n'.join(lines) + '\n'
    output = args.output_root / 'tests/unit/xir/root_construction72_goldens.h'
    if args.write:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding='utf-8', newline='\n')
    else:
        assert output.read_text(encoding='utf-8') == text
    print(json.dumps(dict(status='INDEPENDENT_FIELDS_ONLY_NOT_C_RUN',
                         encoder_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                         model=original, checked_owner_offset=checked_offset,
                         recursive_owner_offset=recursive_offset,
                         vectors=[dict(name=n, bytes=len(d), sha256=hashlib.sha256(d).hexdigest())
                                  for n, d in rows]), indent=2))

if __name__ == '__main__':
    main()
