"""Independently traverse complete retained fields into the unified identity.

The two source grammars are explicit. Every copied byte has an offset mapping;
new declaration facts are canonical zeroes, and only actual hash instruction
ordinals move. No product writer, compiler output or native probe is imported.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
from derive_construction72_vectors import header

ROOT = Path(__file__).resolve().parent
WORD = lambda value: struct.pack('<I', value)
ARRAY = re.compile(r'static const uint8_t (\w+)\[\]\s*=\s*\{(.*?)\};', re.S)


def octets(match):
    return bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', match[2]))


def render(name, data):
    return 'static const uint8_t ' + name + '[] = {\n' + ''.join(
        '    ' + ','.join('0x%02x' % byte for byte in data[at:at+12]) + ',\n'
        for at in range(0, len(data), 12)) + '};'


class Cursor:
    def __init__(self, data, schema):
        self.data, self.schema, self.at = data, schema, 0
        self.out, self.offsets, self.changes = bytearray(), {}, []

    def take(self, count):
        assert 0 <= count <= len(self.data) - self.at, (self.at, count, len(self.data))
        value = self.data[self.at:self.at+count]
        for offset in range(count):
            self.offsets[64+self.at+offset] = 64+len(self.out)+offset
        self.at += count
        self.out += value
        return value

    def word(self):
        return struct.unpack('<I', self.take(4))[0]

    def insert(self, count):
        self.changes.append(('insert_zero', 64+self.at, count))
        self.out += bytes(count)

    def vector(self, stride=4):
        self.take(self.word()*stride)

    def blob(self):
        return self.take(self.word())

    def application(self):
        self.word()
        self.vector()

    def applications(self):
        for _ in range(self.word()):
            self.application()

    def constraints(self, count):
        for _ in range(count):
            self.word()
            self.applications()

    def instructions(self):
        for _ in range(self.word()):
            offset = self.at
            operation = self.word()
            if self.schema == 28 and 154 <= operation <= 175:
                self.out[-4:] = WORD(operation+5)
                self.changes.append(('hash_ordinal', 64+offset, operation, operation+5))
            self.take(36)

    def declarations(self, functions):
        modules, slots, literals = self.word(), self.word(), self.word()
        self.take(8)
        for _ in range(modules):
            self.blob()
            self.vector()
            self.word()
            if self.schema == 28:
                self.insert(4)  # No lexical nesting in the retained fixed models.
            else:
                self.word()
        for _ in range(functions):
            self.take(36)
            if self.schema == 28:
                self.insert(4)  # All retained hash declarations have fixed arity.
            else:
                self.word()
        for _ in range(slots):
            self.take(12)
            if self.schema == 28:
                self.insert(8)  # Retained slot models contain no exported constants.
            else:
                self.take(8)
        for _ in range(literals):
            self.blob()
        for _ in range(self.word()):
            self.word()
            self.application()
            for _ in range(self.word()):
                self.application()
                self.take(8)

    def types(self):
        nodes, nominals, interfaces = self.word(), self.word(), self.word()
        for _ in range(nodes):
            kind = self.word()
            self.word()
            if kind == 1:
                self.vector(8)
                self.take(8)
            elif kind == 6:
                self.vector()
            elif kind in (2, 3, 5, 7, 8, 10):
                self.word()
            elif kind == 9:
                self.take(8)
            elif kind == 4:
                self.word()
                self.vector()
                self.vector()
            else:
                raise AssertionError(('type kind', kind, self.at))
        for _ in range(nominals):
            self.blob()
            self.blob()
            self.take(12)
            native = self.word()
            # Native Map/Set use structural node kinds in these fixed packets.
            assert self.schema != 28 or native not in (6, 7)
            self.take(32)
            self.constraints(self.word())
            for _ in range(self.word()):
                self.blob()
                self.take(8)
            for _ in range(self.word()):
                self.blob()
                self.take(8)
        for _ in range(interfaces):
            self.blob()
            self.blob()
            self.word()
            self.constraints(self.word())
            self.applications()
            for _ in range(self.word()):
                self.blob()
                self.take(8)
                self.constraints(self.word())
        return nominals

    def module(self, depth=0):
        assert depth <= 1
        self.word()
        functions, declarations = self.word(), self.word()
        assert declarations in (0, 1)
        for _ in range(functions):
            self.blob()
            self.vector()
            self.word()
            self.vector(16)
            self.instructions()
            self.vector()
        if declarations:
            self.declarations(functions)
        generic = self.word()
        assert generic in (0, 1)
        if generic:
            for _ in range(functions):
                count, kinds = self.word(), self.word()
                assert kinds in (0, 1)
                if kinds:
                    self.take(count*4)
                self.constraints(count)
                self.vector()
        nominals = self.types()
        assert self.word() == nominals
        for _ in range(nominals):
            self.word()
            self.vector()
        self.vector(16)
        provenance = self.word()
        assert provenance in (0, 1, 2)
        if provenance == 1:
            assert self.word() == functions
            for _ in range(functions):
                self.vector(8)
                self.word()
                self.vector(8)
                self.vector(12)
                self.vector(16)
        elif provenance == 2:
            self.module(depth+1)
            assert self.word() == functions
            for _ in range(functions):
                self.word()
                self.vector()
                self.vector(8)
            self.vector(24)


def frame(packet, schema, semantic):
    assert packet[:8] == b'XRCHK\0\0\0'
    assert struct.unpack_from('<II', packet, 8) == (schema, semantic)
    assert struct.unpack_from('<Q', packet, 24)[0] == len(packet)-64
    assert hashlib.sha256(packet[:32]+packet[64:]).digest() == packet[32:64]
    cursor = Cursor(packet[64:], schema)
    cursor.module()
    assert cursor.at == len(packet)-64
    head = packet[:8] + struct.pack('<II', 32, 84) + packet[16:24] + struct.pack('<Q', len(cursor.out))
    return head + hashlib.sha256(head+cursor.out).digest() + cursor.out, cursor


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    registry = json.loads((ROOT/'semantic84_inputs.json').read_text(encoding='utf-8'))
    for row in json.loads((ROOT/'semantic84_retained_manifest.json').read_text(encoding='utf-8')):
        assert hashlib.sha256((ROOT/row['source']).read_bytes()).hexdigest() == row['sha256'], row['source']
    packets, offsets, ordinals, insertions = 0, 0, 0, 0
    written, retired = {}, []
    for row in registry:
        raw = (ROOT/row['source']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == row['sha256'], row['source']
        text = raw.decode().replace('\r\n', '\n')
        replacements, digests, cursors = {}, {}, {}
        for match in ARRAY.finditer(text):
            old = octets(match)
            if old[:8] != b'XRCHK\0\0\0' or struct.unpack_from('<II', old, 8) != tuple(row['identity']):
                continue
            new, cursor = frame(old, *row['identity'])
            retired.append(('retired84_' + str(len(retired)) + '_' + match[1], old))
            replacements[match[1]], digests[old[32:64]], cursors[match[1]] = new, new[32:64], cursor
            packets += 1
            insertions += sum(change[0] == 'insert_zero' for change in cursor.changes)
            ordinals += sum(change[0] == 'hash_ordinal' for change in cursor.changes)
        assert sorted(replacements) == sorted(row['models']), row['source']
        def offset(match):
            nonlocal offsets
            name, value = match[1], int(match[2])
            choices = [model for prefix, model in row.get('offset_models', {}).items() if name.startswith(prefix)]
            if not choices:
                return match[0]
            assert len(choices) == 1
            new = cursors[choices[0]].offsets[value]
            offsets += 1
            return '#define ' + name + ' ' + str(new) + 'u'
        text = re.sub(r'^#define (\w+)\s+(\d+)u?\s*$', offset, text, flags=re.M)
        output = ARRAY.sub(lambda m: render(m[1], replacements.get(m[1], digests.get(octets(m), octets(m)))), text)
        def rename(match):
            name = match[0]
            for old, new in row.get('rename', {}).items():
                name = name.replace(old, new)
            return name
        output = re.sub(r'\b[A-Za-z_]\w*\b', rename, output)
        output = re.sub(r'(XR_XIR_CHECKED_SCHEMA\s*==\s*)(?:28|31)(?=u?\b)', r'\g<1>32', output)
        output = re.sub(r'(XR_XIR_CHECKED_CONTRACT\s*==\s*)(?:79|83)(?=u?\b)', r'\g<1>84', output)
        target = ROOT/row['target']
        if row['target'] in written:
            previous = {m[1]: octets(m) for m in ARRAY.finditer(written[row['target']])}
            current = {m[1]: octets(m) for m in ARRAY.finditer(output)}
            selected = {name: data for name, data in previous.items()
                        if data[:8] == b'XRCHK\0\0\0' and
                        struct.unpack_from('<II', data, 8) == (32, 84)}
            assert selected and all(current[name] == data for name, data in selected.items()), row['target']
            continue
        written[row['target']] = output
        if args.write:
            target.write_bytes(output.encode())
        else:
            assert target.read_text(encoding='utf-8') == output, row['target']
    assert (packets, offsets, insertions, ordinals) == (95, 88, 116, 48)
    for row in json.loads((ROOT/'semantic84_retired_inputs.json').read_text(encoding='utf-8')):
        raw = (ROOT/row['source']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == row['sha256'], row['source']
        selected = []
        for match in ARRAY.finditer(raw.decode()):
            data = octets(match)
            if match[1] not in row['models']:
                continue
            assert struct.unpack_from('<II', data, 8) == tuple(row['identity'])
            assert struct.unpack_from('<Q', data, 24)[0] == len(data)-64
            assert hashlib.sha256(data[:32]+data[64:]).digest() == data[32:64]
            retired.append(('retired84_' + str(len(retired)) + '_' + match[1], data))
            selected.append(match[1])
        assert sorted(selected) == sorted(row['models'])
    assert len(retired) == 113
    header(ROOT/'semantic84_retired_packets.h', 'SEMANTIC84_RETIRED_PACKETS_H', retired, args.write)
    table = '/* Full authenticated prior packets; no current body is retagged. */\n'
    table += 'static const Retired84Packet retired84_packets[] = {\n'
    table += ''.join('    {"'+name+'",'+name+',sizeof('+name+')},\n' for name, _ in retired)
    table += '};\n'
    target = ROOT/'semantic84_retired_table.h'
    if args.write:
        target.write_bytes(table.encode())
    else:
        assert target.read_text(encoding='utf-8') == table
    print(json.dumps(dict(complete_packets=packets, authenticated_offsets=offsets,
                          zero_field_insertions=insertions, hash_instruction_remaps=ordinals,
                          complete_recursive_grammars=True, product_writer_used=False)))


if __name__ == '__main__':
    main()
