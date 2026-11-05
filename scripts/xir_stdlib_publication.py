"""Exact private Checked-owner publication facts; no IR acceptance authority."""
from __future__ import annotations
import hashlib
import re

IDENTITY = re.compile(r'stdlib-module-v1:module=([1-9][0-9]*):([A-Za-z_][A-Za-z_0-9]*):path=([1-9][0-9]*):([A-Za-z_][A-Za-z_0-9]*(?:/[A-Za-z_][A-Za-z_0-9]*)+\.xr)')
PRELUDE = 'stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def'
PRELUDE_INPUTS = ('stdlib/prelude/builtin_symbols.def', 'stdlib/prelude/range.xr', 'stdlib/prelude/stringable.xr')


def module_identity(namespace, logical):
    if not isinstance(namespace, str) or not isinstance(logical, str):
        raise ValueError('invalid typed stdlib authority')
    value = f'stdlib-module-v1:module={len(namespace.encode())}:{namespace}:path={len(logical.encode())}:{logical}'
    parse_identity(value)
    return value


def parse_identity(value):
    if value == PRELUDE:
        return 'prelude', 'prelude/builtin_symbols.def'
    if not isinstance(value, str):
        raise ValueError('invalid module identity')
    match = IDENTITY.fullmatch(value)
    if not match or int(match[1]) != len(match[2].encode()) or int(match[3]) != len(match[4].encode()) or not match[4].startswith(match[2] + '/'):
        raise ValueError('noncanonical stdlib identity')
    return match[2], match[4]


def validate_modules(rows):
    if not isinstance(rows, list) or not rows:
        raise ValueError('empty module directory')
    names = set()
    for ordinal, row in enumerate(rows):
        if not isinstance(row, dict) or set(row) != {'ordinal', 'authority', 'namespace', 'logical_path', 'canonical', 'dependencies'}:
            raise ValueError('unknown module descriptor fields')
        if type(row['ordinal']) is not int or row['ordinal'] != ordinal or row['authority'] != 'stdlib':
            raise ValueError('module ordinal or authority mismatch')
        if parse_identity(row['canonical']) != (row['namespace'], row['logical_path']) or row['canonical'] in names:
            raise ValueError('duplicate or mismatched module identity')
        names.add(row['canonical'])
        edges = row['dependencies']
        if not isinstance(edges, list) or any(type(edge) is not int or edge < 0 or edge >= len(rows) for edge in edges) or len(set(edges)) != len(edges):
            raise ValueError('invalid module dependencies')
    states = [0] * len(rows)
    for start in range(len(rows)):
        if states[start]:
            continue
        states[start] = 1
        stack = [(start, 0)]
        while stack:
            node, edge = stack[-1]
            if edge == len(rows[node]['dependencies']):
                states[node] = 2
                stack.pop()
                continue
            target = rows[node]['dependencies'][edge]
            stack[-1] = (node, edge + 1)
            if states[target] == 1:
                raise ValueError('cyclic module dependencies')
            if not states[target]:
                states[target] = 1
                stack.append((target, 0))
    return names


