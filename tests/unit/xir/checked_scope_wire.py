"""Project explicit zero lexical scopes from independently frozen module fields."""
import struct


def receive_scopes(body):
    at = 0

    def word():
        nonlocal at
        assert at + 4 <= len(body)
        value = struct.unpack_from('<I', body, at)[0]
        at += 4
        return value

    def skip(size):
        nonlocal at
        assert 0 <= size <= len(body) - at
        at += size

    word()  # Linkage.
    functions, declarations = word(), word()
    for _ in range(functions):
        skip(word())  # Counted function name.
        skip(word() * 4)  # Formal types.
        word()  # Result type.
        skip(word() * 16)  # Complete blocks.
        skip(word() * 40)  # Complete instructions.
        skip(word() * 4)  # Operand pool.
    positions = []
    if declarations:
        modules = word()
        for _ in range(4):
            word()  # Slot/literal counts and root/entry identities.
        for _ in range(modules):
            skip(word())
            skip(word() * 4)
            word()  # Actual initializer ordinal.
            positions.append(at)
    result = bytearray()
    previous = 0
    for position in positions:
        result += body[previous:position] + bytes(4)
        previous = position
    result += body[previous:]
    return bytes(result), [64 + p for p in positions]


def receive_offset(offset, positions):
    return offset + 4 * sum(p <= offset for p in positions)
