"""Independent complete Program wire facts, without Xray writer or migration code."""
from pathlib import Path
import argparse
import hashlib
import re
import struct


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def operation(code, ty=0, left=0, right=0, immediate=0):
    return words(code, ty, left, right, 0, 0) + struct.pack('<q', immediate) + words(0, 0)


def function(name, parameters, result, instructions, operands=()):
    name = name.encode('ascii')
    return (words(len(name)) + name + words(len(parameters), *parameters, result, 1)
            + words(0, len(instructions), 0, 0, len(instructions)) + b''.join(instructions)
            + words(len(operands), *operands))


def declarations(count):
    return (words(1, 0, 0, 0, 1, 6) + b'nested' + words(0, 0)
            + b''.join(words(0, int(i != 0), 0, 0, 0, 0, 0, 0, 0) for i in range(count))
            + words(0))


def packet(body, semantic=60):
    header = b'XRCHK\0\0\0' + words(23, semantic, 2, 0) + struct.pack('<Q', len(body))
    assert len(header) == 32
    return header + hashlib.sha256(header + body).digest() + body


def nested_packet(semantic=60):
    functions = [
        function('$init', [], 0, [operation(25)]),
        function('root', [], 2, [operation(20, 257, immediate=3), operation(20, 256, right=1, immediate=5),
                 operation(126, 1, left=1), operation(2, 2, immediate=41), operation(25, left=3)], [0]),
        function('none', [], 257, [operation(118, 257), operation(25)]),
        function('some_none', [], 257, [operation(118, 256), operation(119, 257), operation(25, left=1)]),
        function('some7', [], 257, [operation(2, 2, immediate=7), operation(119, 256),
                 operation(119, 257, left=1), operation(25, left=2)]),
        function('unwrap_outer', [257], 256, [operation(127, 256), operation(25, left=1)]),
        function('unwrap_inner', [256], 2, [operation(127, 2), operation(25, left=1)]),
        function('presence', [257], 1, [operation(126, 1), operation(25, left=1)]),
    ]
    body = (words(0, 8, 1) + b''.join(functions) + declarations(8)
            + words(0, 2, 0, 0, 5, 0, 2, 5, 0, 256, 0, 0))
    data = packet(body, semantic)
    assert len(data) == 1689
    assert hashlib.sha256(data).hexdigest() == {59: '451461dd5e255f3a331b00c8379825cc46c83b3af9daccd4e6b63435e0f1d928', 60: '53b6a6a78ad1909aecf8b1a95533b6a197c35b9fddaba16793054baeaadd15ac'}[semantic]
    return data


def single_control(semantic=60):
    # A genuine complete Program with only Nullable<i64> must still pass an old reader.
    body = (words(0, 2, 1) + function('$init', [], 0, [operation(25)])
            + function('root', [], 2, [operation(118, 256), operation(2, 2, immediate=41), operation(25, left=1)])
            + declarations(2) + words(0, 1, 0, 0, 5, 0, 2, 0, 0))
    return packet(body, semantic)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    data = nested_packet()
    text = Path(__file__).with_name('xir_nested_nullable_golden.h').read_text(encoding='utf-8')
    header_bytes = bytes(int(word, 16) for word in re.findall(r'0x([0-9a-f]{2})', text))
    assert header_bytes == data
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'nested.xrc').write_bytes(data)
        (args.output / 'single.xrc').write_bytes(single_control())
    print('independent complete Program framing 1689 bytes and single-layer control PASS')


if __name__ == '__main__':
    main()