def validate_sources(rows, modules):
    if not isinstance(rows, list):
        raise ValueError('invalid producer source directory')
    names = set()
    for row in rows:
        fields = {'namespace', 'logical_path', 'sourceKind', 'text', 'length', 'sha256'}
        if not isinstance(row, dict):
            raise ValueError('invalid producer source')
        generated = row.get('sourceKind') == 'generated-prelude'
        if set(row) != fields | ({'provenance'} if generated else set()) or not isinstance(row['text'], str):
            raise ValueError('unknown producer source fields')
        identity = module_identity(row['namespace'], row['logical_path'])
        if generated != (identity == PRELUDE) or (not generated and row['sourceKind'] != 'source'):
            raise ValueError('producer source kind differs from its exact module')
        raw = row['text'].encode('utf-8')
        if identity in names or type(row['length']) is not int or len(raw) != row['length'] or hashlib.sha256(raw).hexdigest() != row['sha256']:
            raise ValueError('producer source bytes or identity mismatch')
        names.add(identity)
        if generated:
            provenance = row['provenance']
            if not isinstance(provenance, dict) or set(provenance) != {'inputs', 'normalizedInputSha256', 'normalizedRangeSha256'}:
                raise ValueError('unknown generated prelude provenance')
            inputs = provenance['inputs']
            if not isinstance(inputs, list) or any(not isinstance(entry, dict) for entry in inputs) or [entry.get('path') for entry in inputs] != list(PRELUDE_INPUTS):
                raise ValueError('incomplete generated prelude inputs')
            for entry in inputs:
                if set(entry) != {'path', 'rawLength', 'rawSha256', 'normalizedSha256'} or type(entry['rawLength']) is not int or entry['rawLength'] <= 0 or any(not isinstance(entry[key], str) or not re.fullmatch('[a-f0-9]{64}', entry[key]) for key in ('rawSha256', 'normalizedSha256')):
                    raise ValueError('invalid raw or normalized prelude input fact')
            if provenance['normalizedInputSha256'] != inputs[0]['normalizedSha256'] or provenance['normalizedRangeSha256'] != inputs[1]['normalizedSha256']:
                raise ValueError('producer prelude fingerprint differs from normalized input')
            for entry in inputs:
                marker = '// ' + entry['path'].split('/')[-1] + ' sha256 ' + entry['normalizedSha256'] + '\n'
                if marker not in row['text']:
                    raise ValueError('generated prelude text differs from its input markers')
    if names != modules:
        raise ValueError('incomplete producer source directory')


def decoded_modules(record, sources):
    fields = {'schema', 'stage', 'root_module', 'modules'}
    if not isinstance(record, dict) or set(record) not in (fields, fields | {'generated_prelude'}) or any(type(record[key]) is not int for key in ('schema', 'stage', 'root_module')) or record['schema'] != 1 or record['stage'] != 2 or record['root_module'] != 0xffffffff or not isinstance(record['modules'], list):
        raise ValueError('not an actual Checked Library directory')
    result = []
    for row in record['modules']:
        if not isinstance(row, dict) or set(row) != {'ordinal', 'canonical', 'dependencies'}:
            raise ValueError('unknown decoded descriptor')
        namespace, logical = parse_identity(row['canonical'])
        if (namespace, logical) not in sources and row['canonical'] != PRELUDE:
            raise ValueError('decoded module has no exact producer source')
        result.append(dict(row, authority='stdlib', namespace=namespace, logical_path=logical))
    validate_modules(result)
    generated = record.get('generated_prelude')
    if any(row['canonical'] == PRELUDE for row in result):
        if not isinstance(generated, dict) or set(generated) != {'canonical', 'sourceKind', 'text', 'normalizedInputSha256', 'normalizedRangeSha256'} or generated['canonical'] != PRELUDE or generated['sourceKind'] != 'generated-prelude' or not isinstance(generated['text'], str):
            raise ValueError('missing exact generated prelude Source')
    elif generated is not None:
        raise ValueError('generated prelude has no decoded module')
    return result


def generated_prelude_source(root, generated):
    inputs = []
    for path in PRELUDE_INPUTS:
        raw = (root / path).read_bytes()
        normalized = raw.decode('utf-8').replace('\r\n', '\n').replace('\r', '\n').encode('utf-8')
        inputs.append(dict(path=path, rawLength=len(raw), rawSha256=hashlib.sha256(raw).hexdigest(),
                           normalizedSha256=hashlib.sha256(normalized).hexdigest()))
    text = generated['text']
    row = dict(namespace='prelude', logical_path='prelude/builtin_symbols.def', sourceKind='generated-prelude',
               text=text, length=len(text.encode('utf-8')), sha256=hashlib.sha256(text.encode('utf-8')).hexdigest(),
               provenance=dict(inputs=inputs, normalizedInputSha256=generated['normalizedInputSha256'],
                               normalizedRangeSha256=generated['normalizedRangeSha256']))
    validate_sources([row], {PRELUDE})
    return row
