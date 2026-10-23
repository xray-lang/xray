#!/usr/bin/env python3
"""Project governed native value declarations into shared compiler metadata."""
from dataclasses import dataclass
import argparse
import hashlib
import json
from pathlib import Path
import re


TYPE_MARKER = re.compile(r'^\s*//\s*@native-type\s+id=([1-9][0-9]*)\s*$', re.M)
OP_MARKER = re.compile(r'//\s*@native-operation\s+([A-Z_][A-Z_0-9]*)\s+allocation=(no_heap|may_heap)\s+'
                       r'failures=([a-z_,]+)\s+ownership=(owned|unit)\s*$')
IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
TOKENS = re.compile(r'\s*(\.\.\.|->|[A-Za-z_][A-Za-z0-9_]*|[<>()?,:|])')


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
        if self.peek() == '|':
            self.take('|')
            right = self.type(depth + 1)
            term = ('union', (term,) + (right[1] if right[0] == 'union' else (right,)))
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
    if term == ('union', tuple(('name', name, ()) for name in ('string', 'rune', 'i64', 'f64', 'bool', 'null'))):
        return 'STRING_BUILDER_TEXT'
    if term == ('name', 'StringBuilder', ()):
        return 'STRING_BUILDER'
    if isinstance(binder, tuple):
        if len(binder) == 2:
            key, value = [('name', name, ()) for name in binder]
            if term == TypeParser(f'fn(key: {binder[0]}, value: {binder[1]})').type():
                return 'CALLBACK_KEY_VALUE_VISIT'
            return {key: 'KEY', value: 'VALUE', ('nullable', value): 'NULLABLE_VALUE',
                    ('name', 'Array', (key,)): 'ARRAY_KEY',
                    ('name', 'Array', (value,)): 'ARRAY_VALUE',
                    ('name', 'Array', (('tuple', (key, value)),)): 'ARRAY_KEY_VALUE'}.get(
                        term, simple_term(term, ''))
        binder = binder[0] if binder else ''
    if term == ('tuple', ()):
        return 'UNIT'
    if term == ('name', 'i64', ()):
        return 'I64'
    if term == ('nullable', ('name', 'i64', ())):
        return 'NULLABLE_I64'
    if term == ('name', 'f64', ()):
        return 'F64'
    if term == ('nullable', ('name', 'f64', ())):
        return 'NULLABLE_F64'
    if term == ('name', 'u32', ()):
        return 'U32'
    if term == ('name', 'Array', (('name', 'u8', ()),)):
        return 'ARRAY_U8'
    if term == ('name', 'string', ()):
        return 'STRING'
    if term == ('name', 'bool', ()):
        return 'BOOL'
    if term == ('name', 'Ordering', ()):
        return 'ORDERING'
    if binder and term == ('tuple', (('name', binder, ()), ('name', 'bool', ()))):
        return 'TUPLE_ELEMENT_BOOL'
    if binder and term == ('name', binder, ()):
        return 'ELEMENT'
    if binder:
        if term == ('name', 'U', ()):
            return 'RESULT_VARIABLE'
        if term == ('name', 'Set', (('name', binder, ()),)):
            return 'SET_ELEMENT'
        if term == ('name', 'Array', (('name', binder, ()),)):
            return 'ARRAY_ELEMENT'
        if term == ('name', 'Array', (('tuple', (('name', 'i64', ()), ('name', binder, ()))),)):
            return 'ARRAY_INDEX_ELEMENT'
        if term == ('name', 'Array', (('name', 'U', ()),)):
            return 'ARRAY_RESULT'
        if term == ('nullable', ('name', binder, ())):
            return 'NULLABLE_ELEMENT'
        if term == TypeParser(f'fn(value: {binder})').type():
            return 'CALLBACK_ELEMENT_VISIT'
        callback_terms = {
            f'fn(item: {binder}, index: i64) -> U': 'CALLBACK_MAP',
            f'fn(item: {binder}, index: i64) -> bool': 'CALLBACK_PREDICATE_INDEXED',
            f'fn(acc: U, item: {binder}) -> U': 'CALLBACK_REDUCE',
            f'fn(item: {binder}, index: i64)': 'CALLBACK_VISIT',
            f'fn(item: {binder}) -> bool': 'CALLBACK_PREDICATE',
        }
        for spelling, identity in callback_terms.items():
            if term == TypeParser(spelling).type():
                return identity
    return 'UNADMITTED'


def array_recipe_contracts(binder):
    """Exact declaration shapes; U denotes the checked callback result variable."""
    if binder == 'U':
        raise ValueError('Array element binder cannot shadow its callback result variable')
    callbacks = 'allocation,retain,limit,callback'
    ordinary = 'allocation,retain,limit'
    return {
        'ARRAY_MAP': ('map', 'READ', f'(fn: fn(item: {binder}, index: i64) -> U) -> Array<U>', callbacks, 'owned'),
        'ARRAY_FILTER': ('filter', 'READ', f'(fn: fn(item: {binder}, index: i64) -> bool) -> Array<{binder}>', callbacks, 'owned'),
        'ARRAY_REDUCE': ('reduce', 'READ', f'(fn: fn(acc: U, item: {binder}) -> U, initial: U) -> U', callbacks, 'owned'),
        'ARRAY_FOR_EACH': ('forEach', 'READ', f'(fn: fn(item: {binder}, index: i64)) -> ()', callbacks, 'unit'),
        'ARRAY_FIND': ('find', 'READ', f'(fn: fn(item: {binder}) -> bool) -> {binder}?', callbacks, 'owned'),
        'ARRAY_FIND_INDEX': ('findIndex', 'READ', f'(fn: fn(item: {binder}) -> bool) -> i64', callbacks, 'owned'),
        'ARRAY_EVERY': ('every', 'READ', f'(fn: fn(item: {binder}) -> bool) -> bool', callbacks, 'owned'),
        'ARRAY_SOME': ('some', 'READ', f'(fn: fn(item: {binder}) -> bool) -> bool', callbacks, 'owned'),
        'ARRAY_CONTAINS': ('contains', 'READ', f'(value: {binder}) -> bool', ordinary, 'owned'),
        'ARRAY_INDEX_OF': ('indexOf', 'READ', f'(value: {binder}) -> i64', ordinary, 'owned'),
        'ARRAY_JOIN': ('join', 'READ', '(separator?: string) -> string', ordinary, 'owned'),
        'ARRAY_CLEAR': ('clear', 'REF', '()', ordinary, 'unit'),
        'ARRAY_REVERSE': ('reverse', 'REF', f'() -> Array<{binder}>', ordinary, 'owned'),
        'ARRAY_UNSHIFT': ('unshift', 'REF', f'(value: {binder})', ordinary, 'unit'),
        'ARRAY_POP': ('pop', 'REF', f'() -> {binder}?', ordinary, 'owned'),
        'ARRAY_SHIFT': ('shift', 'REF', f'() -> {binder}?', ordinary, 'owned'),
        'ARRAY_RESIZE': ('resize', 'REF', f'(length: i64, fill: {binder}) -> Array<{binder}>', ordinary, 'owned'),
        'ARRAY_ENTRIES': ('entries', 'READ', f'() -> Array<(i64, {binder})>', ordinary, 'owned'),
        'ARRAY_FILL': ('fill', 'REF', f'(value: {binder}, start?: i64, end?: i64) -> Array<{binder}>', ordinary, 'owned'),
        'ARRAY_CONCAT': ('concat', 'READ', f'(...arrays: Array<{binder}>) -> Array<{binder}>', ordinary, 'owned'),
    }


