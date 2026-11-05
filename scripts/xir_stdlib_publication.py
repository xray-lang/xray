"""Exact private Checked-owner publication facts; no IR acceptance authority."""
from __future__ import annotations
import hashlib
import re

IDENTITY = re.compile(r'stdlib-module-v1:module=([1-9][0-9]*):([A-Za-z_][A-Za-z_0-9]*):path=([1-9][0-9]*):([A-Za-z_][A-Za-z_0-9]*(?:/[A-Za-z_][A-Za-z_0-9]*)+\.xr)')


def module_identity(namespace, logical):
    if not isinstance(namespace, str) or not isinstance(logical, str):
        raise ValueError('invalid typed stdlib authority')
    value = f'stdlib-module-v1:module={len(namespace.encode())}:{namespace}:path={len(logical.encode())}:{logical}'
    parse_identity(value)
    return value


def parse_identity(value):
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
        if not isinstance(row, dict) or set(row) != {'namespace', 'logical_path', 'text', 'length', 'sha256'} or not isinstance(row['text'], str):
            raise ValueError('unknown producer source fields')
        identity = module_identity(row['namespace'], row['logical_path'])
        raw = row['text'].encode('utf-8')
        if identity in names or type(row['length']) is not int or len(raw) != row['length'] or hashlib.sha256(raw).hexdigest() != row['sha256']:
            raise ValueError('producer source bytes or identity mismatch')
        names.add(identity)
    if names != modules:
        raise ValueError('incomplete producer source directory')


def decoded_modules(record, sources):
    if not isinstance(record, dict) or set(record) != {'schema', 'stage', 'root_module', 'modules'} or any(type(record[key]) is not int for key in ('schema', 'stage', 'root_module')) or record['schema'] != 1 or record['stage'] != 2 or record['root_module'] != 0xffffffff or not isinstance(record['modules'], list):
        raise ValueError('not an actual Checked Library directory')
    result = []
    for row in record['modules']:
        if not isinstance(row, dict) or set(row) != {'ordinal', 'canonical', 'dependencies'}:
            raise ValueError('unknown decoded descriptor')
        namespace, logical = parse_identity(row['canonical'])
        if (namespace, logical) not in sources:
            raise ValueError('decoded module has no exact producer source')
        result.append(dict(row, authority='stdlib', namespace=namespace, logical_path=logical))
    validate_modules(result)
    return result
