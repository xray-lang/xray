"""Encode complete current C11 policies from fixed named facts independently.

No projection owner, executable, generated source, or writer output is read.
All eight prior preimages and the original semantic65 literals are preserved.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct


DOMAIN = b'xray:xir-c11-codegen-policy:v1'
CURRENT = {'checked_schema': 25, 'checked_contract': 69, 'value_abi': 22,
           'call_abi': 28, 'program_abi': 29}
WORD_ORDER = ('format', 'checked_schema', 'checked_contract', 'value_abi',
              'call_abi', 'program_abi', 'prefix_length')
HISTORY = {'historical': (23, 61, 19, 25, 28),
           'current': (24, 63, 20, 25, 28),
           'atomic-current': (25, 64, 21, 26, 29),
           'tuple-group-current': (25, 65, 21, 26, 29)}
PREVIOUS65 = {'linked': 'b8ba8fb63ea3d838ab97a435519b5516f1115c913ee1517a951b57dafe005cd9',
              'original': '751fa4a1bc2f0d27f7d3e3468746f43a0cf10f94239257ef2faad77b3b5164cd'}
CURRENT69 = {'linked': '6b273a3b64d3ad414d97b6a0715b1215b6a156b61100945906f80cd9963f2d38',
             'original': '88c27a62c59ab29355582f0b55ea199ad12c975409b6da1718c9c9b29cc9a119'}


def framing(role, versions):
    prefix = (role + '_source').encode('ascii')
    names = WORD_ORDER[1:-1]
    fields = {'format': 1, **dict(zip(names, versions)), 'prefix_length': len(prefix)}
    preimage = DOMAIN + struct.pack('<7I', *(fields[name] for name in WORD_ORDER)) + prefix
    return {'role': role, 'words': fields, 'prefix_ascii': prefix.decode('ascii'),
            'length': len(preimage), 'preimage': preimage.hex(),
            'sha256': hashlib.sha256(preimage).hexdigest()}


def literal(source, symbol):
    match = re.search(r'\b' + re.escape(symbol) + r'\[32\]=\{([^}]+)\}', source)
    assert match is not None, symbol
    value = bytes(int(item, 16) for item in match[1].split(','))
    assert len(value) == 32
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--expectations', type=Path)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = args.source_root.resolve() if args.source_root else directory.parents[2]
    old_directory = root / 'tests/unit/xir'
    header = args.expectations or directory / 'xir_native_projection_expectations.h'
    source = header.read_text(encoding='utf-8')
    history_path = old_directory / 'native_projection_identity_history.json'
    history = json.loads(history_path.read_text(encoding='utf-8'))
    assert len(history['records']) == 8
    for key, header_name, macro in (
        ('checked_schema', 'xxir_checked.h', 'XR_XIR_CHECKED_SCHEMA'),
        ('checked_contract', 'xxir_checked.h', 'XR_XIR_CHECKED_CONTRACT'),
        ('value_abi', 'xxir_value.h', 'XR_XIR_VALUE_ABI_VERSION'),
        ('call_abi', 'xxir_call.h', 'XR_XIR_CALL_ABI_VERSION'),
        ('program_abi', 'xxir_program.h', 'XR_XIR_PROGRAM_ABI_VERSION'),
    ):
        text = (root / 'src/xir' / header_name).read_text(encoding='utf-8')
        matches = re.findall(r'^#define\s+' + macro + r'\s+(\d+)u?\s*$', text, re.M)
        assert matches == [str(CURRENT[key])], (key, matches)
    records = []
    for role in ('linked', 'original'):
        for identity, versions in HISTORY.items():
            expected = framing(role, versions)
            row = next(row for row in history['records']
                       if row['role'] == role and row['identity'] == identity)
            assert row['preimage'] == expected['preimage'] and row['length'] == expected['length']
            assert row['sha256'] == expected['sha256']
            assert [row[key] for key in ('wire', 'semantic', 'value_abi', 'call_abi', 'program_abi')] == list(versions)
        previous = framing(role, HISTORY['tuple-group-current'])
        current = framing(role, tuple(CURRENT[name] for name in WORD_ORDER[1:-1]))
        assert previous['sha256'] == PREVIOUS65[role]
        assert current['sha256'] == CURRENT69[role]
        assert literal(source, role + '_policy') == bytes.fromhex(previous['sha256'])
        assert literal(source, role + '_policy69') == bytes.fromhex(current['sha256'])
        assert current['sha256'] != previous['sha256']
        assert current['length'] == previous['length'] == (71 if role == 'linked' else 73)
        records.append(current)
    assert 'linked ? linked_policy69 : original_policy69,32' in source
    assert 'linked ? linked_policy : original_policy,32' in source
    print(json.dumps({'complete_fixed_current69_policies': records,
                      'historical_preimages_preserved': 8,
                      'old65_dual_literals_preserved': True,
                      'current69_distinct_from_old65': True,
                      'actual_projection_used_as_oracle': False,
                      'product_consumer_worker_paths_modified': False}, indent=2))


if __name__ == '__main__':
    main()