def validate_array_recipe(member, contract):
    name, receiver, signature, failures, ownership = contract
    parser = TypeParser(signature)
    parameters = parser.parameters()
    result = ('tuple', ())
    if parser.peek() == '->':
        parser.take('->')
        result = parser.type()
    if (parser.peek() is not None or member.name != name or member.receiver != receiver or
            member.parameters != parameters or member.result != result or member.static or not member.method or
            member.lowered or member.allocation != 'may_heap' or member.failures != failures or member.ownership != ownership):
        raise ValueError('operation declaration disagrees with its semantic contract: ' + member.operation)


def parse_source(source, prelude):
    marks = TYPE_MARKER.findall(source)
    if len(marks) != 1:
        raise ValueError('a governed native source needs exactly one type identity')
    # Companion error enums are syntax-checked but grant no native operation authority.
    enum_pattern = re.compile(r'^enum\s+([A-Za-z_]\w*)\s*\{\s*([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*\}', re.M)
    prefix = source[:TYPE_MARKER.search(source).start()]
    if int(marks[0]) in (2, 12):
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
            match = re.fullmatch(r'(struct|class)\s+([A-Za-z_]\w*)(?:<([^<>]+)>)?\s*\{', line)
            if not match:
                raise ValueError('expected a complete native declaration header')
            kind, name, parameter_text = match.groups()
            parameters = []
            for text in parameter_text.split(',') if parameter_text else []:
                parameter = re.fullmatch(r'\s*([A-Za-z_]\w*)\s*(?::\s*(Hashable))?\s*', text)
                if not parameter or parameter.group(1) in [p[0] for p in parameters]:
                    raise ValueError('invalid or duplicate native generic parameter')
                parameters.append(parameter.groups())
            binder = tuple(p[0] for p in parameters)
            identity = int(marks[0])
            valid = (identity == 1 and name == 'Array' and len(parameters) == 1 and not parameters[0][1] or
                     identity == 2 and name == 'string' and not parameters or
                     identity == 10 and name == 'Map' and parameters == [('K', 'Hashable'), ('V', None)] or
                     identity == 11 and name == 'Set' and parameters == [('T', 'Hashable')])
            scalar = (identity, name) in ((12, 'i64'), (13, 'f64'), (14, 'bool'), (15, 'rune'), (16, 'StringBuilder')) and not parameters
            if not ((kind == 'struct' and valid) or (kind == 'class' and scalar)):
                raise ValueError('native identity, value kind or generic arity mismatch')
            if identity not in (2, 12, 13, 14, 15) and len(re.findall(r'XR_BUILTIN_PRELUDE_TYPE\("' + name + r'",\s*' +
                                               str(len(binder)) + r',', prelude)) != 1:
                raise ValueError('native declaration has no exact prelude binding')
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
    if header[0] in (13, 14, 15):
        contracts = {
            13: {'F64_TRY_PARSE': ('tryParse', '(text: string) -> f64?', 'may_heap', 'allocation,retain,limit'),
                'FLOAT_ABS': ('abs', '() -> f64', 'no_heap', 'none'),
                'FLOAT_TO_STRING': ('toString', '() -> string', 'may_heap', 'allocation,retain,limit'),
                'FLOAT_TO_I64': ('toI64', '() -> i64', 'no_heap', 'numeric_range'),
                'FLOAT_FLOOR': ('floor', '() -> i64', 'no_heap', 'numeric_range'),
                'FLOAT_CEIL': ('ceil', '() -> i64', 'no_heap', 'numeric_range'),
                'FLOAT_ROUND': ('round', '() -> i64', 'no_heap', 'numeric_range'),
                'FLOAT_IS_NAN': ('isNaN', '() -> bool', 'no_heap', 'none')},
            14: {'BOOL_TO_STRING': ('toString', '() -> string', 'may_heap', 'allocation,retain,limit')},
            15: {'RUNE_TO_U32': ('toUInt32', '() -> u32', 'no_heap', 'none'),
                'RUNE_TO_STRING': ('toString', '() -> string', 'may_heap', 'allocation,retain,limit')},
        }[header[0]]
        used = set()
        for member in members:
            if member.operation == 'NONE':
                continue
            shape = (member.name, member.signature, member.allocation, member.failures)
            static_parse = member.operation == 'F64_TRY_PARSE'
            if (shape != contracts.get(member.operation) or member.receiver != 'READ' or
                    member.static != static_parse or not member.method or member.lowered != static_parse or
                    member.ownership != 'owned' or any(p[3] or p[4] for p in member.parameters) or
                    member.operation in used):
                raise ValueError('scalar operation disagrees with its complete declaration contract')
            used.add(member.operation)
        if used != set(contracts):
            raise ValueError('scalar declaration omits an admitted recipe')
        return header, members
    if header[0] == 12:
        contracts = {'INTEGER_ABS': ('abs', '() -> i64'),
                     'INTEGER_WRAPPING_ADD': ('wrappingAdd', '(other: i64) -> i64'),
                     'INTEGER_WRAPPING_SUB': ('wrappingSub', '(other: i64) -> i64'),
                     'INTEGER_WRAPPING_MUL': ('wrappingMul', '(other: i64) -> i64'),
                     'INTEGER_TO_STRING': ('toString', '() -> string'),
                     'INTEGER_TO_HEX': ('toHex', '() -> string'),
                     'INTEGER_TO_F64': ('toF64', '() -> f64'),
                     'INTEGER_SQRT': ('sqrt', '() -> f64'),
                     'INTEGER_MIN': ('min', '(other: i64) -> i64'),
                     'INTEGER_MAX': ('max', '(other: i64) -> i64'),
                     'INTEGER_POPCOUNT': ('popcount', '() -> i64'),
                     'INTEGER_LEADING_ZEROS': ('leadingZeros', '() -> i64'),
                     'INTEGER_TRAILING_ZEROS': ('trailingZeros', '() -> i64'),
                     'INTEGER_BYTESWAP': ('byteswap', '() -> i64'),
                     'INTEGER_ROTATE_LEFT': ('rotateLeft', '(n: i64) -> i64'),
                     'INTEGER_ROTATE_RIGHT': ('rotateRight', '(n: i64) -> i64')}
        for base, kind in [('checked', 'CHECKED'), ('saturating', 'SATURATING')]:
            for suffix, op in [('Add', 'ADD'), ('Sub', 'SUB'), ('Mul', 'MUL')]:
                contracts['INTEGER_' + kind + '_' + op] = (base + suffix, '(other: i64) -> i64' + ('?' if base == 'checked' else ''))
        for base, op in [('add', 'ADD'), ('sub', 'SUB'), ('mul', 'MUL')]:
            contracts['INTEGER_' + op + '_OVERFLOWS'] = (base + 'Overflows', '(other: i64) -> bool')
        used = set()
        for member in members:
            if member.operation == 'NONE':
                continue
            expected = contracts.get(member.operation)
            if (expected != (member.name, member.signature) or member.receiver != 'READ' or
                    member.static or not member.method or member.lowered or
                    member.allocation != ('may_heap' if (member.operation in ('INTEGER_TO_STRING', 'INTEGER_TO_HEX') or member.operation.startswith('INTEGER_CHECKED_')) else 'no_heap') or
                    member.failures != ('allocation,retain,limit' if (member.operation in ('INTEGER_TO_STRING', 'INTEGER_TO_HEX') or member.operation.startswith('INTEGER_CHECKED_')) else 'none') or member.ownership != 'owned'):
                raise ValueError('integer operation disagrees with its complete declaration contract')
            used.add(member.operation)
        if used != set(contracts):
            raise ValueError('integer declaration omits an admitted recipe')
        return header, members
    if header[0] in (10, 11):
        validate_hash_members(header[0], members)
        return header, members
    if header[0] == 16:
        expected = {
            'append': ('STRING_BUILDER_APPEND', ('STRING_BUILDER_TEXT',), 'STRING_BUILDER', 'may_heap', 'allocation,retain,limit'),
            'toString': ('STRING_BUILDER_TO_STRING', (), 'STRING', 'no_heap', 'retain,limit'),
            'clear': ('STRING_BUILDER_CLEAR', (), 'STRING_BUILDER', 'no_heap', 'retain,limit')}
        if {member.name for member in members} != set(expected):
            raise ValueError('StringBuilder requires its exact declared member set')
        for member in members:
            actual = (member.operation, tuple(simple_term(p[1], '') for p in member.parameters),
                      simple_term(member.result, ''), member.allocation, member.failures)
            if actual != expected[member.name] or member.receiver != 'READ' or member.static or not member.method or member.lowered or member.ownership != 'owned' or any(p[3] or p[4] for p in member.parameters):
                raise ValueError('StringBuilder member differs from its closed contract')
        return header, members
    recipes = array_recipe_contracts(header[2][0]) if header[0] == 1 else {}
    used = set()
    for member in members:
        if member.operation == 'NONE':
            continue
        if member.operation in used:
            raise ValueError('duplicate operation identity')
        used.add(member.operation)
        if member.operation in recipes:
            validate_array_recipe(member, recipes[member.operation])
            continue
        shape = (member.name, member.receiver,
                 tuple(simple_term(p[1], header[2]) for p in member.parameters),
                 simple_term(member.result, header[2]), member.allocation, member.failures, member.ownership)
        expected = {
            'ARRAY_GET': ('get', 'READ', ('I64',), 'ELEMENT', 'may_heap', 'bounds,allocation,retain,limit', 'owned'),
            'ARRAY_SET': ('set', 'REF', ('I64', 'ELEMENT'), 'UNIT', 'may_heap', 'bounds,allocation,retain,limit', 'unit'),
            'ARRAY_PUSH': ('push', 'REF', ('ELEMENT',), 'UNIT', 'may_heap', 'allocation,retain,limit', 'unit'),
            'ARRAY_CAPACITY': ('capacity', 'READ', (), 'I64', 'no_heap', 'limit', 'owned'),
            'ARRAY_WITH_CAPACITY': ('withCapacity', 'READ', ('I64',), 'ARRAY_ELEMENT', 'may_heap', 'allocation,retain,limit', 'owned'),
            'ARRAY_RESERVE': ('reserve', 'REF', ('I64',), 'ARRAY_ELEMENT', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_INDEX_OF': ('indexOf', 'READ', ('STRING', 'I64'), 'I64', 'no_heap', 'bounds,limit', 'owned'),
            'STRING_LAST_INDEX_OF': ('lastIndexOf', 'READ', ('STRING',), 'I64', 'no_heap', 'limit', 'owned'),
            'STRING_CONTAINS': ('contains', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
            'STRING_STARTS_WITH': ('startsWith', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
            'STRING_ENDS_WITH': ('endsWith', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
            'STRING_TO_STRING': ('toString', 'READ', (), 'STRING', 'no_heap', 'retain', 'owned'),
            'STRING_REPEAT': ('repeat', 'READ', ('I64',), 'STRING', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_COPY_BYTES': ('copyBytes', 'READ', (), 'ARRAY_U8', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_REPLACE': ('replace', 'READ', ('STRING', 'STRING'), 'STRING', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_REPLACE_ALL': ('replaceAll', 'READ', ('STRING', 'STRING'), 'STRING', 'may_heap', 'allocation,retain,limit', 'owned'),
        }.get(member.operation)
        optional = tuple(p[3] for p in member.parameters)
        expected_optional = (False, True) if member.operation == 'STRING_INDEX_OF' else (False,) * len(member.parameters)
        expected_static = member.operation == 'ARRAY_WITH_CAPACITY'
        expected_method = member.operation != 'ARRAY_CAPACITY'
        if shape != expected or member.static != expected_static or member.method != expected_method or optional != expected_optional or any(p[4] for p in member.parameters):
            raise ValueError('operation declaration disagrees with its semantic contract: ' + member.operation)
    required = {'ARRAY_GET', 'ARRAY_SET', 'ARRAY_PUSH', 'ARRAY_CAPACITY', 'ARRAY_WITH_CAPACITY', 'ARRAY_RESERVE'} | set(recipes) if header[0] == 1 else {
        'STRING_CONTAINS', 'STRING_STARTS_WITH', 'STRING_ENDS_WITH', 'STRING_INDEX_OF', 'STRING_LAST_INDEX_OF',
        'STRING_TO_STRING', 'STRING_REPEAT', 'STRING_COPY_BYTES', 'STRING_REPLACE', 'STRING_REPLACE_ALL'}
    if used != required:
        raise ValueError('incorrect admitted native operation set')
    return header, members


def validate_hash_members(identity, members):
    """Admitted members require their complete signature and operation contract."""
    contracts = {
        10: {'get': ('READ', '(key: K) -> V?'), 'set': ('REF', '(key: K, value: V)'),
            'containsKey': ('READ', '(key: K) -> bool'), 'containsValue': ('READ', '(value: V) -> bool'),
            'delete': ('REF', '(key: K) -> bool'), 'clear': ('REF', '()'),
            'keys': ('READ', '() -> Array<K>'), 'values': ('READ', '() -> Array<V>'),
            'entries': ('READ', '() -> Array<(K, V)>'), 'forEach': ('READ', '(fn: fn(key: K, value: V)) -> ()'),
            'iterator': ('READ', '() -> Iterator<K>'), 'entriesIterator': ('READ', '() -> Iterator<(K, V)>'),
            'toString': ('READ', '() -> string')},
        11: {'add': ('REF', '(value: T)'), 'contains': ('READ', '(value: T) -> bool'),
            'delete': ('REF', '(value: T) -> bool'), 'clear': ('REF', '()'),
            'values': ('READ', '() -> Array<T>'), 'forEach': ('READ', '(fn: fn(value: T)) -> ()'),
            **{name: ('READ', '(other: Set<T>) -> Set<T>') for name in
               ('union', 'intersection', 'difference', 'symmetricDifference')},
            **{name: ('READ', '(other: Set<T>) -> bool') for name in ('isSubset', 'isSuperset')},
            'toArray': ('READ', '() -> Array<T>'), 'iterator': ('READ', '() -> Iterator<T>'),
            'toString': ('READ', '() -> string')},
    }[identity]
    if {member.name for member in members} != set(contracts):
        raise ValueError('incorrect native hash member inventory')
    admitted = {10: {'get': 'MAP_GET', 'set': 'MAP_SET', 'containsKey': 'HASH_CONTAINS',
                    'containsValue': 'MAP_CONTAINS_VALUE',
                    'delete': 'HASH_DELETE', 'clear': 'HASH_CLEAR', 'keys': 'HASH_KEYS',
                    'values': 'HASH_VALUES', 'entries': 'MAP_ENTRIES', 'forEach': 'HASH_FOR_EACH'},
                11: {'add': 'SET_ADD', 'contains': 'HASH_CONTAINS',
                    'delete': 'HASH_DELETE', 'clear': 'HASH_CLEAR', 'values': 'HASH_VALUES', 'toArray': 'HASH_VALUES',
                    'union': 'SET_UNION', 'intersection': 'SET_INTERSECTION', 'difference': 'SET_DIFFERENCE',
                    'symmetricDifference': 'SET_SYMMETRIC_DIFFERENCE',
                    'isSubset': 'SET_IS_SUBSET', 'isSuperset': 'SET_IS_SUPERSET', 'forEach': 'HASH_FOR_EACH'}}[identity]
    for member in members:
        receiver, signature = contracts[member.name]
        operation = admitted.get(member.name, 'NONE')
        if operation != 'NONE':
            ownership = 'unit' if member.name in ('set', 'add', 'clear', 'forEach') else 'owned'
            failures = 'allocation,retain,limit,callback' if member.name == 'forEach' else 'allocation,retain,limit'
            if (member.operation, member.allocation, member.failures, member.ownership) != (
                    operation, 'may_heap', failures, ownership):
                raise ValueError('native hash operation disagrees with its execution contract: ' + member.name)
        if (member.receiver != receiver or tokens(member.signature) != tokens(signature) or
                member.static or not member.method or member.lowered or member.operation != operation):
            raise ValueError('native hash declaration disagrees with its signature contract: ' + member.name)


def render_type(root, stem):
    path = root / f'stdlib/types/{stem}.xr'
    source = path.read_text(encoding='utf-8').replace('\r\n', '\n')
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    header, members = parse_source(source, prelude)
    identity, name, binder, line, column = header
    if (stem, identity, name) not in (('array', 1, 'Array'), ('string', 2, 'string'),
                                     ('map', 10, 'Map'), ('set', 11, 'Set'),
                                     ('i64', 12, 'i64'), ('f64', 13, 'f64'),
                                     ('bool', 14, 'bool'), ('rune', 15, 'rune'),
                                     ('stringbuilder', 16, 'StringBuilder')):
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
                  'XR_NATIVE_RESULT_OWNERSHIP_' + member.ownership.upper(), params, str(len(member.parameters)),
                  'XR_NATIVE_TERM_' + simple_term(member.result, binder)]
        lines.append('    {' + ', '.join(fields) + '},')
    digest = hashlib.sha256(source.encode()).digest()
    lines.append('};')
    if binder:
        lines.append(f'static const char *const xr_native_{stem}_type_parameters[] = ' +
                     '{' + ', '.join(c_string(name) for name in binder) + '};')
    type_parameters = f'xr_native_{stem}_type_parameters' if binder else 'NULL'
    lines += [f'static const XrNativeTypeDeclaration xr_native_{stem} = {{',
              f'    {identity}, XR_NATIVE_DECLARATION_{"IDENTITY" if identity == 16 else "VALUE"}, {c_string(name)}, {type_parameters}, {len(binder)},',
              f'    "stdlib/types/{stem}.xr", "xray-native:prelude/{name}", {{{{{", ".join(str(b) for b in digest)}}}}},',
              f'    {line}, {column}, xr_native_{stem}_members, {len(members)}', '};', '']
    return lines, header, members


def render_atomic_identities(root):
    source = (root / 'stdlib/types/atomic.xr').read_text(encoding='utf-8').replace('\r\n', '\n')
    lines = [line.strip() for line in source.splitlines() if line.strip() and not line.strip().startswith('//')]
    expected = [
        'class Atomic<T: AtomicValue> {',
        'load(ordering?: Ordering) -> T',
        'store(value: T, ordering?: Ordering)',
        'add(delta: T, ordering?: Ordering) where T: AtomicNumber',
        'sub(delta: T, ordering?: Ordering) where T: AtomicNumber',
        'fetchAdd(delta: T, ordering?: Ordering) -> T where T: AtomicNumber',
        'fetchSub(delta: T, ordering?: Ordering) -> T where T: AtomicNumber',
        'swap(value: T, ordering?: Ordering) -> T',
        'compareExchange(expected: T, desired: T, ordering?: Ordering) -> (T, bool)',
        'toggle(ordering?: Ordering) -> bool where T: AtomicBoolean',
        'toString() -> string', '}']
    if lines != expected:
        raise ValueError('Atomic declaration differs from its frozen closed contract')
    output = []
    operations = ['LOAD','STORE','ADD','SUB','FETCH_ADD','FETCH_SUB','SWAP','COMPARE_EXCHANGE','TOGGLE','TO_STRING']
    facts = []
    for index, signature in enumerate(lines[1:-1]):
        name = signature.split('(', 1)[0]
        parser = TypeParser(signature.split(' where ', 1)[0][len(name):])
        parameters = parser.parameters()
        result = ('tuple', ())
        if parser.peek() == '->':
            parser.take('->')
            result = parser.type()
        if parser.peek() is not None:
            raise ValueError('Atomic member has an unparsed type contract')
        source_line = next((number, raw) for number, raw in enumerate(source.splitlines(), 1)
                           if raw.strip() == signature)
        facts.append((index, name, signature, parameters, result, source_line))
        if parameters:
            output.append(f'static const XrNativeParameter xr_native_atomic_parameters_{index}[] = {{')
            for pname, term, spelling, optional, variadic in parameters:
                output.append('    {' + ', '.join([c_string(pname), c_string(spelling),
                    'XR_NATIVE_TERM_' + simple_term(term, 'T'), str(optional).lower(), str(variadic).lower()]) + '},')
            output.append('};')
    output += ['static const XrNativeMemberDeclaration xr_native_atomic_members[] = {']
    for index, name, signature, parameters, result, (number, raw) in facts:
        params = f'xr_native_atomic_parameters_{index}' if parameters else 'NULL'
        result_text = signature.split('->', 1)[1].split(' where ', 1)[0].strip() if '->' in signature else '()'
        output.append('    {' + ', '.join([str(index+1), c_string(name), c_string(signature),
            c_string(result_text), str(number), str(raw.index(name)+1),
            'XR_NATIVE_RECEIVER_READ', 'false', 'true', 'true', 'true',
            'XR_NATIVE_OPERATION_ATOMIC_' + operations[index], 'XR_NATIVE_ALLOCATION_MAY_HEAP',
            c_string('allocation,retain,limit,unsupported' + ('' if index == 9 else ',atomic_argument')),
            'XR_NATIVE_RESULT_OWNERSHIP_' + ('UNIT' if index in (1,2,3) else 'OWNED'), params, str(len(parameters)),
            'XR_NATIVE_TERM_' + simple_term(result, 'T')]) + '},')
    digest = hashlib.sha256(source.encode()).digest()
    output += ['};', 'static const char *const xr_native_atomic_type_parameters[] = {"T"};',
        'static const XrNativeTypeDeclaration xr_native_atomic = {',
        '    3, XR_NATIVE_DECLARATION_IDENTITY, "Atomic", xr_native_atomic_type_parameters, 1,',
        '    "stdlib/types/atomic.xr", "xray-native:prelude/Atomic", {{' + ', '.join(str(b) for b in digest) + '}},',
        '    2, 7, xr_native_atomic_members, 10', '};']
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8').replace('\r\n', '\n')
    match = re.search(r'XR_BUILTIN_ENUM\("Ordering",\s*0,\s*ORDERING,(.*?)\n\n', prelude, re.S)
    variants = re.findall(r'XR_BUILTIN_ENUM_VARIANT\("([^" ]+)",\s*NONE\)', match.group(1)) if match else []
    canonical = ['Relaxed','Acquire','Release','AcquireRelease','SeqCst']
    expected_variants = ''.join('XR_BUILTIN_ENUM_VARIANT("'+v+'",NONE)' for v in canonical) + ')'
    if variants != canonical or ''.join(match.group(1).split()) != expected_variants:
        raise ValueError('Ordering registry differs from canonical variant identity')
    ordering_line, ordering_raw = next((number, raw) for number, raw in enumerate(prelude.splitlines(), 1)
                                      if 'XR_BUILTIN_ENUM("Ordering",' in raw)
    output += ['static const XrNativeMemberDeclaration xr_native_ordering_members[] = {']
    for index, name in enumerate(variants):
        variant_line, variant_raw = next((number, raw) for number, raw in enumerate(prelude.splitlines(), 1)
                                         if 'XR_BUILTIN_ENUM_VARIANT("'+name+'",' in raw)
        output.append('    {' + ', '.join([str(index+1),c_string(name),c_string(name),'"Ordering"',
            str(variant_line),str(variant_raw.index(name)+1),'XR_NATIVE_RECEIVER_READ','true','false','false','true',
            'XR_NATIVE_OPERATION_NONE','XR_NATIVE_ALLOCATION_NO_HEAP','"none"',
            'XR_NATIVE_RESULT_OWNERSHIP_UNKNOWN','NULL','0','XR_NATIVE_TERM_UNADMITTED']) + '},')
    digest = hashlib.sha256(prelude.encode()).digest()
    output += ['};','static const XrNativeTypeDeclaration xr_native_ordering = {',
        '    4, XR_NATIVE_DECLARATION_VALUE, "Ordering", NULL, 0,',
        '    "stdlib/prelude/builtin_symbols.def", "xray-native:prelude/Ordering", {{' + ', '.join(str(b) for b in digest) + '}},',
        f'    {ordering_line}, {ordering_raw.index("Ordering")+1}, xr_native_ordering_members, 5', '};']
    return output

def render_task_identity(root):
    """Describe the governed handle without granting member execution recipes."""
    source = (root / 'stdlib/types/coroutine.xr').read_text(encoding='utf-8').replace('\r\n', '\n')
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    bindings = re.findall(r'^\s*XR_BUILTIN_TYPE\("Task",\s*([0-9]+)\)\s*$', prelude, re.M)
    registrations = re.findall(r'XR_BUILTIN_[A-Z_]+\(\s*"Task"', prelude)
    if bindings != ['1'] or len(registrations) != 1:
        raise ValueError('Task registry needs one exact one-parameter binding')
    if re.search(r'@native-|@lowered', source):
        raise ValueError('Task declaration grants no native member operation')
    rows = [(number, raw) for number, raw in enumerate(source.splitlines(), 1)
            if raw.strip() and not raw.strip().startswith('//')]
    expected = [
        'enum TaskResult<T> {', 'Success { value: T },', 'Failed { error: Error },',
        'Cancelled,', 'Timeout,', 'Pending,', '}',
        'enum TaskStatus {', 'Pending,', 'Running,', 'Success,', 'Failed,', 'Cancelled,', '}',
        'class Task<T> {', 'done: bool', 'status: TaskStatus', 'ref cancel()',
        'ref poll() -> TaskResult<T>', 'ref awaitResult() -> TaskResult<T>',
        'ref awaitTimeout(timeout: i64) -> TaskResult<T>', '}',
    ]
    if [raw.strip() for _, raw in rows] != expected:
        raise ValueError('Task source differs from its complete governed declaration')
    output = []
    facts = []
    for index, (number, raw) in enumerate(rows[15:21]):
        text = raw.strip()
        receiver = 'REF' if text.startswith('ref ') else 'READ'
        if receiver == 'REF':
            text = text[4:]
        name = IDENT.match(text).group()
        signature = text[len(name):]
        parser = TypeParser(signature)
        method = parser.peek() == '('
        parameters = parser.parameters() if method else []
        result_text = '()'
        if not method:
            parser.take(':')
            result_text = signature[1:].strip()
            parser.type()
        elif parser.peek() == '->':
            parser.take('->')
            result_text = signature.split('->', 1)[1].strip()
            parser.type()
        if parser.peek() is not None:
            raise ValueError('trailing Task member signature')
        if parameters:
            output.append(f'static const XrNativeParameter xr_native_task_parameters_{index}[] = {{')
            for pname, _, spelling, optional, variadic in parameters:
                output.append('    {' + ', '.join([c_string(pname), c_string(spelling),
                    'XR_NATIVE_TERM_UNADMITTED', str(optional).lower(), str(variadic).lower()]) + '},')
            output.append('};')
        facts.append((index, number, raw, name, signature, receiver, method, parameters, result_text))
    output.append('static const XrNativeMemberDeclaration xr_native_task_members[] = {')
    for index, number, raw, name, signature, receiver, method, parameters, result_text in facts:
        params = f'xr_native_task_parameters_{index}' if parameters else 'NULL'
        output.append('    {' + ', '.join([str(index + 1), c_string(name), c_string(signature),
            c_string(result_text), str(number), str(len(raw[:raw.index(name)].encode('utf-8')) + 1),
            'XR_NATIVE_RECEIVER_' + receiver, 'false', str(method).lower(), 'false', 'true',
            'XR_NATIVE_OPERATION_NONE', 'XR_NATIVE_ALLOCATION_UNKNOWN', '""',
            'XR_NATIVE_RESULT_OWNERSHIP_UNKNOWN', params, str(len(parameters)),
            'XR_NATIVE_TERM_UNADMITTED']) + '},')
    number, raw = rows[14]
    digest = hashlib.sha256(source.encode('utf-8')).digest()
    output += ['};', 'static const char *const xr_native_task_type_parameters[] = {"T"};',
        'static const XrNativeTypeDeclaration xr_native_task = {',
        '    5, XR_NATIVE_DECLARATION_IDENTITY, "Task", xr_native_task_type_parameters, 1,',
        '    "stdlib/types/coroutine.xr", "xray-native:prelude/Task", {{' +
        ', '.join(str(byte) for byte in digest) + '}},',
        f'    {number}, {len(raw[:raw.index("Task")].encode("utf-8")) + 1}, xr_native_task_members, 6', '};']
    return output



def render_iterator_identity(root):
    """Bind the complete declaration and its compiler-owned pull recipes."""
    source = (root / 'stdlib/types/iterator.xr').read_text(encoding='utf-8').replace('\r\n', '\n')
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    binding = re.findall(r'XR_BUILTIN_PRELUDE_TYPE\("Iterator",\s*1,\s*XR_TINSTANCE,\s*GENERIC_1\)', prelude)
    registrations = re.findall(r'XR_BUILTIN_[A-Z_]+\(\s*"Iterator"', prelude)
    if len(binding) != 1 or len(registrations) != 1:
        raise ValueError('Iterator registry needs one exact one-parameter binding')
    if re.search(r'@native-', source):
        raise ValueError('Iterator identity grants no native member operation')
    rows = [(number, raw) for number, raw in enumerate(source.splitlines(), 1)
            if raw.strip() and not raw.strip().startswith('//')]
    expected = ['class Iterator<T> {', 'hasNext() -> bool', 'next() -> T',
                'nth(index: i64) -> T', 'iterator() -> Iterator<T>', 'toString() -> string', '}']
    if [raw.strip() for _, raw in rows] != expected:
        raise ValueError('Iterator source differs from its complete governed declaration')
    output, facts = [], []
    for index, (number, raw) in enumerate(rows[1:-1]):
        text = raw.strip()
        name = IDENT.match(text).group()
        signature = text[len(name):]
        parser = TypeParser(signature)
        parameters = parser.parameters()
        parser.take('->')
        result = parser.type()
        if parser.peek() is not None:
            raise ValueError('trailing Iterator member signature')
        result_text = signature.split('->', 1)[1].strip()
        if parameters:
            output.append(f'static const XrNativeParameter xr_native_iterator_parameters_{index}[] = {{')
            for pname, term, spelling, optional, variadic in parameters:
                output.append('    {' + ', '.join([c_string(pname), c_string(spelling),
                    'XR_NATIVE_TERM_' + simple_term(term, 'T'), str(optional).lower(), str(variadic).lower()]) + '},')
            output.append('};')
        lowered = number > 1 and source.splitlines()[number - 2].strip() == '// @lowered'
        if name in ('hasNext', 'next', 'nth') and not lowered:
            raise ValueError('Iterator pull operation requires its exact lowered signature')
        facts.append((index, number, raw, name, signature, parameters, result, result_text, lowered))
    output.append('static const XrNativeMemberDeclaration xr_native_iterator_members[] = {')
    for index, number, raw, name, signature, parameters, result, result_text, lowered in facts:
        params = f'xr_native_iterator_parameters_{index}' if parameters else 'NULL'
        operation = {'hasNext': 'ITERATOR_HAS_NEXT', 'next': 'ITERATOR_NEXT', 'nth': 'ITERATOR_NTH'}.get(name, 'NONE')
        output.append('    {' + ', '.join([str(index + 1), c_string(name), c_string(signature),
            c_string(result_text), str(number), str(len(raw[:raw.index(name)].encode('utf-8')) + 1),
            'XR_NATIVE_RECEIVER_READ', 'false', 'true', str(lowered).lower(), 'true',
            'XR_NATIVE_OPERATION_' + operation,
            'XR_NATIVE_ALLOCATION_MAY_HEAP' if operation != 'NONE' else 'XR_NATIVE_ALLOCATION_UNKNOWN',
            '"IteratorExhausted"' if name in ('next', 'nth') else '""',
            'XR_NATIVE_RESULT_OWNERSHIP_OWNED' if operation != 'NONE' else 'XR_NATIVE_RESULT_OWNERSHIP_UNKNOWN',
            params, str(len(parameters)),
            'XR_NATIVE_TERM_' + simple_term(result, 'T')]) + '},')
    number, raw = rows[0]
    digest = hashlib.sha256(source.encode('utf-8')).digest()
    output += ['};', 'static const char *const xr_native_iterator_type_parameters[] = {"T"};',
        'static const XrNativeTypeDeclaration xr_native_iterator = {',
        '    9, XR_NATIVE_DECLARATION_IDENTITY, "Iterator", xr_native_iterator_type_parameters, 1,',
        '    "stdlib/types/iterator.xr", "xray-native:prelude/Iterator", {{' +
        ', '.join(str(byte) for byte in digest) + '}},',
        f'    {number}, {len(raw[:raw.index("Iterator")].encode("utf-8")) + 1}, xr_native_iterator_members, 5', '};']
    return output


def render_channel_identity(root):
    """Bind the complete source identity without granting unimplemented recipes."""
    source = (root / 'stdlib/types/channel.xr').read_text(encoding='utf-8').replace('\r\n', '\n')
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    bindings = re.findall(r'^\s*XR_BUILTIN_PRELUDE_TYPE\("Channel",\s*1,\s*XR_TCHANNEL,\s*GENERIC_1\)\s*$', prelude, re.M)
    registrations = re.findall(r'^\s*XR_BUILTIN_[A-Z_]+\(\s*"Channel"', prelude, re.M)
    if len(bindings) != 1 or len(registrations) != 1:
        raise ValueError('Channel registry needs one exact one-parameter binding')
    if re.search(r'@native-|@lowered', source):
        raise ValueError('Channel identity grants no native member operation')
    rows = [(number, raw) for number, raw in enumerate(source.splitlines(), 1)
            if raw.strip() and not raw.strip().startswith('//')]
    expected = ['enum Recv<T> {', 'Value { value: T },', 'Empty,', 'Timeout,', 'Closed,', '}',
                'enum SendResult {', 'Sent,', 'Full,', 'Timeout,', 'Closed,', '}',
                'class Channel<T> {', 'capacity: i64', 'isClosed: bool',
                'send(value: T)', 'recv() -> Recv<T>', 'recvOr(default: T) -> T',
                'trySend(value: T) -> SendResult', 'tryRecv() -> Recv<T>',
                'sendTimeout(value: T, timeout: i64) -> SendResult',
                'recvTimeout(timeout: i64) -> Recv<T>', 'close()', '}']
    if [raw.strip() for _, raw in rows] != expected:
        raise ValueError('Channel source differs from its complete governed declaration')
    output, facts = [], []
    for index, (number, raw) in enumerate(rows[13:-1]):
        text = raw.strip(); name = IDENT.match(text).group(); signature = text[len(name):]
        parser = TypeParser(signature); method = parser.peek() == '('
        parameters = parser.parameters() if method else []; result_text = '()'
        if not method:
            parser.take(':'); result_text = signature[1:].strip(); parser.type()
        elif parser.peek() == '->':
            parser.take('->'); result_text = signature.split('->', 1)[1].strip(); parser.type()
        if parser.peek() is not None: raise ValueError('trailing Channel member signature')
        if parameters:
            output.append(f'static const XrNativeParameter xr_native_channel_parameters_{index}[] = {{')
            for pname, _, spelling, optional, variadic in parameters:
                output.append('    {' + ', '.join([c_string(pname), c_string(spelling),
                    'XR_NATIVE_TERM_'+{'T':'ELEMENT','i64':'I64'}.get(spelling,'UNADMITTED'), str(optional).lower(), str(variadic).lower()]) + '},')
            output.append('};')
        facts.append((index, number, raw, name, signature, method, parameters, result_text))
    output.append('static const XrNativeMemberDeclaration xr_native_channel_members[] = {')
    for index, number, raw, name, signature, method, parameters, result_text in facts:
        params = f'xr_native_channel_parameters_{index}' if parameters else 'NULL'
        admitted = {'capacity': ('CHANNEL_CAPACITY', 'I64'),
                    'isClosed': ('CHANNEL_IS_CLOSED', 'BOOL'), 'close': ('CHANNEL_CLOSE', 'UNIT'),
                    'trySend': ('CHANNEL_TRY_SEND', 'SEND_RESULT'), 'tryRecv': ('CHANNEL_TRY_RECV', 'RECV_ELEMENT'),
                    'send': ('CHANNEL_SEND', 'UNIT'), 'recv': ('CHANNEL_RECV', 'RECV_ELEMENT'),
                    'recvOr': ('CHANNEL_RECV_OR', 'ELEMENT'),
                    'sendTimeout': ('CHANNEL_SEND_TIMEOUT', 'SEND_RESULT'),
                    'recvTimeout': ('CHANNEL_RECV_TIMEOUT', 'RECV_ELEMENT')}.get(name)
        output.append('    {' + ', '.join([str(index + 1), c_string(name), c_string(signature),
            c_string(result_text), str(number), str(len(raw[:raw.index(name)].encode('utf-8')) + 1),
            'XR_NATIVE_RECEIVER_READ', 'false', str(method).lower(), str(bool(admitted)).lower(), 'true',
            'XR_NATIVE_OPERATION_'+(admitted[0] if admitted else 'NONE'),
            ('XR_NATIVE_ALLOCATION_MAY_HEAP' if name in ('trySend','tryRecv','send','recv','recvOr','sendTimeout','recvTimeout') else 'XR_NATIVE_ALLOCATION_NO_HEAP') if admitted else 'XR_NATIVE_ALLOCATION_UNKNOWN',
            (c_string('allocation,retain,limit,numeric_range,host') if name in ('sendTimeout','recvTimeout') else
             c_string('allocation,retain,limit') if name in ('trySend','tryRecv','send','recv','recvOr') else c_string('limit')) if admitted else c_string(''),
            ('XR_NATIVE_RESULT_OWNERSHIP_UNIT' if admitted[1]=='UNIT' else 'XR_NATIVE_RESULT_OWNERSHIP_OWNED')
                if admitted else 'XR_NATIVE_RESULT_OWNERSHIP_UNKNOWN', params, str(len(parameters)),
            'XR_NATIVE_TERM_'+(admitted[1] if admitted else 'UNADMITTED')]) + '},')
    number, raw = rows[12]; digest = hashlib.sha256(source.encode('utf-8')).digest()
    output += ['};', 'static const char *const xr_native_channel_type_parameters[] = {"T"};',
        'static const XrNativeTypeDeclaration xr_native_channel = {',
        '    6, XR_NATIVE_DECLARATION_IDENTITY, "Channel", xr_native_channel_type_parameters, 1,',
        '    "stdlib/types/channel.xr", "xray-native:prelude/Channel", {{' +
        ', '.join(str(byte) for byte in digest) + '}},',
        f'    {number}, {len(raw[:raw.index("Channel")].encode("utf-8")) + 1}, xr_native_channel_members, 10', '};']
    return output


def channel_result_rows(root):
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8').replace('\r\n', '\n')
    rows = [('Ordering', 'ORDERING', [('Relaxed','NONE'),('Acquire','NONE'),('Release','NONE'),('AcquireRelease','NONE'),('SeqCst','NONE')], 4, 0),
            ('CryptoError', 'CRYPTO_ERROR', [('InvalidLength','NONE')], 0, 0),
            ('Recv', 'RECV', [('Value','TYPE_PARAM_0'),('Empty','NONE'),('Timeout','NONE'),('Closed','NONE')], 7, 1),
            ('SendResult', 'SEND_RESULT', [('Sent','NONE'),('Full','NONE'),('Timeout','NONE'),('Closed','NONE')], 8, 0),
            ('Endian', 'ENDIAN', [('Native','NONE'),('LE','NONE'),('BE','NONE')], 0, 0)]
    for name, kind, variants, _, arity in rows:
        registrations = re.findall(r'^\s*XR_BUILTIN_ENUM\(\s*"'+name+r'"', prelude, re.M)
        matches = list(re.finditer(r'XR_BUILTIN_ENUM\("'+name+r'",\s*'+str(arity)+r',\s*'+kind+r',', prelude))
        if len(registrations)!=1 or len(matches)!=1: raise ValueError('prelude needs one exact enum binding: '+name)
        tail=prelude[matches[0].end():];depth=1;end=0
        for end,ch in enumerate(tail):
            if ch=='(':depth+=1
            elif ch==')':
                depth-=1
                if not depth:break
        fixed=''.join('XR_BUILTIN_ENUM_VARIANT("'+variant+'",'+payload+')' for variant,payload in variants)
        if ''.join(tail[:end].split())!=fixed: raise ValueError('prelude enum inventory differs: '+name)
    return prelude, rows


def render_channel_result_identities(root):
    prelude, rows = channel_result_rows(root);digest=hashlib.sha256(prelude.encode('utf-8')).digest();lines=[]
    for name, _, variants, native_id, arity in rows:
        if native_id not in (7,8):continue
        variable='recv' if native_id==7 else 'send_result'
        line,raw=next((number,raw) for number,raw in enumerate(prelude.splitlines(),1) if 'XR_BUILTIN_ENUM("'+name+'",' in raw)
        if arity:
            lines += ['static const XrNativeParameter xr_native_recv_value_parameters[] = {',
                      '    {"value", "T", XR_NATIVE_TERM_ELEMENT, false, false}', '};']
        lines.append('static const XrNativeMemberDeclaration xr_native_'+variable+'_members[] = {')
        for index,(variant,payload) in enumerate(variants):
            member_line,member_raw=next((number,raw) for number,raw in enumerate(prelude.splitlines(),1)
                                       if 'XR_BUILTIN_ENUM_VARIANT("'+variant+'", '+payload+')' in raw and number>=line)
            count=1 if payload=='TYPE_PARAM_0' else 0
            lines.append('    {'+', '.join([str(index+1),c_string(variant),c_string(variant),c_string(name+('<T>' if arity else '')),
                str(member_line),str(member_raw.index(variant)+1),'XR_NATIVE_RECEIVER_READ','true','false','false','true',
                'XR_NATIVE_OPERATION_NONE','XR_NATIVE_ALLOCATION_UNKNOWN','""','XR_NATIVE_RESULT_OWNERSHIP_UNKNOWN',
                'xr_native_recv_value_parameters' if count else 'NULL',str(count),'XR_NATIVE_TERM_UNADMITTED'])+'},')
        lines.append('};')
        if arity: lines.append('static const char *const xr_native_'+variable+'_type_parameters[] = {"T"};')
        lines += ['static const XrNativeTypeDeclaration xr_native_'+variable+' = {',
                  '    '+str(native_id)+', XR_NATIVE_DECLARATION_VALUE, '+c_string(name)+', '+
                  ('xr_native_'+variable+'_type_parameters' if arity else 'NULL')+', '+str(arity)+',',
                  '    "stdlib/prelude/builtin_symbols.def", "xray-native:prelude/'+name+'", {{'+', '.join(str(byte) for byte in digest)+'}},',
                  '    '+str(line)+', '+str(raw.index(name)+1)+', xr_native_'+variable+'_members, '+str(len(variants)), '};']
    return lines


def render_prelude_source(root):
    """Project complete governed enum definitions through ordinary Source checking."""
    prelude,rows=channel_result_rows(root);digest=hashlib.sha256(prelude.encode('utf-8')).digest()
    text='// builtin_symbols.def sha256 '+digest.hex()+'\n'
    for name, _, variants, _, arity in rows:
        values=[variant+(' { value: T }' if payload=='TYPE_PARAM_0' else '') for variant,payload in variants]
        text+='export enum '+name+('<T>' if arity else '')+' { '+', '.join(values)+' }\n'
    builder_source=(root/'stdlib/types/stringbuilder.xr').read_text(encoding='utf-8').replace('\r\n','\n')
    parse_source(builder_source,prelude)
    builder_digest=hashlib.sha256(builder_source.encode('utf-8')).digest()
    text+='// stringbuilder.xr sha256 '+builder_digest.hex()+'\n'
    text+='export class StringBuilder {\n'
    text+='private _text:string\nprivate const _empty:string\n'
    text+='init(){this._text="";this._empty=""}\n'
    text+='append(value:string)->StringBuilder{this._text=this._text+value;return this}\n'
    text+='toString()->string{return this._text}\n'
    text+='clear()->StringBuilder{this._text=this._empty;return this}\n}\n'
    lines=['/* Generated by gen_native_declarations.py; do not edit. */']
    for index, (_, _, variants, _, _) in enumerate(rows):
        lines.append('static const char *const xir_prelude_variants_'+str(index)+'[] = {'+', '.join(c_string(v) for v,_ in variants)+'};')
    lines.append('static const XirPreludeEnum xir_prelude_enums[] = {')
    for index,(name,_,variants,native_id,arity) in enumerate(rows):
        field_count=sum(payload!='NONE' for _,payload in variants)
        lines.append('    {'+c_string(name)+', '+str(native_id)+', '+str(len(variants))+', '+str(arity)+', '+str(field_count)+', xir_prelude_variants_'+str(index)+'},')
    lines += ['};','static const XirPreludeSource xir_prelude_source = {','    '+c_string(text)+',',
              '    {'+', '.join(str(byte) for byte in digest)+'},','    xir_prelude_enums, '+str(len(rows))+',',
              '    {'+', '.join(str(byte) for byte in builder_digest)+'}', '};']
    return '\n'.join(lines)+'\n'


# Explicit IDs are independent of source member order and operation ordinals.
INTRINSICS = (
    (1, 'string', 'STRING_CONTAINS', 'READ', True, 'xr_xir_string_contains'),
    (2, 'string', 'STRING_STARTS_WITH', 'READ', True, 'xr_xir_string_starts_with'),
    (3, 'string', 'STRING_ENDS_WITH', 'READ', True, 'xr_xir_string_ends_with'),
)


def render_intrinsics(root):
    lines = ['static const XrNativeIntrinsicDeclaration xr_native_intrinsics[] = {']
    used = set()
    bindings = set()
    for identity, stem, operation, effects, ctfe, symbol in INTRINSICS:
        if not 0 < identity <= 0xffffffff or identity in used:
            raise ValueError('invalid or duplicate intrinsic ID')
        used.add(identity)
        _, _, members = render_type(root, stem)
        matches = [i for i, member in enumerate(members) if member.operation == operation]
        if len(matches) != 1:
            raise ValueError('intrinsic requires one canonical member: ' + operation)
        binding = (stem, matches[0])
        if binding in bindings:
            raise ValueError('duplicate intrinsic member binding')
        bindings.add(binding)
        if not re.fullmatch(r'[a-zA-Z_][a-zA-Z_0-9]*', symbol):
            raise ValueError('invalid intrinsic C symbol')
        lines.append(f'    {{UINT32_C({identity}), XR_NATIVE_INTRINSIC_{effects}, '
                     f'&xr_native_{stem}, &xr_native_{stem}_members[{matches[0]}], '
                     f'{str(ctfe).lower()}, XR_NATIVE_INTRINSIC_IMPLEMENTATION_{operation}, '
                     f'{c_string(symbol)}}},')
    return lines + ['};', '']


def render(root):
    lines = ['/* Generated by gen_native_declarations.py; do not edit. */']
    array_lines, header, members = render_type(root, "array")
    string_lines, _, _ = render_type(root, "string")
    integer_lines, _, _ = render_type(root, "i64")
    scalar_lines = []
    for stem in ('f64', 'bool', 'rune'):
        generated, _, _ = render_type(root, stem)
        scalar_lines += generated
    lines += array_lines + string_lines + integer_lines + scalar_lines + render_atomic_identities(root) + render_task_identity(root) + render_channel_identity(root) + render_channel_result_identities(root) + render_iterator_identity(root) + render_type(root, 'map')[0] + render_type(root, 'set')[0] + render_type(root, 'stringbuilder')[0]
    lines += render_intrinsics(root)
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
    return {'src/shared/xprelude_source.inc.c': render_prelude_source(root),
            'src/shared/xnative_declarations.inc.c': '\n'.join(lines),
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
