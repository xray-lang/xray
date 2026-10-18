"""Verify complete immutable input packets and derive the composed field peers."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import sys

sys.dont_write_bytecode = True
from model import decode, encode, compose, PROFILES

ROOT = Path(__file__).resolve().parent
ARRAY = re.compile(r'(?:static\s+)?const\s+uint8_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', re.S)


def original(row):
    raw = (ROOT / 'inputs' / (row['blob'] + '.h')).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == row['header_sha256']
    assert hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest() == row['blob']
    matches = [match for match in ARRAY.finditer(raw.decode('utf-8')) if match[1] == row['symbol']]
    assert len(matches) == 1, (row['path'], row['symbol'])
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', matches[0][2], flags=re.S)
    data = bytes(int(value, 0) for value in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', text))
    assert len(data) == row['bytes']
    assert hashlib.sha256(data).hexdigest() == row['sha256']
    return data


def array(name, data):
    return ('static const uint8_t ' + name + '[] = {\n' + ''.join(
        '    ' + ','.join('0x%02x' % byte for byte in data[at:at + 24]) + ',\n'
        for at in range(0, len(data), 24)) + '};\n')


def output(path, text, write):
    if write:
        path.write_bytes(text.encode('utf-8'))
    else:
        assert path.read_bytes() == text.encode('utf-8'), path


def build(write=False):
    rows = json.loads((ROOT / 'inputs.json').read_text('utf-8'))
    assert len(rows) == 300
    closed = {'P08:generator89_reserved_map': 9,
              'P08:generator89_reserved_set': 10,
              'P08:iterator_types_unknown': 0xffffffff}
    report, table, includes = [], [], []
    for i, row in enumerate(rows):
        assert i == row['index']
        old = original(row)
        peer, offsets = None, {}
        if row['closed_type_control']:
            identity = row['label'] + ':' + row['symbol']
            assert identity in closed
            try:
                decode(old, row['label'])
            except AssertionError as error:
                assert error.args[0][:3] == ('closed type kind', 'module.nodes[0]', closed[identity]), str(error)
            else:
                raise AssertionError(('Undefined historical kind was interpreted', identity))
            status = 'EXPECTED_UNDEFINED_HISTORICAL_TYPE'
        else:
            value, original_offsets = decode(old, row['label'])
            exact, _ = encode(value, row['label'])
            assert exact == old, (row['label'], row['symbol'], 'original round trip')
            peer, current_offsets = encode(compose(value, row['label']))
            current, _ = decode(peer, 'current')
            assert encode(current)[0] == peer
            offsets = {key: [offset, current_offsets[key][0], size]
                       for key, (offset, size) in original_offsets.items() if key in current_offsets}
            status = 'COMPLETE_FIELDS_ROUND_TRIP'
        name = 'packet_%03d' % i
        text = '/* Complete independent packets; the original remains byte-identical. */\n'
        text += array(name + '_original', old)
        if peer is not None:
            text += array(name + '_current', peer)
        assert len(text.splitlines()) <= 3000
        output(ROOT / (name + '.inc.c'), text, write)
        includes.append('#include "' + name + '.inc.c"\n')
        table.append('    {"' + row['label'] + ':' + row['symbol'] + '",' + name + '_original,sizeof(' + name + '_original),'
                     + (name + '_current,sizeof(' + name + '_current)' if peer is not None else 'NULL,0') + ','
                     + (row['expected_current_status'] or '-1') + '},\n')
        report.append(dict(index=i, label=row['label'], symbol=row['symbol'], status=status,
                           original_sha256=hashlib.sha256(old).hexdigest(), original_bytes=len(old),
                           current_sha256=hashlib.sha256(peer).hexdigest() if peer is not None else None,
                           current_bytes=len(peer) if peer is not None else None,
                           field_offsets=offsets, expected_current_status=row['expected_current_status'],
                           semantic_execution='NOT_RUN'))
    text = '/* All original packets are early-rejection inputs; current checks retain their explicit denominator. */\n'
    text += ''.join(includes) + 'static const ComposedWireCase composed_wire_cases[] = {\n' + ''.join(table) + '};\n'
    output(ROOT / 'catalog.inc.c', text, write)
    ordinals = '/* Independently composed names retain their exact closed instruction identities. */\n'
    for ordinal, name in enumerate(PROFILES['current'].operations, 1):
        ordinals += '_Static_assert(XR_XIR_' + name + '==' + str(ordinal) + ',"closed instruction identity");\n'
    ordinals += '_Static_assert(XR_XIR_OP_COUNT==193,"closed instruction denominator");\n'
    output(ROOT / 'ordinals.inc.c', ordinals, write)
    output(ROOT / 'model_facts.json', json.dumps(report, indent=2) + '\n', write)
    return dict(original_packets=len(rows), original_headers=len({row['blob'] for row in rows}),
                complete_field_round_trips=sum(row['status'] == 'COMPLETE_FIELDS_ROUND_TRIP' for row in report),
                expected_undefined_type_controls=3,
                current_semantic_cases=sum(row['expected_current_status'] is not None for row in rows),
                current_positive_cases=sum(row['expected_current_status'] == 'XR_XIR_OK' for row in rows),
                product_writer_used=False, product_semantic_execution='NOT_RUN')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    print(json.dumps(build(args.write), indent=2))
