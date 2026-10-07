"""Independently encode the complete seven-function Checked Core packet.

The declaration, CFG, binder, default-owner, and identity facts are fixed here.
No compiler executable, emitted packet, or packet writer is used as an oracle.
Complete prior packets remain independently reproduced rejection inputs.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct


OPCODES = {
    'CONST_BOOL': 1,
    'CONST_STRING': 3,
    'THROW': 30,
    'JUMP': 31,
    'RETURN': 33,
    'INVOKE_INDIRECT': 100,
    'INVOKE_ERROR': 102,
    'PANIC_CATCH': 105,
    'ASSERT_CONDITION': 123,
    'INVOKE_DISCARD': 124,
    'EQUAL': 125,
}
TYPES = {'UNIT': 0, 'BOOL': 1, 'STRING': 3, 'ERROR': 14,
         'ACTION': 256, 'TYPE_PARAMETER0': 65536}
BODY_BYTES = 1911
BODY_SHA256 = 'e0f6170c7694dbcebbe57582125875c4038c293ef371bea783e9c24385e85a19'
PREVIOUS65_SHA256 = '1c6935c54065e61af2867a8eeb0c3e684fc5f5a65874670e9b8cb3cb12c3d934'
SIGNATURES = (
    'export fn assert(cond: bool, msg: string = "") {}',
    'export fn assertPanics(action: fn() -> R, msg: string = "") {}',
    'export fn assertEqual<T: Equal>(a: T, b: T, msg: string = "") {}',
)


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def instruction(opcodes, name, result='UNIT', args=(0, 0), targets=(0, 0), immediate=0):
    return (words(opcodes[name], TYPES[result], *args, *targets)
            + struct.pack('<q', immediate) + words(0, 0))


def core_body(opcodes):
    ins = lambda name, **fields: instruction(opcodes, name, **fields)
    panic_blocks = [(0, 1, 0, 0), (1, 1, 4, 0), (2, 4, 0, 0),
                    (6, 2, 0, 0), (8, 2, 0, 0)]
    panic_recipe = [
        ins('JUMP', targets=(1, 0)),
        ins('INVOKE_INDIRECT', result='TYPE_PARAMETER0', targets=(2, 3)),
        ins('INVOKE_DISCARD', immediate=1),
        ins('CONST_BOOL', result='BOOL'),
        ins('ASSERT_CONDITION', args=(5, 1)),
        ins('RETURN'),
        ins('INVOKE_ERROR', result='ERROR', immediate=1),
        ins('THROW', args=(8, 0)),
        ins('PANIC_CATCH'),
        ins('RETURN'),
    ]
    # Stable declaration order, including independently owned parameter defaults.
    functions = [
        ('$init', (), 'UNIT', [(0, 1, 0, 0)], [ins('RETURN')]),
        ('assert', ('BOOL', 'STRING'), 'UNIT', [(0, 2, 0, 0)],
         [ins('ASSERT_CONDITION', args=(0, 1)), ins('RETURN')]),
        ('assertPanics', ('ACTION', 'STRING'), 'UNIT', panic_blocks, panic_recipe),
        ('assertEqual', ('TYPE_PARAMETER0', 'TYPE_PARAMETER0', 'STRING'), 'UNIT',
         [(0, 3, 0, 0)], [ins('EQUAL', result='BOOL', args=(0, 1)),
                           ins('ASSERT_CONDITION', args=(3, 2)), ins('RETURN')]),
    ]
    for literal in range(3):
        functions.append(('$argument_default', (), 'STRING', [(0, 2, 0, 0)],
                          [ins('CONST_STRING', result='STRING', immediate=literal), ins('RETURN')]))
    # Library linkage, seven functions, and a present declaration table.
    body = bytearray(words(1, len(functions), 1))
    offsets = {}
    for index, (name, parameters, result, blocks, recipe) in enumerate(functions):
        offsets[f'function{index}'] = 64 + len(body)
        encoded_name = name.encode('utf-8')
        body += words(len(encoded_name)) + encoded_name
        body += words(len(parameters)) + words(*(TYPES[p] for p in parameters))
        body += words(TYPES[result], len(blocks))
        for block in blocks:
            body += words(*block)
        body += words(len(recipe))
        offsets[f'op{index}'] = 64 + len(body)
        body += b''.join(recipe) + words(0)  # No variadic operand buffer.
    # One memory module, no state, three empty literals, and no root/entry.
    identity = b'memory-module-v1:id=23:xray-core-assertions-v1'
    body += words(1, 0, 3, 0xffffffff, 0xffffffff)
    body += words(len(identity)) + identity + words(0, 0)  # No dependencies; initializer zero.
    for index, exported in enumerate((0, 1, 1, 1, 0, 0, 0)):
        offsets[f'identity{index}'] = 64 + len(body)
        # Module/export/nominal/access/cleanup/promises/method/test-role/timeout.
        body += words(0, exported, 0, 0, 0, 0, 0, 0, 0)
    body += words(0, 0, 0, 0)  # Empty literal lengths; zero implementation records.
    body += words(1)  # A present generic declaration table.
    for index in range(7):
        offsets[f'generic{index}'] = 64 + len(body)
        if index in (2, 5):
            # One result-variable binder; no markers, interfaces, or arguments.
            body += words(1, 1, 1, 0, 0, 0)
        elif index in (3, 6):
            # One ordinary type binder constrained by Equal (marker bit four).
            body += words(1, 0, 4, 0, 0)
        else:
            body += words(0, 0, 0)
    # One callable node, no nominals/interfaces: fn() -> R, binder span one, no effects.
    body += words(1, 0, 0, 1, 1, 0, TYPES['TYPE_PARAMETER0'], 0)
    offsets['defaults'] = 64 + len(body)
    body += words(3)
    for owner, ordinal, helper in ((1, 1, 4), (2, 1, 5), (3, 2, 6)):
        body += words(0, owner, ordinal, helper)  # Parameter default owner.
    body += words(0)  # No specialization provenance in the Checked definition.
    return bytes(body), offsets


def frame(body, wire, semantic):
    header = b'XRCHK\0\0\0' + words(wire, semantic, 2, 0) + struct.pack('<Q', len(body))
    return header + hashlib.sha256(header + body).digest() + body


def literal(directory, filename, symbol):
    text = (directory / filename).read_text(encoding='utf-8')
    match = re.search(r'const\s+uint8_t\s+' + re.escape(symbol)
                      + r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    assert match is not None, filename
    return bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})\b', match[1]))


def header_text(packet):
    text = ('/* Complete independently framed seven-function wire25 semantic69 Core. */\n'
            '#ifndef XIR_ASSERT_PANICS69_GOLDEN_H\n'
            '#define XIR_ASSERT_PANICS69_GOLDEN_H\n'
            'static const uint8_t assert_panics69_golden[] = {\n')
    text += '\n'.join('    ' + ','.join(f'0x{b:02x}' for b in packet[at:at + 12]) + ','
                      for at in range(0, len(packet), 12))
    return text + '\n};\n#endif // XIR_ASSERT_PANICS69_GOLDEN_H\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--output-directory', type=Path)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = args.source_root.resolve() if args.source_root else directory.parents[2]
    prior_directory = root / 'tests/unit/xir'
    output = args.output_directory.resolve() if args.output_directory else directory
    names = re.findall(r'^XR_XIR_OP\((\w+)', (root / 'src/xir/xxir_ops.def').read_text(encoding='utf-8'), re.M)
    for name, ordinal in OPCODES.items():
        assert names[ordinal - 1] == name, (name, ordinal)
    checked_header = (root / 'src/xir/xxir_checked.h').read_text(encoding='utf-8')
    assert re.search(r'#define\s+XR_XIR_CHECKED_SCHEMA\s+25u\b', checked_header)
    assert re.search(r'#define\s+XR_XIR_CHECKED_CONTRACT\s+69u\b', checked_header)
    core_source = (root / 'src/xir/xcore_declarations.xr').read_text(encoding='utf-8')
    declarations = tuple(line.strip() for line in core_source.splitlines()
                         if line.strip() and not line.strip().startswith('//'))
    assert declarations == SIGNATURES
    body, offsets = core_body(OPCODES)
    assert len(body) == BODY_BYTES and hashlib.sha256(body).hexdigest() == BODY_SHA256
    # Historical schema24 removed no body fields used by this closed Core family.
    previous_opcodes = {name: value if value < 10 else value - 8 for name, value in OPCODES.items()}
    previous_body, previous_offsets = core_body(previous_opcodes)
    assert previous_offsets == offsets
    previous63 = frame(previous_body, 24, 63)
    previous64, previous65 = frame(body, 25, 64), frame(body, 25, 65)
    assert literal(prior_directory, 'xir_assert_panics_golden.h', 'assert_panics_golden') == previous63
    assert literal(prior_directory, 'xir_assert_panics64_golden.h', 'assert_panics64_golden') == previous64
    assert literal(prior_directory, 'xir_assert_panics65_golden.h', 'assert_panics65_golden') == previous65
    assert hashlib.sha256(previous65).hexdigest() == PREVIOUS65_SHA256
    previous_text = (prior_directory / 'xir_assert_panics_golden.h').read_text(encoding='utf-8')
    prior_offsets = {name.lower(): int(value) for name, value in
                     re.findall(r'#define XR_PANICS_VECTOR_(\w+) (\d+)u', previous_text)}
    assert prior_offsets == offsets
    current = frame(body, 25, 69)
    assert current[64:] == previous65[64:] == previous64[64:]
    header = output / 'xir_assert_panics69_golden.h'
    if args.write:
        output.mkdir(parents=True, exist_ok=True)
        header.write_text(header_text(current), encoding='utf-8', newline='\n')
    else:
        assert literal(output, header.name, 'assert_panics69_golden') == current
    print(json.dumps({'wire': 25, 'semantic': 69, 'packet_bytes': len(current),
                      'body_bytes': len(body), 'body_sha256': BODY_SHA256,
                      'packet_sha256': hashlib.sha256(current).hexdigest(),
                      'previous63_sha256': hashlib.sha256(previous63).hexdigest(),
                      'previous64_sha256': hashlib.sha256(previous64).hexdigest(),
                      'previous65_sha256': PREVIOUS65_SHA256,
                      'complete_prior_vectors_reproduced': True,
                      'current_body_equal_to_complete65': True,
                      'actual_compiler_packet_used_as_oracle': False,
                      'opcodes': OPCODES, 'offsets': offsets}, indent=2))


if __name__ == '__main__':
    main()
