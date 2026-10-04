"""Frame the closed core declaration from static facts, without a compiler dump."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def op(code, result=0, left=0, right=0, normal=0, error=0, immediate=0):
    return words(code, result, left, right, normal, error) + struct.pack('<q', immediate) + words(0, 0)


def function(name, parameters, result, blocks, instructions):
    name = name.encode('ascii')
    return (words(len(name)) + name + words(len(parameters)) + words(*parameters) +
            words(result, len(blocks)) + b''.join(words(*block) for block in blocks) +
            words(len(instructions)) + b''.join(instructions) + words(0))


def packet(body, wire, semantic):
    head = b'XRCHK\0\0\0' + words(wire, semantic, 2, 0) + struct.pack('<Q', len(body))
    return head + hashlib.sha256(head + body).digest() + body


def vector():
    # Unit0/Bool1/String3/Error14, local fn()->R256, result variable65536.
    # The declaration has two distinct empty STRING default literals.
    blocks = [(0, 1, 0, 0), (1, 1, 4, 0), (2, 4, 0, 0), (6, 2, 0, 0), (8, 2, 0, 0)]
    panic_ops = [op(23, normal=1), op(92, 65536, normal=2, error=3),
                 op(116, immediate=1), op(1, 1), op(115, left=5, right=1), op(25),
                 op(94, 14, immediate=1), op(22, left=8), op(97), op(25)]
    functions = [function('$init', [], 0, [(0, 1, 0, 0)], [op(25)]),
                 function('assert', [1, 3], 0, [(0, 2, 0, 0)], [op(115, right=1), op(25)]),
                 function('assertPanics', [256, 3], 0, blocks, panic_ops),
                 function('$argument_default', [], 3, [(0, 2, 0, 0)], [op(3, 3), op(25)]),
                 function('$argument_default', [], 3, [(0, 2, 0, 0)], [op(3, 3, immediate=1), op(25)])]
    identity = b'memory-module-v1:id=23:xray-core-assertions-v1'
    body = words(1, 5, 1) + b''.join(functions)
    body += words(1, 0, 2, 0xffffffff, 0xffffffff) + words(len(identity)) + identity + words(0, 0)
    for exported in (0, 1, 1, 0, 0):
        body += words(0, exported, 0, 0, 0, 0, 0)
    body += words(0, 0, 0)  # Empty literals0/1 and no implementation records.
    body += words(1)  # Generic table exists, including kind presence for every function.
    offsets = {}
    for index in range(5):
        offsets[f'generic{index}'] = 64 + len(body)
        body += words(1, 1, 1, 0, 0, 0) if index in (2, 4) else words(0, 0, 0)
    body += words(1, 0, 0, 1, 1, 0, 65536, 0)  # One callable, zero nominals/interfaces.
    offsets['defaults'] = 64 + len(body)
    body += words(2, 0, 1, 1, 3, 0, 2, 1, 4) + words(0)
    return packet(body, 22, 56), offsets


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).parent
    current, offsets = vector()
    data = {'wire': 22, 'semantic': 56, 'core_packet_hex': current.hex(),
            'core_body_sha256': hashlib.sha256(current[64:]).hexdigest(),
            'core_packet_sha256': hashlib.sha256(current).hexdigest(), 'offsets': offsets}
    from derive_assert_equal_vector import vector as current_vector
    from derive_semantic61_migration import semantic61_packet
    historical59, current_offsets = current_vector()
    executable = semantic61_packet(historical59)
    header = ('/* Complete independently framed wire23 semantic61 core declaration. */\n'
              'static const uint8_t assert_panics_golden[] = {\n' +
              '\n'.join('    ' + ','.join(f'0x{byte:02x}' for byte in executable[at:at+12]) + ','
                        for at in range(0, len(executable), 12)) + '\n};\n')
    header += '\n'.join(f'#define XR_PANICS_VECTOR_{name.upper()} {value}u'
                        for name,value in current_offsets.items())+'\n'
    if args.write:
        (directory / 'xir_assert_panics_golden.h').write_text(header, encoding='utf-8')
        (directory / 'assert_panics_packet_vectors.json').write_text(json.dumps(data, indent=2)+'\n', encoding='utf-8')
    else:
        literal = bytes(int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]{2}',
                        (directory / 'xir_assert_panics_golden.h').read_text(encoding='utf-8')))
        assert literal == executable
        assert json.loads((directory / 'assert_panics_packet_vectors.json').read_text(encoding='utf-8')) == data
    print(json.dumps({'historical56_bytes': len(current), 'historical56_sha256': data['core_packet_sha256'],
                     'current61_bytes':len(executable), 'current61_sha256':hashlib.sha256(executable).hexdigest(),
                     'historical56_offsets': offsets, 'current61_offsets':current_offsets}, indent=2))


if __name__ == '__main__':
    main()
