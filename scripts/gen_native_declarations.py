#!/usr/bin/env python3
"""Project governed native value declarations into shared compiler metadata."""
from dataclasses import dataclass
import argparse
import hashlib
import json
from pathlib import Path
import re


TYPE_MARKER = re.compile(r'^\s*//\s*@native-type\s+id=([1-9][0-9]*)\s*$', re.M)
OP_MARKER = re.compile(r'//\s*@native-operation\s+([A-Z_]+)\s+allocation=(no_heap|may_heap)\s+'
                       r'failures=([a-z_,]+)\s+ownership=(owned|unit)\s*$')
IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
TOKENS = re.compile(r'\s*(\.\.\.|->|[A-Za-z_][A-Za-z0-9_]*|[<>()?,:])')


def c_string(value):
    return json.dumps(value, ensure_ascii=True)


def tokens(text):
    result, pos = [], 0
    while pos < len(text):
        match = TOKENS.match(text, pos)
        if not match:
            raise ValueError('invalid signature token: ' + text[pos:])
        result.append(match.group(1))
        pos = match.end()
    return result


class TypeParser:
    def __init__(self, text):
        self.items, self.pos = tokens(text), 0

    def peek(self):
        return self.items[self.pos] if self.pos < len(self.items) else None

    def take(self, value=None):
        item = self.peek()
        if item is None or value is not None and item != value:
            raise ValueError('expected ' + str(value) + ', got ' + str(item))
        self.pos += 1
        return item

    def ident(self):
        name = self.take()
        if not IDENT.fullmatch(name):
            raise ValueError('expected identifier')
        return name

    def type(self, depth=0):
        if depth >= 64:
            raise ValueError('native signature nesting exceeds its format limit')
        if self.peek() == 'const':
            self.take('const')
            return ('const', self.type(depth + 1))
        if self.peek() == '(':
            self.take('(')
            children = []
            if self.peek() != ')':
                children.append(self.type(depth + 1))
                while self.peek() == ',':
                    self.take(',')
                    children.append(self.type(depth + 1))
            self.take(')')
            term = ('tuple', tuple(children))
        elif self.peek() == 'fn':
            self.take('fn')
            params = self.parameters(depth + 1)
            result = ('tuple', ())
            if self.peek() == '->':
                self.take('->')
                result = self.type(depth + 1)
            term = ('function', tuple(params), result)
        else:
            name = self.ident()
            children = []
            if self.peek() == '<':
                self.take('<')
                children.append(self.type(depth + 1))
                while self.peek() == ',':
                    self.take(',')
                    children.append(self.type(depth + 1))
                self.take('>')
            term = ('name', name, tuple(children))
        if self.peek() == '?':
            self.take('?')
            term = ('nullable', term)
        return term

    def parameters(self, depth=0):
        self.take('(')
        params = []
        while self.peek() != ')':
            variadic = self.peek() == '...'
            if variadic:
                self.take('...')
            name = self.ident()
            optional = self.peek() == '?'
            if optional:
                self.take('?')
            self.take(':')
            start = self.pos
            term = self.type(depth + 1)
            spelling = ' '.join(self.items[start:self.pos])
            if any(item[0] == name for item in params):
                raise ValueError('duplicate parameter: ' + name)
            params.append((name, term, spelling, optional, variadic))
            if self.peek() != ',':
                break
            self.take(',')
        self.take(')')
        return params


@dataclass
class Member:
    name: str
    signature: str
    line: int
    column: int
    receiver: str
    static: bool
    method: bool
    lowered: bool
    parameters: list
    result: tuple
    result_text: str
    operation: str = 'NONE'
    allocation: str = 'unknown'
    failures: str = ''
    ownership: str = 'unknown'


def simple_term(term, binder):
    if term == ('tuple', ()):
        return 'UNIT'
    if term == ('name', 'i64', ()):
        return 'I64'
    if term == ('name', 'string', ()):
        return 'STRING'
    if term == ('name', 'bool', ()):
        return 'BOOL'
    if binder and term == ('name', binder, ()):
        return 'ELEMENT'
    return 'UNADMITTED'


