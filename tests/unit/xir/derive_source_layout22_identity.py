"""Verify complete Lowered layout KATs from fixed named facts independently.

The layout format contains three ABI identities, not Checked schema/semantic.
No generated artifact, executable, writer output or measured layout is read.
The preceding complete 198-byte layout and both prior digest literals remain.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct


DOMAIN = b'xray:xir-lowered-layout:v1'
PREFIX_ORDER = ('format', 'architecture', 'value_abi', 'call_abi', 'program_abi',
                'entry', 'function_count', 'module_count')
LAYOUT_ORDER = ('function', 'parameter_count', 'slot_count', 'frame_bytes',
                'result_size', 'result_alignment', 'owned_count',
                'outgoing_count', 'path_count')
FIXED = {'format': 1, 'architecture': 1, 'entry': 2,
         'function_count': 3, 'module_count': 1}
CURRENT = {'value_abi': 22, 'call_abi': 28, 'program_abi': 29}
PREVIOUS = {'value_abi': 21, 'call_abi': 26, 'program_abi': 29}
HISTORICAL = {'value_abi': 20, 'call_abi': 25, 'program_abi': 28}
LAYOUTS = [
    {'function': 0, 'parameter_count': 1, 'slot_count': 2, 'frame_bytes': 8,
     'result_size': 16, 'result_alignment': 8, 'owned_count': 1,
     'outgoing_count': 0, 'path_count': 0, 'offsets': [0, 0xffffffff],
     'parameters': [[16, 8]], 'owned_offsets': [0]},
    {'function': 1, 'parameter_count': 0, 'slot_count': 1, 'frame_bytes': 0,
     'result_size': 16, 'result_alignment': 8, 'owned_count': 0,
     'outgoing_count': 0, 'path_count': 0, 'offsets': [0xffffffff],
     'parameters': [], 'owned_offsets': []},
    {'function': 2, 'parameter_count': 0, 'slot_count': 2, 'frame_bytes': 8,
     'result_size': 16, 'result_alignment': 8, 'owned_count': 0,
     'outgoing_count': 0, 'path_count': 0, 'offsets': [0, 0xffffffff],
     'parameters': [], 'owned_offsets': []},
]
HISTORICAL_SHA = '4e4205a7fe9b388761a34c975f9cf7f55bc8a163510baffaf700fd6b06e37a23'
PREVIOUS_SHA = '5325dff66ebc0db50f20dda2ed30b002e4b6fbbdac04c95a5889d755407a789a'


def framing(versions):
    facts = {**FIXED, **versions}
    words = [facts[key] for key in PREFIX_ORDER]
    for layout in LAYOUTS:
        assert len(layout['offsets']) == layout['slot_count']
        assert len(layout['parameters']) == layout['parameter_count']
        assert len(layout['owned_offsets']) == layout['owned_count']
        words += [layout[key] for key in LAYOUT_ORDER]
        words += layout['offsets']
        words += [value for parameter in layout['parameters'] for value in parameter]
        words += layout['owned_offsets']
    assert len(DOMAIN) == 26 and len(words) == 43
    preimage = DOMAIN + struct.pack('<43I', *words)
    assert len(preimage) == 198
    return {'facts': facts, 'layouts': LAYOUTS, 'words': words, 'length': 198,
            'preimage': preimage.hex(), 'sha256': hashlib.sha256(preimage).hexdigest()}


def literal(header, symbol):
    match = re.search(r'\b' + re.escape(symbol) + r'\[[^]]*\]\s*=\s*\{([^}]+)\}', header)
    assert match is not None, symbol
    return bytes(int(value, 16) for value in re.findall(r'0x[0-9a-fA-F]{2}', match[1]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--identity-header', type=Path)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = args.source_root.resolve() if args.source_root else directory.parents[2]
    header_path = args.identity_header or directory / 'xir_source_product_identity.h'
    header = header_path.read_text(encoding='utf-8')
    for key, filename, macro in (
        ('value_abi', 'xxir_value.h', 'XR_XIR_VALUE_ABI_VERSION'),
        ('call_abi', 'xxir_call.h', 'XR_XIR_CALL_ABI_VERSION'),
        ('program_abi', 'xxir_program.h', 'XR_XIR_PROGRAM_ABI_VERSION'),
    ):
        source = (root / 'src/xir' / filename).read_text(encoding='utf-8')
        matches = re.findall(r'^#define\s+' + macro + r'\s+(\d+)u?\s*$', source, re.M)
        assert matches == [str(CURRENT[key])], (key, matches)
    history = framing(HISTORICAL)
    previous = framing(PREVIOUS)
    current = framing(CURRENT)
    assert history['sha256'] == HISTORICAL_SHA and previous['sha256'] == PREVIOUS_SHA
    assert literal(header, 'expected').hex() == previous['sha256']
    assert literal(header, 'expected_current22').hex() == current['sha256']
    assert literal(header, 'layout_previous21_preimage').hex() == previous['preimage']
    assert literal(header, 'layout_current22_preimage').hex() == current['preimage']
    current_bytes = bytes.fromhex(current['preimage'])
    previous_bytes = bytes.fromhex(previous['preimage'])
    assert current_bytes[:34] == previous_bytes[:34]
    assert current_bytes[42:] == previous_bytes[42:]
    assert current['sha256'] != previous['sha256'] != history['sha256']
    assert [index for index, pair in enumerate(zip(previous['words'], current['words']))
            if pair[0] != pair[1]] == [2, 3]
    work = len(DOMAIN) + 1 + 43 * 8 + 1 + 32
    assert work == 404
    print(json.dumps({'status': 'STATIC_INDEPENDENT_FRAMING_PASS',
                      'layout_format_contains_schema_or_semantic': False,
                      'historical20': history, 'previous21': previous,
                      'current22': current, 'work': work,
                      'C_build_or_runtime_tests_executed': False}, indent=2))


if __name__ == '__main__':
    main()
