"""Derive complete current and distinct historical product identities from fields."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

LAYOUT_DOMAIN = b'xray:xir-lowered-layout:v1'
POLICY_DOMAIN = b'xray:xir-c11-codegen-policy:v1'
AUTHORITY = ((34, 'xxir_checked.h', 'XR_XIR_CHECKED_SCHEMA'),
             (90, 'xxir_checked.h', 'XR_XIR_CHECKED_CONTRACT'),
             (28, 'xxir_value.h', 'XR_XIR_VALUE_ABI_VERSION'),
             (35, 'xxir_call.h', 'XR_XIR_CALL_ABI_VERSION'),
             (37, 'xxir_program.h', 'XR_XIR_PROGRAM_ABI_VERSION'))


def layout_frame(value_abi, call_abi, program_abi):
    # Three exact fixture functions: String identity, Unit initializer and i64 entry.
    # Every slot offset, boxed carrier, owned offset and Unit sentinel is explicit.
    functions = [[0, 1, 2, 8, 16, 8, 1, 0, 0, 0, 0xffffffff, 16, 8, 0],
                 [1, 0, 1, 0, 16, 8, 0, 0, 0, 0xffffffff],
                 [2, 0, 2, 8, 16, 8, 0, 0, 0, 0, 0xffffffff]]
    words = [1, 1, value_abi, call_abi, program_abi, 2, 3, 1] + sum(functions, [])
    assert len(words) == 43
    frame = LAYOUT_DOMAIN + struct.pack('<43I', *words)
    assert len(frame) == 198
    return frame


def policy_frame(wire, semantic, value_abi, call_abi, program_abi, role):
    prefix = (role + '_source').encode('ascii')
    return POLICY_DOMAIN + struct.pack('<7I', 1, wire, semantic, value_abi,
                                       call_abi, program_abi, len(prefix)) + prefix


def current_frames():
    layout = layout_frame(28, 35, 37)
    arrays = {'layout_current_product_preimage': layout,
              'product_layout_current_digest': hashlib.sha256(layout).digest()}
    for role in ('linked', 'original'):
        frame = policy_frame(34, 90, 28, 35, 37, role)
        arrays[role + '_policy_current_preimage'] = frame
        arrays[role + '_policy_current'] = hashlib.sha256(frame).digest()
    return arrays


def literal(text, symbol):
    match = re.search(r'\b' + re.escape(symbol) + r'\[[^]]*\]\s*=\s*\{([^}]+)\}', text)
    assert match is not None, symbol
    return bytes(int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]{2}', match[1]))



def encode_previous(record):
    facts = record['facts']
    if record['kind'] == 'policy':
        frame = policy_frame(facts['wire'], facts['semantic'], facts['value_abi'],
                             facts['call_abi'], facts['program_abi'],
                             facts['prefix'].removesuffix('_source'))
    else:
        assert record['kind'] == 'layout'
        names = ['version', 'architecture', 'value_abi', 'call_abi', 'program_abi',
                 'entry', 'function_count', 'module_count']
        words = [facts[name] for name in names]
        assert len(facts['functions']) == facts['function_count']
        for function in facts['functions']:
            names = ['index', 'parameter_count', 'slot_count', 'frame_bytes',
                     'result_size', 'result_alignment', 'owned_count',
                     'outgoing_count', 'path_count']
            words += [function[name] for name in names]
            assert len(function['offsets']) == function['slot_count']
            assert len(function['parameters']) == function['parameter_count']
            assert len(function['owned_offsets']) == function['owned_count']
            words += function['offsets']
            words += sum(function['parameters'], [])
            words += function['owned_offsets']
        frame = LAYOUT_DOMAIN + struct.pack('<' + 'I' * len(words), *words)
    assert len(frame) == record['bytes']
    assert frame.hex() == record['frame_hex']
    assert hashlib.sha256(frame).hexdigest() == record['sha256']
    return frame


def previous_models(path):
    records = json.loads(path.read_text(encoding='utf-8'))['records']
    assert len(records) == 65
    assert len({record['sha256'] for record in records}) == 65
    assert sum(record['kind'] == 'layout' for record in records) == 11
    assert sum(record['kind'] == 'policy' for record in records) == 54
    for record in records:
        frame = encode_previous(record)
        if record['kind'] == 'layout':
            assert frame != current_frames()['layout_current_product_preimage']
        else:
            role = record['facts']['prefix'].removesuffix('_source')
            assert frame != current_frames()[role + '_policy_current_preimage']
    return records


def validate_current_header(header_path, source_root, models_path=None, previous_path=None):
    for value, filename, macro in AUTHORITY:
        text = (source_root / 'src/xir' / filename).read_text(encoding='utf-8')
        assert re.findall(r'^#define\s+' + macro + r'\s+(\d+)u?\s*$', text, re.M) == [str(value)]
    arrays = current_frames()
    text = header_path.read_text(encoding='utf-8')
    for name, data in arrays.items():
        assert literal(text, name) == data, name
    records = previous_models(models_path or header_path.parent / 'product_identity_previous_models.json')
    previous = (previous_path or header_path.parent / 'xir_previous_product_identities.inc.c').read_text(encoding='utf-8')
    for index, record in enumerate(records):
        name = f'previous_product_frame_{index:03d}'
        assert literal(previous, name + '_preimage') == encode_previous(record)
        assert literal(previous, name + '_digest').hex() == record['sha256']
    return {'current_identity': [34, 90, 28, 35, 37], 'layout_words': 43,
            'layout_bytes': 198, 'current_arrays': len(arrays),
            'previous_layout_frames': 11, 'previous_policy_frames': 54,
            'previous_full_field_frames': len(records),
            'source_input_links': sum(len(record['provenance']) for record in records),
            'current_fields': [{'name': name, 'bytes': len(data),
                                'sha256': hashlib.sha256(data).hexdigest()} for name, data in arrays.items()],
            'product_writer_or_generated_probe_used': False}


def emit_header(arrays):
    lines = ['/*',
             ' * xray - Lightweight typed scripting with native concurrency',
             ' * https://www.xray-lang.org',
             ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>',
             ' * Licensed under the MIT License',
             ' *',
             ' * xir_current_product_identity.h - Complete independent product identity fields',
             ' */', '#ifndef XIR_CURRENT_PRODUCT_IDENTITY_H',
             '#define XIR_CURRENT_PRODUCT_IDENTITY_H', '#include <stdint.h>']
    for name, data in arrays.items():
        lines.append('static const uint8_t ' + name + '[] = {')
        lines.extend('    ' + ','.join(f'0x{v:02x}' for v in data[i:i+12]) + ','
                     for i in range(0, len(data), 12))
        lines.append('};')
    lines.append('#endif // XIR_CURRENT_PRODUCT_IDENTITY_H')
    return '\n'.join(lines) + '\n'


def emit_previous(records):
    lines = ['/* Complete prior product fields retain their exact fixed-input bytes. */',
             '#ifndef XIR_PREVIOUS_PRODUCT_IDENTITIES_INC_C',
             '#define XIR_PREVIOUS_PRODUCT_IDENTITIES_INC_C',
             '#include <stdint.h>', '#include <stddef.h>']
    for index, record in enumerate(records):
        name = f'previous_product_frame_{index:03d}'
        for suffix, data in [('_preimage', encode_previous(record)),
                             ('_digest', bytes.fromhex(record['sha256']))]:
            lines.append('static const uint8_t ' + name + suffix + '[] = {')
            lines.extend('    ' + ','.join(f'0x{v:02x}' for v in data[i:i+12]) + ','
                         for i in range(0, len(data), 12))
            lines.append('};')
    lines += ['typedef struct PreviousProductIdentityFrame {',
              '    const uint8_t *preimage;', '    size_t length;',
              '    const uint8_t *digest;', '} PreviousProductIdentityFrame;']
    for kind in ('layout', 'policy'):
        lines.append('static const PreviousProductIdentityFrame previous_product_' + kind + '_frames[] = {')
        for index, record in enumerate(records):
            if record['kind'] == kind:
                name = f'previous_product_frame_{index:03d}'
                lines.append('    {' + name + '_preimage,sizeof(' + name + '_preimage),' + name + '_digest},')
        lines.append('};')
    lines.append('#endif // XIR_PREVIOUS_PRODUCT_IDENTITIES_INC_C')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--header', type=Path)
    parser.add_argument('--models', type=Path)
    parser.add_argument('--previous', type=Path)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    source_root = args.source_root or directory.parents[2]
    header_path = args.header or directory / 'xir_current_product_identity.h'
    models_path = args.models or header_path.parent / 'product_identity_previous_models.json'
    previous_path = args.previous or header_path.parent / 'xir_previous_product_identities.inc.c'
    if args.write:
        header_path.write_text(emit_header(current_frames()), encoding='utf-8')
        previous_path.write_text(emit_previous(previous_models(models_path)), encoding='utf-8')
    print(json.dumps(validate_current_header(header_path, source_root, models_path, previous_path), indent=2))


if __name__ == '__main__':
    main()
