#!/usr/bin/env python3
"""Cross-check fixed math operations against governed native function declarations."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from gen_native_declarations import TypeParser
from stdlib_manifest import load_stdlibgen


def generate(root):
    metadata = load_stdlibgen(root).parse_def_metadata(root)
    entries = [entry for group in metadata for entry in group
               if entry.module == 'math' and entry.name.startswith('__')]
    names = {entry.name: entry for entry in entries}
    if len(names) != len(entries):
        raise ValueError('duplicate governed math declaration')
    source = (root / 'stdlib/defs/core.def').read_text(encoding='utf-8')
    rows = []
    used_ids, used_names = set(), set()
    pattern = re.compile(r'XR_XIR_MATH_(UNARY|BINARY|INTEGER|ENTROPY)\('
                         r'([A-Z_0-9]+),([1-9][0-9]*),"(__[A-Za-z0-9]+)",([A-Za-z0-9_]+)\)')
    for line in (root / 'src/xir/xxir_math.def').read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('/*'):
            continue
        match = pattern.fullmatch(line)
        if not match:
            raise ValueError('invalid fixed math operation: ' + line)
        kind, operation, identity, name, implementation = match.groups()
        identity = int(identity)
        if identity in used_ids or name in used_names or name not in names:
            raise ValueError('ambiguous math operation identity: ' + name)
        used_ids.add(identity)
        used_names.add(name)
        entry = names[name]
        parser = TypeParser(entry.signature)
        parameters = parser.parameters()
        parser.take(':')
        result = parser.type()
        if parser.peek() is not None or any(p[3] or p[4] for p in parameters):
            raise ValueError('math declaration must have fixed ordinary parameters')
        result_token = 'i64' if kind == 'INTEGER' or kind == 'ENTROPY' and implementation == 'I64' else 'f64'
        parameter_token = 'i64' if kind == 'ENTROPY' and implementation == 'I64' else 'f64'
        arity = 2 if kind == 'BINARY' or kind == 'ENTROPY' and implementation == 'I64' else 0 if kind == 'ENTROPY' else 1
        if result != ('name', result_token, ()) or len(parameters) != arity or any(
                p[1] != ('name', parameter_token, ()) for p in parameters):
            raise ValueError('governed math signature disagrees with fixed operation: ' + name)
        declarations = list(re.finditer(r'^\s*fn\s+(?P<name>' + re.escape(name) + r')\s*\{', source, re.M))
        if len(declarations) != 1:
            raise ValueError('math source range is not unique: ' + name)
        offset = declarations[0].start('name')
        line_number = source.count('\n', 0, offset) + 1
        column = offset - source.rfind('\n', 0, offset)
        ir = 'RANDOM_' + result_token.upper() if kind == 'ENTROPY' else 'MATH_' + (
            'BINARY_F64' if kind == 'BINARY' else 'UNARY_' + result_token.upper())
        rows.append('    {%du,XR_XIR_%s,%s,%s,%du,%du},' % (
            identity, ir, json.dumps(name), json.dumps(entry.signature), line_number, column))
    if used_names != set(names) or used_ids != set(range(1, len(rows) + 1)):
        raise ValueError('fixed math operations do not cover the complete governed family')
    fingerprint = ','.join(str(byte) for byte in hashlib.sha256(source.encode()).digest())
    return '\n'.join([
        '/* Generated from governed math declarations and fixed operation identities. */',
        'typedef struct SourceMathDeclaration {',
        '    uint32_t operation;',
        '    XrXirOp opcode;',
        '    const char *name,*signature;',
        '    uint32_t line,column;',
        '} SourceMathDeclaration;',
        'static const XrFingerprint source_math_fingerprint={{' + fingerprint + '}};',
        'static const SourceMathDeclaration source_math_declarations[]={',
        *rows, '};',
        '_Static_assert(sizeof(source_math_declarations)/sizeof(*source_math_declarations)==XR_XIR_MATH_COUNT-1,"complete math declarations");', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    result = generate(args.root).encode()
    if args.check:
        if args.output.read_bytes() != result:
            raise SystemExit('stale governed math declarations')
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(result)
    print(json.dumps({'declarations': 28, 'sha256': hashlib.sha256(result).hexdigest()}))


if __name__ == '__main__':
    main()
