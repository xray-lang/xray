"""Rebuild local current fields while preserving complete retired wire inputs."""
from pathlib import Path
import argparse
import copy
import hashlib
import importlib.util
import re
import struct
from checked_scope_wire import receive_scopes, receive_offset

HERE = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def arrays(path):
    return {match[1]: bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', match[2]))
            for match in re.finditer(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',
                                    path.read_text(encoding='utf-8'), re.S)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    fields = load('derive_construction72_vectors')
    encoder = load('derive_callable71_checked_vectors')
    previous = load('derive_root_construction75_vectors')
    model = previous.model()
    words = lambda *values: struct.pack('<' + 'I' * len(values), *values)
    body, _ = encoder.encode_body(model, 0)
    assert body[-8:] == bytes(8)
    facts = model['template_facts']
    assert [row['constant_mask'] for row in facts] == [4, 0, 0, 0, 0]
    template = words(1, 5) + b''.join(words(0, row['constant_mask'], 0, 0, 0) for row in facts)
    checked = body[:-8] + words(1, 3, 2, 4, 5) + words(0) + template
    instance = copy.deepcopy(model)
    instance['generics'] = None
    instance['nodes'][0][4] = [2, 2]
    projected, _ = encoder.encode_body(instance, 0)
    assert projected[-8:] == bytes(8)
    origins = words(5) + b''.join(words(index, 0, 0) for index in range(5)) + words(0)
    recursive = projected[:-8] + words(1, 0, 2, 0, 0) + words(0, 2) + checked + origins
    old = arrays(HERE / 'root_construction75_goldens.h')
    rows, positions = [], {}
    for name, payload in [('checked', checked), ('instance', recursive)]:
        assert fields.frame(payload, 28, 75) == old['root_construction75_' + name]
        current, offsets = receive_scopes(payload)
        positions[name] = offsets
        rows.append(('root_construction76_' + name, fields.frame(current, 29, 76)))
    rows.append(('root_construction71_previous_current76', old['root_construction71_previous_current75']))
    text = (HERE / 'root_construction75_goldens.h').read_text(encoding='utf-8')
    macros = []
    for name, value in re.findall(r'#define (ROOT_CONSTRUCTION75_\w+) (\d+)u', text):
        offsets = positions['instance' if 'RECURSIVE' in name else 'checked']
        macros.append((name.replace('75', '76'), receive_offset(int(value), offsets)))
    assert len(macros) == 2
    fields.header(HERE / 'root_construction76_goldens.h', 'ROOT_CONSTRUCTION76_GOLDENS_H',
                  rows, args.write, macros)
    identities = load('derive_source_product_identity')
    current = identities.frames(25, 76, 29, 31)
    rows = [item for mapping in current.values() for item in mapping.items()]
    fields.header(HERE / 'xir_hash76_identity.h', 'XIR_HASH76_IDENTITY_H', rows, args.write)
    print('Independent root field migration28/75 to29/76 and Value25 current identity: PASS')


if __name__ == '__main__':
    main()
