"""Independent current assertion wire roles; no compiler dump or ABI probe."""
import hashlib
import struct


def current64_packet(previous):
    assert previous[:8] == b'XRCHK\0\0\0' and len(previous) >= 64
    assert struct.unpack_from('<4I', previous, 8) == (24, 63, 2, 0)
    assert struct.unpack_from('<Q', previous, 24)[0] == len(previous) - 64
    assert hashlib.sha256(previous[:32] + previous[64:]).digest() == previous[32:64]
    output = bytearray(previous)
    at = 64
    changed = set()

    def word():
        nonlocal at
        assert at + 4 <= len(previous)
        value = struct.unpack_from('<I', previous, at)[0]
        at += 4
        return value

    def skip(size):
        nonlocal at
        assert 0 <= size <= len(previous) - at
        at += size

    def application():
        word()
        skip(4 * word())

    linkage, functions, declarations = word(), word(), word()
    assert linkage == 1 and functions in (3, 7) and declarations == 1
    # Current Atomic replaces the three fixed operations with eleven operations.
    # This fixture has none of the retired cell operations. Every later opcode
    # keeps its semantic name and moves by eight; all non-opcode bytes stay fixed.
    for _ in range(functions):
        skip(word())
        skip(4 * word())
        word()
        skip(16 * word())
        for _ in range(word()):
            code = word()
            position = at - 4
            assert 1 <= code <= 136 and code not in (7, 8, 9)
            if code >= 10:
                struct.pack_into('<I', output, position, code + 8)
                changed.update(range(position, position + 4))
            skip(36)
        skip(4 * word())
    modules, slots, literals, root, entry = word(), word(), word(), word(), word()
    assert modules == 1 and slots == 0 and root == entry == 0xffffffff
    for _ in range(modules):
        skip(word())
        skip(4 * word())
        word()
    skip(36 * functions + 12 * slots)
    for _ in range(literals):
        skip(word())
    for _ in range(word()):
        word()
        application()
        for _ in range(word()):
            application()
            word()
            word()
    if word():
        for _ in range(functions):
            parameters, kinds = word(), word()
            if kinds:
                skip(4 * parameters)
            for _ in range(parameters):
                word()
                for _ in range(word()):
                    application()
            skip(4 * word())
    type_count = word()
    assert type_count in (0, 1)
    nominal_count, interface_count = word(), word()
    assert nominal_count == interface_count == 0
    for _ in range(type_count):
        kind, span, parameters = word(), word(), word()
        assert kind == 1 and span == 1 and parameters == 0
        skip(8 * parameters)
        word()  # Callable result.
        word()  # Primitive kind flags.
    # No nominal NativeRecord36 exists in these exact fixture bodies.
    # Defaults and unspecialized provenance preserve their original roles.
    skip(16 * word())
    assert word() == 0 and at == len(previous)
    assert all(output[i] == previous[i] for i in range(64, len(previous)) if i not in changed)
    struct.pack_into('<II', output, 8, 25, 64)
    output[32:64] = hashlib.sha256(output[:32] + output[64:]).digest()
    return bytes(output)