def parse_source(source, prelude):
    marks = TYPE_MARKER.findall(source)
    if len(marks) != 1:
        raise ValueError('a governed native source needs exactly one type identity')
    # Companion error enums are syntax-checked but grant no native operation authority.
    enum_pattern = re.compile(r'^enum\s+([A-Za-z_]\w*)\s*\{\s*([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*\}', re.M)
    prefix = source[:TYPE_MARKER.search(source).start()]
    if int(marks[0]) == 2:
        seen = set()
        for enum in enum_pattern.finditer(prefix):
            if enum.group(1) in seen:
                raise ValueError('duplicate companion enum')
            seen.add(enum.group(1))
        prefix = enum_pattern.sub(lambda m: '\n' * m.group().count('\n'), prefix)
        if any(line.strip() and not line.strip().startswith('//') for line in prefix.splitlines()):
            raise ValueError('invalid native declaration preamble')
        source = prefix + source[TYPE_MARKER.search(source).start():]
    rows = source.splitlines()
    header = None
    members = []
    pending = None
    lowered = False
    closed = False
    for number, raw in enumerate(rows, 1):
        line = raw.strip()
        if not line:
            continue
        if line.startswith('//'):
            if '@native-operation' in line:
                if pending or not header or closed:
                    raise ValueError('misplaced native operation marker')
                pending = OP_MARKER.fullmatch(line)
                if not pending:
                    raise ValueError('invalid native operation contract')
            if line == '// @lowered':
                lowered = True
            continue
        if header is None:
            match = re.fullmatch(r'(struct|class)\s+([A-Za-z_]\w*)(?:<([A-Za-z_]\w*)>)?\s*\{', line)
            if not match:
                raise ValueError('expected a complete native declaration header')
            kind, name, binder = match.groups()
            binder = binder or ''
            identity = int(marks[0])
            if kind != 'struct' or (identity, name, bool(binder)) not in ((1, 'Array', True), (2, 'string', False)):
                raise ValueError('native identity, value kind or generic arity mismatch')
            if identity == 1 and not re.search(r'XR_BUILTIN_PRELUDE_TYPE\("Array",\s*1,', prelude):
                raise ValueError('native declaration has no exact one-parameter prelude binding')
            header = (int(marks[0]), name, binder, number, len(raw[:raw.index(name)].encode('utf-8')) + 1)
            continue
        if line == '}':
            if closed or pending:
                raise ValueError('unexpected closing brace or unconsumed contract')
            closed = True
            continue
        if closed:
            raise ValueError('trailing native declaration contents')
        static = line.startswith('static ')
        if static:
            line = line[7:].lstrip()
        receiver = 'READ'
        for prefix in ('ref', 'move'):
            if line.startswith(prefix + ' '):
                receiver = prefix.upper()
                line = line[len(prefix) + 1:].lstrip()
        if static and receiver != 'READ':
            raise ValueError('static member has no receiver')
        match = IDENT.match(line)
        if not match:
            raise ValueError('member name missing')
        name = match.group()
        signature = line[match.end():].strip()
        if any(member.name == name for member in members):
            raise ValueError('duplicate native member: ' + name)
        parser = TypeParser(signature)
        method = parser.peek() == '('
        params = []
        if method:
            params = parser.parameters()
            result = ('tuple', ())
            result_text = '()'
            if parser.peek() == '->':
                parser.take('->')
                start = parser.pos
                result = parser.type()
                result_text = ' '.join(parser.items[start:parser.pos])
        else:
            parser.take(':')
            result = parser.type()
            result_text = signature[1:].strip()
        if parser.peek() is not None:
            raise ValueError('trailing signature tokens')
        member = Member(name, signature, number, len(raw[:raw.index(name)].encode('utf-8')) + 1, receiver, static,
                        method, lowered, params, result, result_text)
        if pending:
            member.operation, member.allocation, member.failures, member.ownership = pending.groups()
        members.append(member)
        pending, lowered = None, False
    if header is None or not closed or pending:
        raise ValueError('incomplete native declaration')
    used = set()
    for member in members:
        if member.operation == 'NONE':
            continue
        if member.operation in used:
            raise ValueError('duplicate operation identity')
        used.add(member.operation)
        shape = (member.name, member.receiver,
                 tuple(simple_term(p[1], header[2]) for p in member.parameters),
                 simple_term(member.result, header[2]), member.allocation, member.failures, member.ownership)
        expected = {
            'ARRAY_GET': ('get', 'READ', ('I64',), 'ELEMENT', 'may_heap', 'bounds,allocation,retain,limit', 'owned'),
            'ARRAY_SET': ('set', 'REF', ('I64', 'ELEMENT'), 'UNIT', 'may_heap', 'bounds,allocation,retain,limit', 'unit'),
            'ARRAY_PUSH': ('push', 'REF', ('ELEMENT',), 'UNIT', 'may_heap', 'allocation,retain,limit', 'unit'),
            'STRING_CONTAINS': ('contains', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
            'STRING_STARTS_WITH': ('startsWith', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
            'STRING_ENDS_WITH': ('endsWith', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
        }.get(member.operation)
        if shape != expected or member.static or not member.method or any(p[3] or p[4] for p in member.parameters):
            raise ValueError('operation declaration disagrees with its semantic contract: ' + member.operation)
    required = {'ARRAY_GET', 'ARRAY_SET', 'ARRAY_PUSH'} if header[0] == 1 else {
        'STRING_CONTAINS', 'STRING_STARTS_WITH', 'STRING_ENDS_WITH'}
    if used != required:
        raise ValueError('incorrect admitted native operation set')
    return header, members


def render_type(root, stem):
    path = root / f'stdlib/types/{stem}.xr'
    source = path.read_text(encoding='utf-8').replace('\r\n', '\n')
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    header, members = parse_source(source, prelude)
    identity, name, binder, line, column = header
    if (stem, identity, name) not in (('array', 1, 'Array'), ('string', 2, 'string')):
        raise ValueError('native source path does not own this declaration identity')
    lines = []
    for index, member in enumerate(members):
        if not member.parameters:
            continue
        lines.append(f'static const XrNativeParameter xr_native_{stem}_parameters_{index}[] = {{')
        for pname, term, text, optional, variadic in member.parameters:
            lines.append(f'    {{{c_string(pname)}, {c_string(text)}, XR_NATIVE_TERM_{simple_term(term, binder)}, '
                         f'{str(optional).lower()}, {str(variadic).lower()}}},')
        lines.append('};')
    lines.append(f'static const XrNativeMemberDeclaration xr_native_{stem}_members[] = {{')
    for index, member in enumerate(members):
        params = f'xr_native_{stem}_parameters_{index}' if member.parameters else 'NULL'
        fields = [str(index + 1), c_string(member.name), c_string(member.signature),
                  c_string(member.result_text), str(member.line), str(member.column),
                  'XR_NATIVE_RECEIVER_' + member.receiver, str(member.static).lower(),
                  str(member.method).lower(), str(member.lowered).lower(), 'true',
                  'XR_NATIVE_OPERATION_' + member.operation,
                  'XR_NATIVE_ALLOCATION_' + member.allocation.upper(), c_string(member.failures),
                  'XR_NATIVE_OWNERSHIP_' + member.ownership.upper(), params, str(len(member.parameters)),
                  'XR_NATIVE_TERM_' + simple_term(member.result, binder)]
        lines.append('    {' + ', '.join(fields) + '},')
    digest = hashlib.sha256(source.encode()).digest()
    lines += ['};', f'static const XrNativeTypeDeclaration xr_native_{stem} = {{',
              f'    {identity}, XR_NATIVE_DECLARATION_VALUE, {c_string(name)}, {c_string(binder)}, {int(bool(binder))},',
              f'    "stdlib/types/{stem}.xr", "xray-native:prelude/{name}", {{{{{", ".join(str(b) for b in digest)}}}}},',
              f'    {line}, {column}, xr_native_{stem}_members, {len(members)}', '};', '']
    return lines, header, members


def render(root):
    lines = ['/* Generated by gen_native_declarations.py; do not edit. */']
    array_lines, header, members = render_type(root, "array")
    string_lines, _, _ = render_type(root, "string")
    lines += array_lines + string_lines
    binder = header[2]
    receiver = {}
    for operation in ('ARRAY_GET', 'ARRAY_SET', 'ARRAY_PUSH'):
        member = next(m for m in members if m.operation == operation)
        kinds = {'UNIT': 'UNIT', 'I64': 'INT', 'ELEMENT': 'RECEIVER_ELEM'}
        params = [kinds[simple_term(p[1], binder)] for p in member.parameters] + ['NONE'] * 3
        args = [operation, c_string(member.name), 'XA_BUILTIN_RECEIVER_ARRAY',
                'XA_BUILTIN_TYPE_' + kinds[simple_term(member.result, binder)],
                *['XA_BUILTIN_TYPE_' + p for p in params[:3]], str(len(member.parameters)),
                str(len(member.parameters)), 'XA_BUILTIN_TYPE_PARAMS_NONE',
                'XA_BUILTIN_EFFECT_' + ('READS_RECEIVER' if member.receiver == 'READ' else 'MUTATES_RECEIVER'),
                'XR_PARAM_' + member.receiver, 'XA_BUILTIN_ALLOCATION_' + member.allocation.upper(),
                'XA_BUILTIN_UNSAFE_NONE', c_string(operation.lower().replace('_', '.'))]
        receiver[operation] = 'XB_RECEIVER_METHOD(' + ', '.join(args) + ')'
    comment = '/* Generated from the same native declaration rows; do not edit. */\n'
    return {'src/shared/xnative_declarations.inc.c': '\n'.join(lines),
            'src/frontend/analyzer/xnative_array_index_methods.def':
                comment + receiver['ARRAY_GET'] + '\n' + receiver['ARRAY_SET'] + '\n',
            'src/frontend/analyzer/xnative_array_push_method.def': comment + receiver['ARRAY_PUSH'] + '\n'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    try:
        outputs = render(args.root)
        for relative, text in outputs.items():
            path = args.root / relative
            if args.check:
                if not path.exists() or path.read_text(encoding='utf-8') != text:
                    raise ValueError('stale generated native declaration: ' + relative)
            elif not path.exists() or path.read_text(encoding='utf-8') != text:
                path.write_text(text, encoding='utf-8', newline='\n')
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
