#!/usr/bin/env python3
"""Encode complete independent Checked module graphs and strict rejection cases."""
import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIR = ROOT / 'tests/unit/xir/product_consumers'


def model():
    return dict(names=[b'base', b'left', b'right', b'main'],
                dependencies=[[], [0], [0], [1, 2]], initializers=[0, 1, 2, 3],
                owners=[0, 1, 2, 3, 3], exported=[0]*5, roles=[0]*5, root=3, entry=4)


def encode(m):
    def words(*v):
        return struct.pack('<' + 'I'*len(v), *v)
    def blob(v):
        return words(len(v)) + v
    def ins(op, type_id, value=0):
        return words(op, type_id, 0, 0, 0, 0) + struct.pack('<Q', value) + words(0, 0)
    body = words(0, 5, 1)
    for index in range(4):
        body += blob(('init'+str(index)).encode()) + words(0, 0, 1, 0, 1, 0, 0, 1)
        body += ins(33, 0) + words(0)
    body += blob(b'entry') + words(0, 2, 1, 0, 2, 0, 0, 2)
    body += ins(2, 2, 42) + ins(33, 0) + words(0)
    body += words(4, 0, 0, m['root'], m['entry'])
    modules = []
    for i in range(4):
        deps = m['dependencies'][i]
        modules.append(dict(offset=64+len(body)+4, length=len(m['names'][i]),
                            initializer=m['initializers'][i], dependencies=deps))
        body += blob(m['names'][i]) + words(len(deps), *deps, m['initializers'][i])
    for i in range(5):
        body += words(m['owners'][i], m['exported'][i], 0, 0, 0, 0, 0, m['roles'][i], 0)
    body += words(0, 0, 0, 0, 0, 0, 0)  # implementations, generics, types, defaults, provenance
    head = b'XRCHK\0\0\0' + words(25, 67, 2, 0) + struct.pack('<Q', len(body))
    return head + hashlib.sha256(head+body).digest() + body, modules


def cases():
    rows = [('diamond', model(), 'XR_XIR_OK')]
    m = model(); m['dependencies'][3] = [2, 1]
    rows.append(('reversed_edges', m, 'XR_XIR_OK'))
    m = model(); m.update(names=[b'main', b'left', b'right', b'base'],
                         dependencies=[[1, 2], [3], [3], []], owners=[0, 1, 2, 3, 0], root=0)
    rows.append(('forward_acyclic', m, 'XR_XIR_OK'))
    mutations = [
        ('empty_identity', 'names', 1, b''),
        ('duplicate_identity', 'names', 1, b'base'),
        ('foreign_initializer', 'initializers', 1, 0),
        ('stolen_function_owner', 'owners', 0, 1),
        ('initializer_out_of_range', 'initializers', 1, 5),
        ('function_module_out_of_range', 'owners', 1, 4),
        ('empty_module_ownership', 'owners', 1, 0),
        ('self_dependency', 'dependencies', 1, [1]),
        ('forward_cycle', 'dependencies', 1, [3]),
        ('duplicate_dependency', 'dependencies', 3, [1, 1]),
        ('missing_dependency', 'dependencies', 1, [4]),
        ('missing_initializer', 'initializers', 1, 0xffffffff),
        ('foreign_entry', 'owners', 4, 1),
        ('exported_initializer', 'exported', 1, 1),
        ('test_initializer', 'roles', 1, 1),
        ('nul_identity', 'names', 1, b'le\0ft'),
        ('invalid_utf8_identity', 'names', 1, b'\x80'),
    ]
    for name, field, index, value in mutations:
        m = model(); m[field][index] = copy.deepcopy(value)
        rows.append((name, m, 'XR_XIR_BAD_STRUCTURE'))
    for field, value in [('root', 4), ('entry', 5)]:
        m = model(); m[field] = value
        rows.append((field+'_out_of_range', m, 'XR_XIR_BAD_STRUCTURE'))
    return rows


def artifacts():
    lines = ['/* Independently encoded complete graphs; no product writer constructs inputs. */']
    manifest = dict(schema=1, controls=3, rejects=19, cases=[])
    for i, (name, m, expected) in enumerate(cases()):
        packet, modules = encode(m)
        lines.append(f'static const uint8_t module_graph_packet_{i}[] = {{')
        for off in range(0, len(packet), 32):
            lines.append('    '+','.join(f'0x{x:02x}' for x in packet[off:off+32])+',')
        lines.append('};')
        manifest['cases'].append(dict(name=name, expected=expected, bytes=len(packet),
            sha256=hashlib.sha256(packet).hexdigest(), modules=modules,
            owners=m['owners'], root=m['root'], entry=m['entry']))
    lines += ['typedef struct ModuleGraphRow {',
        '    size_t offset, length; uint32_t initializer, count, dependencies[2];',
        '} ModuleGraphRow;', 'typedef struct ModuleGraphCase {',
        '    const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected;',
        '    ModuleGraphRow modules[4]; uint32_t owners[5], root, entry;',
        '} ModuleGraphCase;', 'static const ModuleGraphCase module_graph_cases[] = {']
    for i, row in enumerate(manifest['cases']):
        modules = []
        for m in row['modules']:
            deps = m['dependencies']; padded = deps+[0]*(2-len(deps))
            modules.append('{%d,%d,%du,%d,{%s}}' % (m['offset'],m['length'],m['initializer'],len(deps),','.join(map(str,padded))))
        lines.append('    {"%s",module_graph_packet_%d,sizeof(module_graph_packet_%d),%s,\n     {%s},{%s},%d,%d},' %
                     (row['name'],i,i,row['expected'],','.join(modules),','.join(map(str,row['owners'])),row['root'],row['entry']))
    lines += ['};', '']
    return '\n'.join(lines), manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    header, manifest = artifacts()
    for path, text in {DIR/'module_graph_cases.h':header,
                       DIR/'module_graph_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode == 'write':
            path.write_text(text, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != text:
            raise ValueError(f'independent module graph fixture differs: {path}')
    print('Module graph obligations: 22 complete packets, 3 controls, 19 rejects')
