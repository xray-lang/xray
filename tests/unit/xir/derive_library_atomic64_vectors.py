"""Independent Library/default packet projection; no writer output or ABI probe."""
from pathlib import Path
import argparse
import hashlib
import re
import struct


def arrays(path):
    text = path.read_text(encoding='utf-8')
    return {name: bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', body))
            for name, body in re.findall(r'static const uint8_t (\w+)\[\]\s*=\s*\{(.*?)\};', text, re.S)}


def current_packet(previous, semantic=64):
    assert semantic in (64,65)
    assert previous[:8] == b'XRCHK\0\0\0'
    assert struct.unpack_from('<4I', previous, 8) == (24, 63, 2, 0)
    assert struct.unpack_from('<Q', previous, 24)[0] == len(previous) - 64
    assert hashlib.sha256(previous[:32] + previous[64:]).digest() == previous[32:64]
    output, changed, at = bytearray(previous), set(), 64

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

    def constraints(count):
        for _ in range(count):
            word()
            for _ in range(word()):
                application()

    linkage, functions, declarations = word(), word(), word()
    assert linkage in (0, 1) and functions in (1, 2, 3, 4) and declarations in (0, 1)
    assert declarations or (linkage == 0 and functions == 1)
    for _ in range(functions):
        skip(word())
        skip(4 * word())
        word()
        skip(16 * word())
        for _ in range(word()):
            opcode = word()
            assert 1 <= opcode <= 136 and opcode not in (7, 8, 9)
            if opcode >= 10:
                struct.pack_into('<I', output, at - 4, opcode + 8)
                changed.update(range(at - 4, at))
            skip(36)
        skip(4 * word())
    if declarations:
        modules, slots, literals, root, entry = word(), word(), word(), word(), word()
        assert modules == 1 and slots == 0
        assert (linkage == 1 and root == entry == 0xffffffff) or (linkage == 0 and root == 0 and entry < functions)
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
            constraints(parameters)
            skip(4 * word())
    # These exact independent fixtures contain no nominal metadata. The
    # generic fixture does include its two original interface declarations.
    # NativeRecord36 therefore appends zero bytes. Do not generalize this to
    # any nominal packet or infer a new nominal current packet from this helper.
    type_counts = word(), word(), word()
    types, nominals, interfaces = type_counts
    assert types in (0, 1) and nominals == 0 and interfaces in (0, 2), (type_counts, at)
    for _ in range(types):
        kind, span, parameters = word(), word(), word()
        assert kind == 1 and span == 2 and parameters == 1
        skip(8 * parameters)
        word()
        word()
    for _ in range(interfaces):
        skip(word())
        skip(word())
        word()
        constraints(word())
        for _ in range(word()):
            application()
        for _ in range(word()):
            skip(word())
            word()
            word()
            constraints(word())
    skip(16 * word())
    assert word() == 0 and at == len(previous)
    assert all(output[i] == previous[i] for i in range(64, len(previous)) if i not in changed)
    struct.pack_into('<II', output, 8, 25, semantic)
    output[32:64] = hashlib.sha256(output[:32] + output[64:]).digest()
    return bytes(output)


def header(guard, facts):
    lines = ['/* Independent current packets; historical headers remain unchanged. */',
             '#ifndef ' + guard, '#define ' + guard]
    for name, packet in facts.items():
        lines.append('static const uint8_t ' + name + '[] = {')
        for at in range(0, len(packet), 12):
            lines.append('    ' + ','.join('0x%02x' % b for b in packet[at:at + 12]) + ',')
        lines.append('};')
    return '\n'.join(lines + ['#endif', ''])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).parent
    source = arrays(root / 'xir_library_string_goldens.h')
    assert len(source) == 7
    strings = {name.replace('library_string_', 'library_string64_'): current_packet(packet)
               for name, packet in source.items()}
    old_defaults = arrays(root / 'xir_defaults_golden_bytes.h')
    defaults = {'defaults64_golden_' + str(i): current_packet(old_defaults['defaults_golden_' + str(i)])
                for i in range(7, 11)}
    invoke = arrays(root / 'xir_defaults_invoke_golden.h')['defaults_invoke_golden']
    defaults['defaults64_invoke_golden'] = current_packet(invoke)
    for filename, guard, facts in [
            ('xir_library_string64_goldens.h', 'XIR_LIBRARY_STRING64_GOLDENS_H', strings),
            ('xir_defaults64_golden_bytes.h', 'XIR_DEFAULTS64_GOLDEN_BYTES_H', defaults)]:
        expected = header(guard, facts)
        if args.write:
            (root / filename).write_bytes(expected.encode('utf-8'))
        else:
            assert (root / filename).read_text(encoding='utf-8') == expected
    current65_facts = {
        'xir_library_string65_goldens.h': {name.replace('library_string_', 'library_string65_'): current_packet(packet,65) for name,packet in source.items()},
        'xir_defaults65_golden_bytes.h': {'defaults65_golden_'+str(i): current_packet(old_defaults['defaults_golden_'+str(i)],65) for i in range(7,11)}}
    current65_facts['xir_defaults65_golden_bytes.h']['defaults65_invoke_golden']=current_packet(invoke,65)
    for filename,facts in current65_facts.items():
        assert arrays(root/filename)==facts
    print('Independent current25/65 Library7/default4/invoke1 full packets; complete previous25/64 and old24/63 verified unchanged PASS')


if __name__ == '__main__':
    main()
