#!/usr/bin/env python3
"""Project governed native value declarations into shared compiler metadata."""
from dataclasses import dataclass, asdict
import argparse
import hashlib
import json
from pathlib import Path
import re


TYPE_MARKER = re.compile(r'^\s*//\s*@native-type\s+id=([1-9][0-9]*)\s*$', re.M)
OP_MARKER = re.compile(r'//\s*@native-operation\s+([A-Z_][A-Z_0-9]*)\s+allocation=(no_heap|may_heap)\s+'
                       r'failures=([a-z0-9_,]+)\s+ownership=(owned|unit)\s*$')
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
    error: str = 'Never'


def simple_term(term, binder):
    if term == ('name', 'Iterator', (('name', 'rune', ()),)):
        return 'ITERATOR_RUNE'
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
    if term == ('name', 'Array', (('name', 'string', ()),)):
        return 'ARRAY_STRING'
    if term == ('name', 'Array', (('name', 'f64', ()),)):
        return 'ARRAY_F64'
    if term == ('name', 'Slice', (('name', 'u8', ()),)):
        return 'SLICE_U8'
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
        'ARRAY_SORT': ('sort', 'REF', f'(compareFn?: fn(a: {binder}, b: {binder}) -> i64) -> Array<{binder}>', ordinary, 'owned'),
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
            scalar = (identity, name) in ((12, 'i64'), (13, 'f64'), (14, 'bool'), (15, 'rune')) and not parameters
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
            error = 'Never'
            if parser.peek() == 'throws':
                parser.take('throws'); parser.take('(')
                error = parser.take(); parser.take(')')
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
        member.error = error if method else 'Never'
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
                'RUNE_TO_STRING': ('toString', '() -> string', 'may_heap', 'allocation,retain,limit'),
                'RUNE_IS_WHITESPACE': ('isWhitespace', '() -> bool', 'no_heap', 'none')},
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
            'ARRAY_BYTE_APPEND': ('appendFrom', 'REF', ('SLICE_U8',), 'ARRAY_U8', 'may_heap', 'allocation,retain,limit', 'owned'),
            'ARRAY_BYTE_REPEAT': ('repeatFrom', 'REF', ('I64', 'I64'), 'ARRAY_U8', 'may_heap', 'bounds,allocation,retain,limit', 'owned'),
            'ARRAY_CAPACITY': ('capacity', 'READ', (), 'I64', 'no_heap', 'limit', 'owned'),
            'ARRAY_WITH_CAPACITY': ('withCapacity', 'READ', ('I64',), 'ARRAY_ELEMENT', 'may_heap', 'allocation,retain,limit', 'owned'),
            'ARRAY_RESERVE': ('reserve', 'REF', ('I64',), 'ARRAY_ELEMENT', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_INDEX_OF': ('indexOf', 'READ', ('STRING', 'I64'), 'I64', 'no_heap', 'bounds,limit', 'owned'),
            'STRING_LAST_INDEX_OF': ('lastIndexOf', 'READ', ('STRING',), 'I64', 'no_heap', 'limit', 'owned'),
            'STRING_CONTAINS': ('contains', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
            'STRING_STARTS_WITH': ('startsWith', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
            'STRING_ENDS_WITH': ('endsWith', 'READ', ('STRING',), 'BOOL', 'no_heap', 'none', 'owned'),
            'STRING_FROM_UTF8': ('fromUtf8', 'READ', ('SLICE_U8',), 'STRING', 'may_heap', 'utf8,allocation,retain,limit', 'owned'),
            'STRING_SLICE': ('slice', 'READ', ('I64', 'I64'), 'STRING', 'may_heap', 'bounds,allocation,retain,limit', 'owned'),
            'STRING_TO_STRING': ('toString', 'READ', (), 'STRING', 'no_heap', 'retain', 'owned'),
            'STRING_REPEAT': ('repeat', 'READ', ('I64',), 'STRING', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_COPY_BYTES': ('copyBytes', 'READ', (), 'ARRAY_U8', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_RUNES': ('runes', 'READ', (), 'ITERATOR_RUNE', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_SPLIT': ('split', 'READ', ('STRING',), 'ARRAY_STRING', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_REPLACE': ('replace', 'READ', ('STRING', 'STRING'), 'STRING', 'may_heap', 'allocation,retain,limit', 'owned'),
            'STRING_REPLACE_ALL': ('replaceAll', 'READ', ('STRING', 'STRING'), 'STRING', 'may_heap', 'allocation,retain,limit', 'owned'),
        }.get(member.operation)
        optional = tuple(p[3] for p in member.parameters)
        expected_optional = (False, True) if member.operation in ('STRING_INDEX_OF', 'STRING_SLICE') else (False,) * len(member.parameters)
        expected_static = member.operation in ('ARRAY_WITH_CAPACITY', 'STRING_FROM_UTF8')
        if member.error != ('Utf8Error' if member.operation == 'STRING_FROM_UTF8' else
                            'StringPatternError' if member.operation == 'STRING_SPLIT' else 'Never'):
            raise ValueError('native operation error type disagrees with its declaration')
        expected_method = member.operation != 'ARRAY_CAPACITY'
        if shape != expected or member.static != expected_static or member.method != expected_method or optional != expected_optional or any(p[4] for p in member.parameters):
            raise ValueError('operation declaration disagrees with its semantic contract: ' + member.operation)
    required = {'ARRAY_GET', 'ARRAY_SET', 'ARRAY_PUSH', 'ARRAY_CAPACITY', 'ARRAY_WITH_CAPACITY', 'ARRAY_RESERVE', 'ARRAY_BYTE_APPEND', 'ARRAY_BYTE_REPEAT'} | set(recipes) if header[0] == 1 else {
        'STRING_CONTAINS', 'STRING_STARTS_WITH', 'STRING_ENDS_WITH', 'STRING_INDEX_OF', 'STRING_LAST_INDEX_OF',
        'STRING_TO_STRING', 'STRING_REPEAT', 'STRING_COPY_BYTES', 'STRING_REPLACE', 'STRING_REPLACE_ALL', 'STRING_SLICE', 'STRING_FROM_UTF8', 'STRING_RUNES', 'STRING_SPLIT'}
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
                                     ('bool', 14, 'bool'), ('rune', 15, 'rune')):
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
    """Bind the governed handle and its single admitted cancellation recipe."""
    source = (root / 'stdlib/types/coroutine.xr').read_text(encoding='utf-8').replace('\r\n', '\n')
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    bindings = re.findall(r'^\s*XR_BUILTIN_TYPE\("Task",\s*([0-9]+)\)\s*$', prelude, re.M)
    registrations = re.findall(r'XR_BUILTIN_[A-Z_]+\(\s*"Task"', prelude)
    if bindings != ['2'] or len(registrations) != 1:
        raise ValueError('Task registry needs one exact value-and-error binding')
    if re.search(r'@native-|@lowered', source):
        raise ValueError('Task declaration grants no native member operation')
    rows = [(number, raw) for number, raw in enumerate(source.splitlines(), 1)
            if raw.strip() and not raw.strip().startswith('//')]
    expected = [
        'enum TaskResult<T, E: Error> {', 'Success { value: T },', 'Error { error: E },',
        'Panic { info: PanicInfo },', 'Cancelled,', '}',
        'class Task<T: Sendable, E: Error & Sendable = Never> {', 'ref cancel()',
        'ref poll() -> TaskResult<T, E>?', 'ref awaitResult() -> TaskResult<T, E>', '}',
    ]
    expected[-1:-1] = [
        'private static __allBegin(tasks: Array<Task<T, E>>) -> Task<Array<T>, E>',
        'private static __firstBegin(tasks: Array<Task<T, E>>) -> Task<T, E>',
        'static all(tasks: Array<Task<T, E>>) throws(E) -> Array<T>',
        'static first(tasks: Array<Task<T, E>>) throws(E) -> T',
        'static timeout(task: Task<T, E>, milliseconds: i64) throws(E | Timeout) -> T',
    ]
    if [raw.strip() for _, raw in rows] != expected:
        raise ValueError('Task source differs from its complete governed declaration')
    output = []
    facts = []
    for index, (number, raw) in enumerate(rows[7:-1]):
        text = raw.strip()
        private = text.startswith('private static ')
        if private:
            text = text[len('private static '):]
        static = private or text.startswith('static ')
        if not private and static:
            text = text[len('static '):]
        receiver = 'REF' if text.startswith('ref ') else 'READ'
        if receiver == 'REF':
            text = text[4:]
        name = IDENT.match(text).group()
        signature = text[len(name):]
        parser = TypeParser(signature)
        method = parser.peek() == '('
        parameters = parser.parameters() if method else []
        if parser.peek() == 'throws':
            parser.take('throws')
            parser.take('(')
            parser.type()
            parser.take(')')
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
                    ('XR_NATIVE_TERM_I64' if spelling == 'i64' else
                     'XR_NATIVE_TERM_TASK_ELEMENT' if name == 'timeout' else
                     'XR_NATIVE_TERM_ARRAY_TASK') if static else 'XR_NATIVE_TERM_UNADMITTED',
                    str(optional).lower(), str(variadic).lower()]) + '},')
            output.append('};')
        facts.append((index, number, raw, name, signature, receiver, method, parameters, result_text, private, static))
    output.append('static const XrNativeMemberDeclaration xr_native_task_members[] = {')
    for index, number, raw, name, signature, receiver, method, parameters, result_text, private, static in facts:
        params = f'xr_native_task_parameters_{index}' if parameters else 'NULL'
        output.append('    {' + ', '.join([str(index + 1), c_string(name), c_string(signature),
            c_string(result_text), str(number), str(len(raw[:raw.index(name)].encode('utf-8')) + 1),
            'XR_NATIVE_RECEIVER_' + receiver, str(static).lower(), str(method).lower(),
            str(not private).lower(), str(not private).lower(),
            'XR_NATIVE_OPERATION_'+{'cancel':'TASK_CANCEL', 'poll':'TASK_POLL', 'awaitResult':'TASK_AWAIT_RESULT',
                'all':'TASK_ALL','first':'TASK_FIRST','timeout':'TASK_TIMEOUT'}.get(name,'NONE'),
            'XR_NATIVE_ALLOCATION_NO_HEAP' if name == 'cancel' else 'XR_NATIVE_ALLOCATION_MAY_HEAP',
            '"Never"' if name == 'cancel' else '"allocation,retain,limit,unsupported"' if private else '"allocation,retain,limit"',
            'XR_NATIVE_RESULT_OWNERSHIP_UNIT' if name == 'cancel' else 'XR_NATIVE_RESULT_OWNERSHIP_OWNED',
            params, str(len(parameters)),
            'XR_NATIVE_TERM_'+{'cancel':'UNIT', 'poll':'NULLABLE_TASK_RESULT', 'awaitResult':'TASK_RESULT',
                '__allBegin':'TASK_ARRAY','__firstBegin':'TASK_ELEMENT','all':'ARRAY_ELEMENT','first':'ELEMENT','timeout':'ELEMENT'}[name]]) + '},')
    number, raw = rows[6]
    digest = hashlib.sha256(source.encode('utf-8')).digest()
    output += ['};', 'static const char *const xr_native_task_type_parameters[] = {"T", "E"};',
        'static const XrNativeTypeDeclaration xr_native_task = {',
        '    5, XR_NATIVE_DECLARATION_IDENTITY, "Task", xr_native_task_type_parameters, 2,',
        '    "stdlib/types/coroutine.xr", "xray-native:prelude/Task", {{' +
        ', '.join(str(byte) for byte in digest) + '}},',
        f'    {number}, {len(raw[:raw.index("Task")].encode("utf-8")) + 1}, xr_native_task_members, {len(facts)}', '};']
    return output



def render_iterator_identity(root):
    """Bind the complete declaration and its compiler-owned pull recipes."""
    source = (root / 'stdlib/types/iterator.xr').read_text(encoding='utf-8').replace('\r\n', '\n')
    prelude = (root / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    binding = re.findall(r'XR_BUILTIN_PRELUDE_TYPE\("Iterator",\s*2,\s*XR_TINSTANCE,\s*GENERIC_2\)', prelude)
    registrations = re.findall(r'XR_BUILTIN_[A-Z_]+\(\s*"Iterator"', prelude)
    if len(binding) != 1 or len(registrations) != 1:
        raise ValueError('Iterator registry needs one exact value-and-error binding')
    if re.search(r'@native-', source):
        raise ValueError('Iterator identity grants no native member operation')
    rows = [(number, raw) for number, raw in enumerate(source.splitlines(), 1)
            if raw.strip() and not raw.strip().startswith('//')]
    expected = ['class Iterator<T, E: Error = Never> {', 'hasNext() throws(E) -> bool', 'next() throws(E) -> T',
                'nth(index: i64) throws(E) -> T', 'iterator() -> Iterator<T, E>', 'toString() -> string', '}']
    if [raw.strip() for _, raw in rows] != expected:
        raise ValueError('Iterator source differs from its complete governed declaration')
    output, facts = [], []
    for index, (number, raw) in enumerate(rows[1:-1]):
        text = raw.strip()
        name = IDENT.match(text).group()
        signature = text[len(name):]
        parser = TypeParser(signature)
        parameters = parser.parameters()
        if parser.peek() == 'throws':
            parser.take('throws'); parser.take('('); parser.take('E'); parser.take(')')
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
    output += ['};', 'static const char *const xr_native_iterator_type_parameters[] = {"T", "E"};',
        'static const XrNativeTypeDeclaration xr_native_iterator = {',
        '    9, XR_NATIVE_DECLARATION_IDENTITY, "Iterator", xr_native_iterator_type_parameters, 2,',
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
        if method and parser.peek() == 'throws':
            parser.take('throws'); parser.take('('); parser.take('SendResult'); parser.take(')')
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
    rows = [('Utf8Error', 'UTF8_ERROR', [('InvalidUtf8','NONE')], 17, 0),
            ('Ordering', 'ORDERING', [('Relaxed','NONE'),('Acquire','NONE'),('Release','NONE'),('AcquireRelease','NONE'),('SeqCst','NONE')], 4, 0),
            ('CryptoError', 'CRYPTO_ERROR', [('InvalidLength','NONE')], 0, 0),
            ('StringPatternError', 'STRING_PATTERN_ERROR', [('EmptyPattern','NONE')], 0, 0),
            ('Recv', 'RECV', [('Value','TYPE_PARAM_0'),('Empty','NONE'),('Timeout','NONE'),('Closed','NONE')], 7, 1),
            ('SendResult', 'SEND_RESULT', [('Sent','NONE'),('Full','NONE'),('Timeout','NONE'),('Closed','NONE')], 8, 0),
            ('Endian', 'ENDIAN', [('Native','NONE'),('LE','NONE'),('BE','NONE')], 0, 0),
            ('Cancelled', 'CANCELLED', [('Cancelled','NONE')], 18, 0),
            ('Timeout', 'TASK_TIMEOUT', [('Timeout','NONE')], 0, 0),
            ('TaskResult', 'TASK_RESULT', [('Success','TYPE_PARAM_0'),('Error','ERROR'),('Panic','PANIC_INFO'),('Cancelled','NONE')], 19, 2),
            ('CompressionError', 'COMPRESSION_ERROR', [('InvalidData','NONE')], 0, 0)]
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
        if native_id not in (7,8,17,18,19):continue
        variable={7:'recv', 8:'send_result', 17:'utf8_error', 18:'cancelled', 19:'task_result'}[native_id]
        line,raw=next((number,raw) for number,raw in enumerate(prelude.splitlines(),1) if 'XR_BUILTIN_ENUM("'+name+'",' in raw)
        if native_id==19:
            for index,(field,spelling,term) in enumerate((('value','T','ELEMENT'),('error','E','UNADMITTED'),('info','PanicInfo','UNADMITTED'))):
                lines += ['static const XrNativeParameter xr_native_task_result_parameters_'+str(index)+'[] = {',
                          '    {'+c_string(field)+', '+c_string(spelling)+', XR_NATIVE_TERM_'+term+', false, false}', '};']
        elif arity:
            lines += ['static const XrNativeParameter xr_native_recv_value_parameters[] = {',
                      '    {"value", "T", XR_NATIVE_TERM_ELEMENT, false, false}', '};']
        lines.append('static const XrNativeMemberDeclaration xr_native_'+variable+'_members[] = {')
        for index,(variant,payload) in enumerate(variants):
            member_line,member_raw=next((number,raw) for number,raw in enumerate(prelude.splitlines(),1)
                                       if 'XR_BUILTIN_ENUM_VARIANT("'+variant+'", '+payload+')' in raw and number>=line)
            count=1 if payload!='NONE' else 0
            parameters=('xr_native_task_result_parameters_'+str(index) if native_id==19 else 'xr_native_recv_value_parameters') if count else 'NULL'
            suffix='<T, E>' if arity==2 else '<T>' if arity else ''
            lines.append('    {'+', '.join([str(index+1),c_string(variant),c_string(variant),c_string(name+suffix),
                str(member_line),str(member_raw.index(variant)+1),'XR_NATIVE_RECEIVER_READ','true','false','false','true',
                'XR_NATIVE_OPERATION_NONE','XR_NATIVE_ALLOCATION_UNKNOWN','""','XR_NATIVE_RESULT_OWNERSHIP_UNKNOWN',
                parameters,str(count),'XR_NATIVE_TERM_UNADMITTED'])+'},')
        lines.append('};')
        if arity: lines.append('static const char *const xr_native_'+variable+'_type_parameters[] = {'+('"T", "E"' if arity==2 else '"T"')+'};')
        lines += ['static const XrNativeTypeDeclaration xr_native_'+variable+' = {',
                  '    '+str(native_id)+', XR_NATIVE_DECLARATION_VALUE, '+c_string(name)+', '+
                  ('xr_native_'+variable+'_type_parameters' if arity else 'NULL')+', '+str(arity)+',',
                  '    "stdlib/prelude/builtin_symbols.def", "xray-native:prelude/'+name+'", {{'+', '.join(str(byte) for byte in digest)+'}},',
                  '    '+str(line)+', '+str(raw.index(name)+1)+', xr_native_'+variable+'_members, '+str(len(variants)), '};']
    return lines


def prelude_declaration_rows(root):
    text = (root/'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    rows = re.findall(r'XR_BUILTIN_PRELUDE_DECLARATION\(\s*"([A-Za-z_][A-Za-z_0-9]*)"\s*,\s*"([A-Za-z_][A-Za-z_0-9]*)"\s*\)', text)
    names = [name for name, _ in rows]
    if len(names) != len(set(names)):
        raise ValueError('duplicate builtin prelude declaration')
    if names.count('Path') != 1:
        raise ValueError('Path requires one builtin prelude declaration route')
    return rows


def render_prelude_source(root):
    """Project complete governed enum definitions through ordinary Source checking."""
    prelude,rows=channel_result_rows(root);digest=hashlib.sha256(prelude.encode('utf-8')).digest()
    text='// builtin_symbols.def sha256 '+digest.hex()+'\n'
    for name, _, variants, _, arity in rows:
        payloads={'TYPE_PARAM_0':' { value: T }', 'ERROR':' { error: E }', 'PANIC_INFO':' { info: PanicInfo }', 'NONE':''}
        values=[variant+payloads[payload] for variant,payload in variants]
        suffix='<T, E: Error>' if arity==2 else '<T>' if arity else ''
        text+='export enum '+name+suffix+' { '+', '.join(values)+' }\n'
    range_source=(root/'stdlib/prelude/range.xr').read_text(encoding='utf-8').replace('\r\n','\n')
    range_digest=hashlib.sha256(range_source.encode('utf-8')).digest()
    text+='// range.xr sha256 '+range_digest.hex()+'\n'+range_source
    stringable_source=(root/'stdlib/prelude/stringable.xr').read_text(encoding='utf-8').replace('\r\n','\n')
    text+='// stringable.xr sha256 '+hashlib.sha256(stringable_source.encode('utf-8')).hexdigest()+'\n'+stringable_source
    lines=['/* Generated by gen_native_declarations.py; do not edit. */']
    for index, (_, _, variants, _, _) in enumerate(rows):
        lines.append('static const char *const xir_prelude_variants_'+str(index)+'[] = {'+', '.join(c_string(v) for v,_ in variants)+'};')
    lines.append('static const XirPreludeEnum xir_prelude_enums[] = {')
    for index,(name,_,variants,native_id,arity) in enumerate(rows):
        field_count=sum(payload!='NONE' for _,payload in variants)
        lines.append('    {'+c_string(name)+', '+str(native_id)+', '+str(len(variants))+', '+str(arity)+', '+str(field_count)+', xir_prelude_variants_'+str(index)+'},')
    lines += ['};','static const XirPreludeSource xir_prelude_source = {','    '+c_string(text)+',',
              '    {'+', '.join(str(byte) for byte in digest)+'},','    xir_prelude_enums, '+str(len(rows))+',',
              '    {'+', '.join(str(byte) for byte in range_digest)+'}', '};']
    lines.append('static inline const char *xir_prelude_declaration_module(const char *name) {')
    lines.append('    if (!name) return NULL;')
    for name, module in prelude_declaration_rows(root):
        lines.append('    if (!strcmp(name,'+c_string(name)+')) return '+c_string(module)+';')
    lines += ['    return NULL;', '}']
    return '\n'.join(lines)+'\n'


# Explicit IDs are independent of source member order and operation ordinals.
INTRINSICS = (
    (1, 'string', 'STRING_CONTAINS', 'READ', True, 'xr_xir_string_contains'),
    (2, 'string', 'STRING_STARTS_WITH', 'READ', True, 'xr_xir_string_starts_with'),
    (3, 'string', 'STRING_ENDS_WITH', 'READ', True, 'xr_xir_string_ends_with'),
    (4, 'string', 'STRING_SLICE', 'READ', True, 'xr_xir_string_slice'),
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


MATH_CALLABLE_CONTRACTS = (
    (27, '__abs', '(x: f64): f64'),
    (28, '__floor', '(x: f64): i64'),
    (29, '__ceil', '(x: f64): i64'),
    (30, '__round', '(x: f64): i64'),
    (31, '__sqrt', '(x: f64): f64'),
    (32, '__pow', '(base: f64, exp: f64): f64'),
    (33, '__sin', '(x: f64): f64'),
    (34, '__cos', '(x: f64): f64'),
    (35, '__tan', '(x: f64): f64'),
    (36, '__asin', '(x: f64): f64'),
    (37, '__acos', '(x: f64): f64'),
    (38, '__atan', '(x: f64): f64'),
    (39, '__atan2', '(y: f64, x: f64): f64'),
    (40, '__log', '(x: f64): f64'),
    (41, '__log10', '(x: f64): f64'),
    (42, '__log2', '(x: f64): f64'),
    (43, '__exp', '(x: f64): f64'),
    (44, '__random', '(): f64'),
    (45, '__randomInt', '(min: i64, max: i64): i64'),
    (46, '__sinh', '(x: f64): f64'),
    (47, '__cosh', '(x: f64): f64'),
    (48, '__tanh', '(x: f64): f64'),
    (49, '__hypot', '(x: f64, y: f64): f64'),
    (50, '__cbrt', '(x: f64): f64'),
    (51, '__trunc', '(x: f64): i64'),
    (52, '__fmod', '(x: f64, y: f64): f64'),
    (53, '__log1p', '(x: f64): f64'),
    (54, '__expm1', '(x: f64): f64'),
)


def render_math_callables(declarations):
    lines, rows = [], []
    for ordinal, (identity, name, signature) in enumerate(MATH_CALLABLE_CONTRACTS):
        candidates = [row for row in declarations if row.module == 'math' and row.name == name]
        if len(candidates) != 1:
            raise ValueError('Math callable requires its sole governed declaration: ' + name)
        row = candidates[0]
        arguments, result = signature.rsplit(': ', 1)
        params = [part.split(': ', 1) for part in arguments[1:-1].split(', ')] if arguments != '()' else []
        pure = name not in ('__random', '__randomInt')
        implementation = 'math_' + name[2:]
        expected_aot = 'builtin' if pure else 'xrt_math_random_int' if name == '__randomInt' else 'xrt_math_random'
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
                signature, str(len(params)), 'vv' if name == '__randomInt' else '',
                'internal', 'nothrow', '', '', 'core' if pure else 'system', implementation,
                expected_aot, True, 'builtin' if pure else 'method', '' if pure else 'math.' + name,
                'normal', '', '', 'value', False, None, (), ''):
            raise ValueError('Math callable differs from its complete source contract: ' + name)
        symbol = 'xr_native_math_' + str(ordinal)
        if params:
            lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {']
            lines += ['    {' + c_string(n) + ', ' + c_string(t) + ', XR_NATIVE_TERM_' + t.upper() + ', false, false},' for n, t in params]
            lines += ['};']
        lines += ['static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(ordinal + 1) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' + c_string(result) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + (symbol + '_parameters' if params else 'NULL') + ', ' + str(len(params)) + ', XR_NATIVE_TERM_' + result.upper(), '};']
        rows += ['    {' + str(identity) + ', NULL, &' + symbol + ', NULL, ' + str(pure).lower() + ', "math", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows


def render_sys_thread_yield(declarations):
    candidates = [row for row in declarations if row.module == 'sys' and row.name == '__threadYield']
    if len(candidates) != 1:
        raise ValueError('Thread yield requires its sole governed declaration')
    row = candidates[0]
    if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
            row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
            row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
            row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
            '(): ()', '0', '', 'internal', '', '', '', 'system', 'sys_thread_yield',
            'xrt_sys_thread_yield', True, 'method', '', 'normal', '', '', 'value', False, None, (), ''):
        raise ValueError('Thread yield differs from its complete source contract')
    return [
        'static const XrNativeMemberDeclaration xr_native_sys_thread_yield = {',
        '    1, "__threadYield", "(): ()", "()", 0, 0, XR_NATIVE_RECEIVER_READ,',
        '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
        '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, NULL, 0, XR_NATIVE_TERM_UNIT',
        '};'], [
        '    {XR_NATIVE_CALLABLE_SYS_THREAD_YIELD, NULL, &xr_native_sys_thread_yield, NULL, false, "sys", "stdlib/defs/core.def", FINGERPRINT}']


def render_sys_signal(declarations):
    candidates = [row for row in declarations if row.module == 'sys' and row.name == '__onSignal']
    if len(candidates) != 1:
        raise ValueError('Signal registration requires its sole governed declaration')
    row = candidates[0]
    if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
            row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
            row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
            row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
            '(signal: i64, handler: fn(): ()): bool', '2', 'vv', 'internal', '', '', '',
            'system', 'sys_on_signal', 'xrt_sys_on_signal', True, 'method', '', 'normal', '', '',
            'value', False, None, (), ''):
        raise ValueError('Signal registration differs from its complete source contract')
    return [
        'static const XrNativeParameter xr_native_sys_on_signal_parameters[] = {',
        '    {"signal", "i64", XR_NATIVE_TERM_I64, false, false},',
        '    {"handler", "fn(): ()", XR_NATIVE_TERM_UNIT_CALLBACK, false, false},',
        '};',
        'static const XrNativeMemberDeclaration xr_native_sys_on_signal = {',
        '    3, "__onSignal", "(signal: i64, handler: fn(): ()): bool", "bool", 0, 0, XR_NATIVE_RECEIVER_READ,',
        '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
        '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, xr_native_sys_on_signal_parameters, 2, XR_NATIVE_TERM_BOOL',
        '};'], [
        '    {XR_NATIVE_CALLABLE_SYS_ON_SIGNAL, NULL, &xr_native_sys_on_signal, NULL, false, "sys", "stdlib/defs/core.def", FINGERPRINT}']


def render_sys_thread_tokens(declarations):
    contracts = (
        ('__threadLocalId', '(): i64', '', 'sys_thread_local_id', 'xrt_sys_thread_local_id', 'SYS_THREAD_LOCAL_ID', 'I64', 4),
        ('__threadLocalAlive', '(id: i64): bool', 'v', 'sys_thread_local_alive', 'xrt_sys_thread_local_alive', 'SYS_THREAD_LOCAL_ALIVE', 'BOOL', 5),
    )
    lines, rows = [], []
    for name, signature, arguments, vm, aot, identity, result, ordinal in contracts:
        candidates = [row for row in declarations if row.module == 'sys' and row.name == name]
        if len(candidates) != 1:
            raise ValueError('Thread token callable requires its sole governed declaration: ' + name)
        row = candidates[0]
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
                signature, str(len(arguments)), arguments, 'internal', '', '', '', 'system', vm,
                aot, True, 'method', '', 'normal', '', '', 'value', False, None, (), ''):
            raise ValueError('Thread token callable differs from its complete source contract: ' + name)
        symbol = 'xr_native_' + identity.lower()
        if arguments:
            lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {',
                      '    {"id", "i64", XR_NATIVE_TERM_I64, false, false},', '};']
        lines += ['static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(ordinal) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' +
            c_string('bool' if arguments else 'i64') + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + (symbol + '_parameters' if arguments else 'NULL') +
            ', ' + str(len(arguments)) + ', XR_NATIVE_TERM_' + result, '};']
        rows += ['    {XR_NATIVE_CALLABLE_' + identity + ', NULL, &' + symbol + ', NULL, false, "sys", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows


def render_sys_pipes(declarations):
    contracts = (
        ('__pipeOpen', '(): (i64, i64)?', 'SYS_PIPE_OPEN', 'NULLABLE_I64_PAIR', '(i64, i64)?', (), 'sys_pipe_open'),
        ('__pipeRead', '(handle: i64, maxBytes: i64): Array<u8>?', 'SYS_PIPE_READ', 'NULLABLE_ARRAY_U8', 'Array<u8>?', (('handle', 'i64', 'I64'), ('maxBytes', 'i64', 'I64')), 'sys_pipe_read'),
        ('__pipeWrite', '(handle: i64, data: Array<u8>): i64', 'SYS_PIPE_WRITE', 'I64', 'i64', (('handle', 'i64', 'I64'), ('data', 'Array<u8>', 'ARRAY_U8')), 'sys_pipe_write'),
        ('__pipeClose', '(handle: i64): bool', 'SYS_PIPE_CLOSE', 'BOOL', 'bool', (('handle', 'i64', 'I64'),), 'sys_pipe_close'),
    )
    expected_provider = {
        'contract': 'xray.runtime.provider.v1/io',
        'operation': 'xray.runtime.provider-operation.v1/io/pipe-open',
        'logical': {'parameters': (), 'result_type': '(i64, i64)?', 'result_owner': 'trivial',
            'error': 'none', 'panic': 'none', 'suspend': 'never', 'refusal': 'trap',
            'resources': tuple({'resource': 'xray.runtime.resource.v1/pipe-endpoint',
                'value': 'result.some.' + str(i), 'action': 'acquire', 'when': 'result-present'} for i in range(2)),
            'effects': ('io',), 'threads': 'any', 'reentry': 'allowed', 'callbacks': 'none',
            'platforms': ('linux', 'macos', 'windows'), 'profiles': ('hosted',)},
        'host': {'adapter': 'optional-i64-pair-pipe-create', 'header': 'os/os_pipe.h', 'symbol': 'xr_pipe_create'}}
    expected_close = {
        'contract': 'xray.runtime.provider.v1/io',
        'operation': 'xray.runtime.provider-operation.v1/io/pipe-close',
        'logical': {'parameters': ({'type': 'i64', 'mode': 'in', 'owner': 'trivial'},),
            'result_type': 'bool', 'result_owner': 'trivial', 'error': 'none', 'panic': 'none',
            'suspend': 'never', 'refusal': 'trap',
            'resources': ({'resource': 'xray.runtime.resource.v1/pipe-endpoint',
                'value': 'parameter.0', 'action': 'consume', 'when': 'result-true'},),
            'effects': ('io',), 'threads': 'any', 'reentry': 'allowed', 'callbacks': 'none',
            'platforms': ('linux', 'macos', 'windows'), 'profiles': ('hosted',)},
        'host': {'adapter': 'bool-i64-pipe-close', 'header': 'os/os_pipe.h', 'symbol': 'xr_pipe_close'}}
    lines, rows = [], []
    for at, (name, signature, identity, term, result, parameters, vm) in enumerate(contracts):
        candidates = [r for r in declarations if r.module == 'sys' and r.name == name]
        if len(candidates) != 1:
            raise ValueError('Pipe callable requires its sole governed declaration: ' + name)
        row = candidates[0]
        suspend = at in (1, 2)
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.caps, row.suspension_kind) != (
                signature, str(len(parameters)), 'v' * len(parameters), 'internal', 'nothrow' if at in (0, 3) else '', '',
                'fresh' if at == 1 else '', 'system', vm + ('_yieldable' if suspend else ''),
                'xrt_' + vm, True, 'method', '', 'yieldable' if suspend else 'normal', '', '', 'value', False, (), ''):
            raise ValueError('Pipe callable differs from its complete source contract: ' + name)
        try:
            provider = asdict(row.provider_declaration) if row.provider_declaration else None
        except TypeError as error:
            raise ValueError('Malformed pipe provider: ' + name) from error
        if provider != (expected_provider if at == 0 else expected_close if at == 3 else None):
            raise ValueError('Pipe provider differs from its complete source contract: ' + name)
        symbol = 'xr_native_' + identity.lower()
        if parameters:
            lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {']
            lines += ['    {' + c_string(n) + ', ' + c_string(t) + ', XR_NATIVE_TERM_' + k + ', false, false},' for n, t, k in parameters]
            lines += ['};']
        lines += ['static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(at + 6) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' + c_string(result) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + (symbol + '_parameters' if parameters else 'NULL') + ', ' + str(len(parameters)) + ', XR_NATIVE_TERM_' + term, '};']
        rows += ['    {XR_NATIVE_CALLABLE_' + identity + ', NULL, &' + symbol + ', NULL, false, "sys", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows


def render_sys_processes(declarations):
    contracts = (
        ('__processSpawn', '(program: string, args: Array<string>, cwd: string?, envKeys: Array<string>?, envValues: Array<string>?, stdinRead: i64?, stdoutWrite: i64?, stderrWrite: i64?, detached: bool): i64', 'SYS_PROCESS_SPAWN', 'I64', 'i64', (('program', 'string', 'STRING'), ('args', 'Array<string>', 'ARRAY_STRING'), ('cwd', 'string?', 'NULLABLE_STRING'), ('envKeys', 'Array<string>?', 'NULLABLE_ARRAY_STRING'), ('envValues', 'Array<string>?', 'NULLABLE_ARRAY_STRING'), ('stdinRead', 'i64?', 'NULLABLE_I64'), ('stdoutWrite', 'i64?', 'NULLABLE_I64'), ('stderrWrite', 'i64?', 'NULLABLE_I64'), ('detached', 'bool', 'BOOL')), 'sys_process_spawn'),
        ('__processTryWait', '(id: i64): i64?', 'SYS_PROCESS_TRY_WAIT', 'NULLABLE_I64', 'i64?', (('id', 'i64', 'I64'),), 'sys_process_try_wait'),
        ('__processKill', '(id: i64, signal: i64): bool', 'SYS_PROCESS_KILL', 'BOOL', 'bool', (('id', 'i64', 'I64'), ('signal', 'i64', 'I64')), 'sys_process_kill'),
    )
    lines, rows = [], []
    for at, (name, signature, identity, term, result, parameters, vm) in enumerate(contracts):
        candidates = [r for r in declarations if r.module == 'sys' and r.name == name]
        if len(candidates) != 1:
            raise ValueError('Process callable requires its sole governed declaration: ' + name)
        row = candidates[0]
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
                signature, str(len(parameters)), 'svvvvvvvv' if at == 0 else 'v' * len(parameters),
                'internal', '', '', '', 'system', vm, 'xrt_' + vm, True, 'method', '', 'normal', '', '',
                'value', False, None, (), ''):
            raise ValueError('Process callable differs from its complete source contract: ' + name)
        symbol = 'xr_native_' + identity.lower()
        lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {']
        lines += ['    {' + c_string(n) + ', ' + c_string(t) + ', XR_NATIVE_TERM_' + k + ', false, false},' for n, t, k in parameters]
        lines += ['};', 'static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(at + 10) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' + c_string(result) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + symbol + '_parameters, ' + str(len(parameters)) + ', XR_NATIVE_TERM_' + term, '};']
        rows += ['    {XR_NATIVE_CALLABLE_' + identity + ', NULL, &' + symbol + ', NULL, false, "sys", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows


def render_sys_dylibs(declarations):
    contracts = (
        ('__dylibOpen', '(path: Path): i64', 'SYS_DYLIB_OPEN', 'I64', 'i64', (('path', 'Path', 'PATH'),), 'sys_dylib_open'),
        ('__dylibSymbol', '(handle: i64, name: string): Ptr<u8>?', 'SYS_DYLIB_SYMBOL', 'NULLABLE_PTR_U8', 'Ptr<u8>?', (('handle', 'i64', 'I64'), ('name', 'string', 'STRING')), 'sys_dylib_symbol'),
        ('__dylibClose', '(handle: i64): bool', 'SYS_DYLIB_CLOSE', 'BOOL', 'bool', (('handle', 'i64', 'I64'),), 'sys_dylib_close'),
        ('__dylibLastError', '(): string', 'SYS_DYLIB_LAST_ERROR', 'STRING', 'string', (), 'sys_dylib_last_error'),
    )
    lines, rows = [], []
    for at, (name, signature, identity, term, result, parameters, vm) in enumerate(contracts):
        candidates = [r for r in declarations if r.module == 'sys' and r.name == name]
        if len(candidates) != 1:
            raise ValueError('Dylib callable requires its sole governed declaration: ' + name)
        row = candidates[0]
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
                signature, str(len(parameters)), ('p', 'vs', 'v', '')[at],
                'internal', '', '', 'fresh' if at == 3 else '', 'system', vm, 'xrt_' + vm, True, 'method', '', 'normal', '', '',
                'value', False, None, (), ''):
            raise ValueError('Dylib callable differs from its complete source contract: ' + name)
        symbol = 'xr_native_' + identity.lower()
        if parameters:
            lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {']
            lines += ['    {' + c_string(n) + ', ' + c_string(t) + ', XR_NATIVE_TERM_' + k + ', false, false},' for n, t, k in parameters]
            lines += ['};']
        lines += ['static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(at + 13) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' + c_string(result) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + (symbol + '_parameters' if parameters else 'NULL') + ', ' + str(len(parameters)) + ', XR_NATIVE_TERM_' + term, '};']
        rows += ['    {XR_NATIVE_CALLABLE_' + identity + ', NULL, &' + symbol + ', NULL, false, "sys", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows


def render_cpu_callables(declarations):
    contracts = (
        ('os', '__cpuCount', '(): i64', 'os_cpuCount', 'xrt_os_cpu_count', 'OS_CPU_COUNT', 'I64', 13),
        ('sys', '__pinToCpu', '(cpu: i64): bool', 'sys_pin_to_cpu', 'xrt_sys_pin_to_cpu', 'SYS_PIN_TO_CPU', 'BOOL', 2),
    )
    lines, rows = [], []
    for module, name, signature, vm, aot, identity, result, ordinal in contracts:
        candidates = [row for row in declarations if row.module == module and row.name == name]
        if len(candidates) != 1:
            raise ValueError('CPU callable requires its sole governed declaration: ' + name)
        row = candidates[0]
        count = 1 if module == 'sys' else 0
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
                signature, str(count), 'v' if count else '', 'internal', '', '', '', 'system', vm,
                aot, True, 'method', '', 'normal', '', '', 'value', False, None, (), ''):
            raise ValueError('CPU callable differs from its complete source contract: ' + name)
        symbol = 'xr_native_' + identity.lower()
        if count:
            lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {',
                '    {"cpu", "i64", XR_NATIVE_TERM_I64, false, false},', '};']
        lines += ['static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(ordinal) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' +
            c_string('bool' if count else 'i64') + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + (symbol + '_parameters' if count else 'NULL') +
            ', ' + str(count) + ', XR_NATIVE_TERM_' + result, '};']
        rows += ['    {XR_NATIVE_CALLABLE_' + identity + ', NULL, &' + symbol + ', NULL, false, "' +
            module + '", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows



def render_os_system_callables(declarations):
    contracts = (
        ('uid', '(): i64?', 'uid', 'NULLABLE_I64'),
        ('gid', '(): i64?', 'gid', 'NULLABLE_I64'),
        ('ppid', '(): i64', 'ppid', 'I64'),
        ('kill', '(pid: i64, signal: i64): bool', 'kill_signal', 'BOOL'),
        ('totalMemory', '(): i64', 'total_memory', 'I64'),
        ('freeMemory', '(): i64', 'free_memory', 'I64'),
        ('uptime', '(): f64', 'uptime', 'F64'),
        ('loadavg', '(): Array<f64>', 'loadavg', 'ARRAY_F64'),
    )
    lines, rows = [], []
    for ordinal, (name, signature, provider, term) in enumerate(contracts, 14):
        candidates = [row for row in declarations if row.module == 'os' and row.name == '__' + name]
        if len(candidates) != 1:
            raise ValueError('OS system query requires its sole governed declaration: ' + name)
        row = candidates[0]
        kill = name == 'kill'
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
                signature, '2' if kill else '0', 'vv' if kill else '', 'internal', '', '',
                'fresh' if name == 'loadavg' else '', 'system', 'os_' + name,
                '' if name in ('uid', 'gid') else 'xrt_os_' + provider, name not in ('uid', 'gid'), '' if name in ('uid', 'gid') else 'method', '', 'normal', '', '',
                'value', False, None, (), ''):
            raise ValueError('OS system query differs from its complete source contract: ' + name)
        identity = 'OS_' + ('KILL' if kill else provider.upper())
        symbol = 'xr_native_' + identity.lower()
        if kill:
            lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {',
                '    {"pid", "i64", XR_NATIVE_TERM_I64, false, false},',
                '    {"signal", "i64", XR_NATIVE_TERM_I64, false, false},', '};']
        lines += ['static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(ordinal) + ', ' + c_string('__' + name) + ', ' + c_string(signature) + ', ' +
            c_string(signature.split(': ')[-1]) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + (symbol + '_parameters' if kill else 'NULL') +
            ', ' + ('2' if kill else '0') + ', XR_NATIVE_TERM_' + term, '};']
        rows += ['    {XR_NATIVE_CALLABLE_' + identity + ', NULL, &' + symbol + ', NULL, false, "os", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows


def render_file_stat_identity(root):
    import sys
    sys.path.insert(0, str(root / 'tools/stdlibgen'))
    from stdlibgen import parse_handles
    candidates = [h for h in parse_handles(root) if h.module == 'io' and h.name == '__FileStat']
    expected = tuple(zip(('size', 'mode', 'mtime', 'atime', 'ctime', 'uid', 'gid',
                         'isFile', 'isDir', 'isSymlink'),
                        ('i64',) * 5 + ('i64?',) * 2 + ('bool',) * 3))
    if len(candidates) != 1 or tuple((f.name, f.type) for f in candidates[0].fields) != expected or not all(f.is_const for f in candidates[0].fields):
        raise ValueError('File metadata requires its exact immutable governed fields')
    digest = hashlib.sha256((root / 'stdlib/defs/core.def').read_text(encoding='utf-8').encode('utf-8')).digest()
    fingerprint = '{{' + ','.join(str(b) for b in digest) + '}}'
    lines = ['static const XrNativeMemberDeclaration xr_native_file_stat_members[] = {']
    for index, (name, type_text) in enumerate(expected):
        term = {'i64': 'I64', 'i64?': 'NULLABLE_I64', 'bool': 'BOOL'}[type_text]
        lines += ['    {' + str(index + 1) + ', ' + c_string(name) + ', ' + c_string(': ' + type_text) + ', ' + c_string(type_text) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
                  '        false, false, false, true, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
                  '        "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, NULL, 0, XR_NATIVE_TERM_' + term + '},']
    return lines + ['};', 'static const XrNativeTypeDeclaration xr_native_file_stat = {',
        '    XR_NATIVE_DECLARATION_FILE_STAT, XR_NATIVE_DECLARATION_IDENTITY, "__FileStat", NULL, 0,',
        '    "stdlib/defs/core.def", "xray.native-type.v1/io/__FileStat", ' + fingerprint + ', 0, 0,',
        '    xr_native_file_stat_members, 10', '};']


def render_io_callables(declarations):
    contracts = (
        ('__chmod', '(path: Path, mode: i64): bool', 'pv', 'io_chmod', 'xrt_io_chmod_value', 'IO_CHMOD'),
        ('__chdir', '(path: Path): bool', 'p', 'io_chdir', 'xrt_io_chdir', 'IO_CHDIR'),
        ('__stat', '(path: Path): __FileStat?', 'p', 'io_stat', '', 'IO_STAT'),
        ('__mkdir', '(path: Path): bool', 'p', 'io_mkdir', 'xrt_io_mkdir', 'IO_MKDIR'),
        ('__rmdir', '(path: Path): bool', 'p', 'io_rmdir', 'xrt_io_rmdir', 'IO_RMDIR'),
        ('__remove', '(path: Path): bool', 'p', 'io_remove', 'xrt_io_remove', 'IO_REMOVE'),
        ('__rename', '(old: Path, new: Path): bool', 'pp', 'io_rename', 'xrt_io_rename', 'IO_RENAME'),
        ('__utimeNow', '(path: Path): bool', 'p', 'io_utime_now', 'xrt_io_utime_now', 'IO_UTIME_NOW'),
        ('__symlink', '(source: Path, link: Path): bool', 'pp', 'io_symlink', 'xrt_io_symlink', 'IO_SYMLINK'),
        ('__readlink', '(path: Path): string?', 'p', 'io_readlink', 'xrt_io_readlink', 'IO_READLINK'),
        ('__realpath', '(path: Path): string?', 'p', 'io_realpath', 'xrt_io_realpath', 'IO_REALPATH'),
        ('__makeTempDir', '(root: string): string?', 's', 'io_make_temp_dir', 'xrt_io_make_temp_dir', 'IO_MAKE_TEMP_DIR'),
        ('__makeTempFile', '(root: string): string?', 's', 'io_make_temp_file', 'xrt_io_make_temp_file', 'IO_MAKE_TEMP_FILE'),
        ('__dirOpen', '(path: Path): i64', 'p', 'io_dirOpen', 'xrt_io_dir_open', 'IO_DIR_OPEN'),
        ('__dirNext', '(handle: i64): string?', 'i', 'io_dirNext', 'xrt_io_dir_next', 'IO_DIR_NEXT'),
        ('__dirClose', '(handle: i64): bool', 'i', 'io_dirClose', 'xrt_io_dir_close', 'IO_DIR_CLOSE'),
    )
    lines, rows = [], []
    for ordinal, (name, signature, arguments, vm, aot, identity) in enumerate(contracts, 1):
        candidates = [row for row in declarations if row.module == 'io' and row.name == name]
        if len(candidates) != 1:
            raise ValueError('Filesystem callable requires its sole governed declaration: ' + name)
        row = candidates[0]
        stat = identity == 'IO_STAT'
        text_result = identity in ('IO_READLINK', 'IO_REALPATH', 'IO_MAKE_TEMP_DIR', 'IO_MAKE_TEMP_FILE', 'IO_DIR_NEXT')
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
                signature, str(len(arguments)), arguments, 'internal', '', '', 'fresh' if stat or text_result else '', 'system', vm,
                aot, not stat, '' if stat else 'method', '', 'normal', '', '', 'value', False, None, (), ''):
            raise ValueError('Filesystem callable differs from its complete source contract: ' + name)
        symbol = 'xr_native_' + identity.lower()
        result, term = ('__FileStat?', 'NULLABLE_FILE_STAT') if stat else ('bool', 'BOOL')
        if text_result:
            result, term = 'string?', 'NULLABLE_STRING'
        elif identity == 'IO_DIR_OPEN':
            result, term = 'i64', 'I64'
        lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {']
        if identity in ('IO_MAKE_TEMP_DIR', 'IO_MAKE_TEMP_FILE'):
            lines += ['    {"root", "string", XR_NATIVE_TERM_STRING, false, false},']
        elif identity in ('IO_DIR_NEXT', 'IO_DIR_CLOSE'):
            lines += ['    {"handle", "i64", XR_NATIVE_TERM_I64, false, false},']
        elif identity == 'IO_RENAME':
            lines += ['    {"old", "Path", XR_NATIVE_TERM_PATH, false, false},',
                      '    {"new", "Path", XR_NATIVE_TERM_PATH, false, false},']
        elif identity == 'IO_SYMLINK':
            lines += ['    {"source", "Path", XR_NATIVE_TERM_PATH, false, false},',
                      '    {"link", "Path", XR_NATIVE_TERM_PATH, false, false},']
        else:
            lines += ['    {"path", "Path", XR_NATIVE_TERM_PATH, false, false},']
            if len(arguments) == 2:
                lines += ['    {"mode", "i64", XR_NATIVE_TERM_I64, false, false},']
        lines += ['};', 'static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(ordinal) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' + c_string(result) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + symbol + '_parameters, ' + str(len(arguments)) + ', XR_NATIVE_TERM_' + term, '};']
        rows += ['    {XR_NATIVE_CALLABLE_' + identity + ', NULL, &' + symbol + ', NULL, false, "io", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows


def render_os_getpid(declarations):
    candidates = [row for row in declarations if row.module == 'os' and row.name == '__getpid']
    if len(candidates) != 1:
        raise ValueError('Process id requires its sole governed declaration')
    row = candidates[0]
    if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
            row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct, row.aot_kind,
            row.link_object, row.vm_binding, row.vm_ifdef, row.define, row.ret,
            row.semantic_intrinsic, row.caps, row.suspension_kind) != (
            '(): i64', '0', '', 'internal', 'nothrow', 'no_heap', '', 'system',
            'os_getpid', 'xrt_os_getpid', True, 'method', '', 'normal', '', '',
            'value', False, (), ''):
        raise ValueError('Process id differs from its complete source contract')
    provider = row.provider_declaration
    if not provider or (provider.contract, provider.operation,
            tuple((p.type, p.mode, p.owner) for p in provider.logical.parameters),
            provider.logical.result_type, provider.logical.result_owner,
            provider.logical.error, provider.logical.panic, provider.logical.suspend,
            provider.logical.refusal, provider.logical.resources, provider.logical.effects,
            provider.logical.threads, provider.logical.reentry, provider.logical.callbacks,
            provider.logical.platforms, provider.logical.profiles,
            provider.host.adapter, provider.host.header, provider.host.symbol) != (
            'xray.runtime.provider.v1/process', 'xray.runtime.provider-operation.v1/process/getpid',
            (), 'i64', 'trivial', 'none', 'none', 'never', 'trap', (), ('reads-process',),
            'any', 'allowed', 'none', ('linux', 'macos', 'windows'), ('hosted',),
            'i64-nullary-i64', 'shared/xr_os_core.h', 'xr_os_core_getpid'):
        raise ValueError('Process id provider differs from its typed receiver')
    return [
        'static const XrNativeMemberDeclaration xr_native_os_getpid = {',
        '    8, "__getpid", "(): i64", "i64", 0, 0, XR_NATIVE_RECEIVER_READ,',
        '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_NO_HEAP,',
        '    "none", XR_NATIVE_RESULT_OWNERSHIP_OWNED, NULL, 0, XR_NATIVE_TERM_I64',
        '};'], [
        '    {XR_NATIVE_CALLABLE_OS_GETPID, NULL, &xr_native_os_getpid, NULL, false, "os", "stdlib/defs/core.def", FINGERPRINT}']


def render_os_callables(declarations):
    lines, rows = [], []
    for ordinal, name in enumerate(('platform', 'arch', 'getenv', 'setenv', 'unsetenv', 'environBlock', 'exit', 'getpid', 'getcwd', 'hostname', 'systemUsername', 'systemHomedir')):
        if name == 'getpid':
            pid_lines, pid_rows = render_os_getpid(declarations)
            lines += pid_lines
            rows += pid_rows
            continue
        candidates = [row for row in declarations if row.module == 'os' and row.name == '__' + name]
        if len(candidates) != 1:
            raise ValueError('OS host fact requires its sole governed declaration: ' + name)
        row = candidates[0]
        environment, setter, remove = name == 'getenv', name == 'setenv', name == 'unsetenv'
        snapshot, terminate, username = name == 'environBlock', name == 'exit', name in ('systemUsername', 'systemHomedir')
        provider = 'environ_block' if snapshot else 'system_username' if name == 'systemUsername' else 'system_homedir' if username else name
        signature = '(code: i64): ()' if terminate else '(): Array<string>' if snapshot else '(name: string): bool' if remove else '(name: string, value: string): bool' if setter else '(name: string): string?' if environment else '(): string?' if username else '(): string'
        result = '()' if terminate else 'Array<string>' if snapshot else 'bool' if setter or remove else 'string?' if environment or username else 'string'
        if (row.signature, row.argc, row.arg_spec, row.visibility, row.effect, row.allocation,
                row.return_ownership, row.layer, row.vm, row.aot, row.aot_direct,
                row.aot_kind, row.link_object, row.vm_binding, row.vm_ifdef, row.define,
                row.ret, row.semantic_intrinsic, row.provider_declaration, row.caps, row.suspension_kind) != (
                signature, '2' if setter else '1' if environment or remove or terminate else '0', 'ss' if setter else 's' if environment or remove else 'v' if terminate else '', 'internal', '', '', '' if setter or remove or terminate else 'fresh', 'system', 'os_' + provider,
                'xrt_os_' + provider, True, 'method', '', 'normal', '', '', 'value', False, None, (), ''):
            raise ValueError('OS host fact differs from its complete source contract: ' + name)
        symbol = 'xr_native_os_' + provider
        if terminate:
            lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {',
                '    {"code", "i64", XR_NATIVE_TERM_I64, false, false},', '};']
        if environment or setter or remove:
            lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {',
                '    {"name", "string", XR_NATIVE_TERM_STRING, false, false},']
            if setter:
                lines += ['    {"value", "string", XR_NATIVE_TERM_STRING, false, false},']
            lines += ['};']
        lines += ['static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(ordinal + 1) + ', "__' + name + '", ' + c_string(signature) + ', ' + c_string(result) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_UNKNOWN,',
            '    "", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + (symbol + '_parameters, 1, XR_NATIVE_TERM_UNIT' if terminate else symbol + '_parameters, 1, XR_NATIVE_TERM_BOOL' if remove else symbol + '_parameters, 2, XR_NATIVE_TERM_BOOL' if setter else symbol + '_parameters, 1, XR_NATIVE_TERM_NULLABLE_STRING' if environment else
                'NULL, 0, XR_NATIVE_TERM_ARRAY_STRING' if snapshot else 'NULL, 0, XR_NATIVE_TERM_NULLABLE_STRING' if username else 'NULL, 0, XR_NATIVE_TERM_STRING'), '};']
        rows += ['    {XR_NATIVE_CALLABLE_OS_' + provider.upper() + ', NULL, &' + symbol + ', NULL, false, "os", "stdlib/defs/core.def", FINGERPRINT}']
    return lines, rows



def render_unicode_callable(root, decoder):
    import sys
    sys.path.insert(0, str(root / 'tools' / 'stdlibgen'))
    from stdlibgen import parse_defs
    declarations = parse_defs(root)
    contracts = (
        ('__unicodePropId', '(name: string): i64', '1', 's'),
        ('__unicodeHasProp', '(cp: i64, propId: i64): bool', '2', 'vv'),
    )
    for name, signature, count, arguments in contracts:
        rows = [row for row in declarations if row.module == 'regex' and row.name == name]
        if len(rows) != 1:
            raise ValueError('Unicode callable requires its sole governed declaration: ' + name)
        row = rows[0]
        if (row.signature, row.argc, row.arg_spec, row.layer, row.allocation) != (
                signature, count, arguments, 'pure', 'no_heap'):
            raise ValueError('Unicode callable disagrees with its typed contract: ' + name)
    buffer_contracts = (
        (4, '__alloc', '(n: i64): __BufferStorage'),
        (5, '__allocZeroed', '(n: i64): __BufferStorage'),
        (6, '__allocAligned', '(n: i64, align: i64): __BufferStorage'),
        (7, '__bufferLength', '(storage: __BufferStorage): i64'),
        (10, '__bufferAsBytes', '(storage: __BufferStorage): Slice<u8>'),
        (11, '__bufferBorrowPtr', '(storage: __BufferStorage): MutPtr<u8>'),
        (12, '__bufferResize', '(storage: __BufferStorage, n: i64): bool'),
        (13, '__fence', '(ordering: i64): ()'),
        (14, '__prefetch', '(ptr: Ptr<u8>, rw: i64): ()'),
        (15, '__cacheFlush', '(ptr: Ptr<u8>, n: i64): ()'),
        (16, '__nontemporalStore', '(ptr: MutPtr<u8>, v: i64, size: i64): ()'),
        (17, '__cacheLineSize', '(): i64'),
        (18, '__pageAlloc', '(bytes: i64, prot: i64): MutPtr<u8>'),
        (19, '__pageProtect', '(ptr: MutPtr<u8>, bytes: i64, prot: i64): bool'),
        (20, '__pageFree', '(ptr: MutPtr<u8>, bytes: i64): bool'),
        (21, '__move', '(dst: MutPtr<u8>, src: Ptr<u8>, n: i64): ()'),
        (22, '__volatileLoad', '(ptr: Ptr<u8>, size: i64): i64'),
        (23, '__volatileStore', '(ptr: MutPtr<u8>, v: i64, size: i64): ()'),
        (24, '__bufferSliceMutBegin', '(ptr: MutPtr<u8>, bytes: i64, owner: ref Buffer): MutSlice<u8>'),
    )
    terms = {'__BufferStorage': 'BUFFER_STORAGE', 'i64': 'I64',
             'bool': 'BOOL', '()': 'UNIT', 'Slice<u8>': 'SLICE_U8',
             'Ptr<u8>': 'PTR_U8', 'MutPtr<u8>': 'MUT_PTR_U8',
             'ref Buffer': 'BUFFER_REF', 'MutSlice<u8>': 'MUT_SLICE_U8'}
    buffer_lines, buffer_rows = [], []
    for ordinal, (identity, name, signature) in enumerate(buffer_contracts):
        rows = [row for row in declarations if row.module == 'mem' and row.name == name]
        if len(rows) != 1 or rows[0].signature != signature:
            raise ValueError('Memory callable disagrees with its governed declaration: ' + name)
        row = rows[0]
        signature_args, result_text = signature.rsplit(': ', 1)
        arguments = signature_args[1:-1]
        params = [item.split(': ', 1) for item in arguments.split(', ')] if arguments else []
        if row.argc != str(len(params)) or row.arg_spec != 'v' * len(params):
            raise ValueError('Memory callable arity disagrees: ' + name)
        borrowed = row.return_ownership == 'borrowed_param:0'
        scoped = identity == 24
        if borrowed != (ordinal in (4, 5)) or (row.return_ownership == 'borrowed_param:2') != scoped:
            raise ValueError('Memory callable borrow origin disagrees: ' + name)
        allocation = 'MAY_HEAP' if ordinal in (0, 1, 2, 4, 5, 6, 12) or scoped else 'NO_HEAP'
        if row.allocation.upper() != allocation:
            raise ValueError('Memory callable allocation facts disagree: ' + name)
        ownership = 'BORROWED_PARAM_2' if scoped else 'BORROWED_PARAM_0' if borrowed else 'UNIT' if result_text == '()' else 'OWNED'
        symbol = 'xr_native_buffer_' + str(ordinal)
        if params:
            buffer_lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {']
            buffer_lines += ['    {' + c_string(n) + ', ' + c_string(t) + ', XR_NATIVE_TERM_' + terms[t] + ', false, false},' for n, t in params]
            buffer_lines += ['};']
        buffer_lines += ['static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(ordinal + 1) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' + c_string(result_text) + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_' + allocation + ',',
            '    ' + c_string('allocation,retain,limit' if allocation == 'MAY_HEAP' else 'none') + ', XR_NATIVE_RESULT_OWNERSHIP_' + ownership + ', ' + (symbol + '_parameters' if params else 'NULL') + ', ' + str(len(params)) + ', XR_NATIVE_TERM_' + terms[result_text], '};']
        buffer_rows.append('    {' + str(identity) + ', NULL, &' + symbol + ', NULL, false, "mem", "stdlib/defs/core.def", FINGERPRINT}')
    crypto_contracts = (
        ('__fillRandomBytes', '(bytes: ref Array<u8>): ()', '1', 'v'),
        ('__timingSafeEqualBytes', '(a: Array<u8>, b: Array<u8>): bool', '2', 'vv'),
    )
    for ordinal, (name, signature, count, arguments) in enumerate(crypto_contracts):
        rows = [row for row in declarations if row.module == 'crypto' and row.name == name]
        if len(rows) != 1 or (rows[0].signature, rows[0].argc, rows[0].arg_spec,
                rows[0].visibility, rows[0].effect, rows[0].allocation) != (
                signature, count, arguments, 'internal', 'nothrow', '' if ordinal == 0 else 'no_heap'):
            raise ValueError('Crypto callable differs from its complete source contract: ' + name)
        provider = rows[0].provider_declaration
        if ordinal == 0:
            if not provider or (provider.contract, provider.operation,
                    tuple((p.type, p.mode, p.owner) for p in provider.logical.parameters),
                    provider.logical.result_type, provider.logical.result_owner,
                    provider.logical.error, provider.logical.panic, provider.logical.suspend,
                    provider.logical.refusal, provider.logical.resources, provider.logical.effects,
                    provider.logical.threads, provider.logical.reentry, provider.logical.callbacks,
                    provider.logical.platforms, provider.logical.profiles,
                    provider.host.adapter, provider.host.header, provider.host.symbol) != (
                    'xray.runtime.provider.v1/entropy', 'xray.runtime.provider-operation.v1/entropy/fill-bytes',
                    (('Array<u8>', 'ref', 'borrow'),), '()', 'trivial', 'none', 'none', 'never',
                    'trap', (), ('io',), 'any', 'forbidden', 'none', ('linux', 'macos', 'windows'),
                    ('hosted',), 'typed', 'execution/xr_random_provider.h', 'xr_random_provider_fill'):
                raise ValueError('Entropy provider contract differs from its typed receiver')
        elif provider is not None:
            raise ValueError('Timing comparison must remain a no-heap core operation')
        symbol = 'xr_native_crypto_' + str(ordinal)
        params = [('bytes', 'ref Array<u8>')] if ordinal == 0 else [('a', 'Array<u8>'), ('b', 'Array<u8>')]
        buffer_lines += ['static const XrNativeParameter ' + symbol + '_parameters[] = {']
        buffer_lines += ['    {' + c_string(n) + ', ' + c_string(t) + ', XR_NATIVE_TERM_ARRAY_U8, false, false},' for n, t in params]
        buffer_lines += ['};', 'static const XrNativeMemberDeclaration ' + symbol + ' = {',
            '    ' + str(ordinal + 1) + ', ' + c_string(name) + ', ' + c_string(signature) + ', ' + c_string('()' if ordinal == 0 else 'bool') + ', 0, 0, XR_NATIVE_RECEIVER_READ,',
            '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_' + ('UNKNOWN' if ordinal == 0 else 'NO_HEAP') + ',',
            '    "none", XR_NATIVE_RESULT_OWNERSHIP_OWNED, ' + symbol + '_parameters, ' + count + ', XR_NATIVE_TERM_' + ('UNIT' if ordinal == 0 else 'BOOL'), '};']
        buffer_rows.append('    {' + str(ordinal + 8) + ', NULL, &' + symbol + ', NULL, false, "crypto", "stdlib/defs/core.def", FINGERPRINT}')
    math_lines, math_rows = render_math_callables(declarations)
    buffer_lines += math_lines
    buffer_rows += math_rows
    sys_lines, sys_rows = render_sys_thread_yield(declarations)
    buffer_lines += sys_lines
    buffer_rows += sys_rows
    signal_lines, signal_rows = render_sys_signal(declarations)
    buffer_lines += signal_lines
    buffer_rows += signal_rows
    thread_lines, thread_rows = render_sys_thread_tokens(declarations)
    buffer_lines += thread_lines
    buffer_rows += thread_rows
    pipe_lines, pipe_rows = render_sys_pipes(declarations)
    buffer_lines += pipe_lines
    buffer_rows += pipe_rows
    process_lines, process_rows = render_sys_processes(declarations)
    buffer_lines += process_lines
    buffer_rows += process_rows
    dylib_lines, dylib_rows = render_sys_dylibs(declarations)
    buffer_lines += dylib_lines
    buffer_rows += dylib_rows
    os_lines, os_rows = render_os_callables(declarations)
    buffer_lines += os_lines
    buffer_rows += os_rows
    cpu_lines, cpu_rows = render_cpu_callables(declarations)
    buffer_lines += cpu_lines
    buffer_rows += cpu_rows
    system_lines, system_rows = render_os_system_callables(declarations)
    buffer_lines += system_lines
    buffer_rows += system_rows
    io_lines, io_rows = render_io_callables(declarations)
    buffer_lines += io_lines
    buffer_rows += io_rows
    digest = hashlib.sha256((root / 'stdlib/defs/core.def').read_text(encoding='utf-8').encode('utf-8')).digest()
    fingerprint = '{{' + ','.join(str(byte) for byte in digest) + '}}'
    return buffer_lines + [
        'static const XrNativeParameter xr_native_unicode_property_parameters[] = {',
        '    {"name", "string", XR_NATIVE_TERM_STRING, false, false},', '};',
        'static const XrNativeParameter xr_native_unicode_has_property_parameters[] = {',
        '    {"cp", "i64", XR_NATIVE_TERM_I64, false, false},',
        '    {"propId", "i64", XR_NATIVE_TERM_I64, false, false},', '};',
        'static const XrNativeMemberDeclaration xr_native_unicode_property = {',
        '    1, "__unicodePropId", "(name: string): i64", "i64", 0, 0, XR_NATIVE_RECEIVER_READ,',
        '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_NO_HEAP,',
        '    "none", XR_NATIVE_RESULT_OWNERSHIP_OWNED, xr_native_unicode_property_parameters, 1, XR_NATIVE_TERM_I64', '};',
        'static const XrNativeMemberDeclaration xr_native_unicode_has_property = {',
        '    2, "__unicodeHasProp", "(cp: i64, propId: i64): bool", "bool", 0, 0, XR_NATIVE_RECEIVER_READ,',
        '    false, false, false, false, XR_NATIVE_OPERATION_NONE, XR_NATIVE_ALLOCATION_NO_HEAP,',
        '    "none", XR_NATIVE_RESULT_OWNERSHIP_OWNED, xr_native_unicode_has_property_parameters, 2, XR_NATIVE_TERM_BOOL', '};',
        'static const XrNativeCallableDeclaration xr_native_callables[] = {',
        '    {XR_NATIVE_CALLABLE_STRING_FROM_UTF8, &xr_native_string, &xr_native_string_members[' + str(decoder) + '], &xr_native_utf8_error, false, NULL, NULL, {{0}}},',
        '    {XR_NATIVE_CALLABLE_TASK_ALL_BEGIN, &xr_native_task, &xr_native_task_members[3], NULL, false, NULL, NULL, {{0}}},',
        '    {XR_NATIVE_CALLABLE_TASK_FIRST_BEGIN, &xr_native_task, &xr_native_task_members[4], NULL, false, NULL, NULL, {{0}}},',
        '    {XR_NATIVE_CALLABLE_UNICODE_PROPERTY_ID, NULL, &xr_native_unicode_property, NULL, true, "regex", "stdlib/defs/core.def", ' + fingerprint + '},',
        '    {XR_NATIVE_CALLABLE_UNICODE_HAS_PROPERTY, NULL, &xr_native_unicode_has_property, NULL, true, "regex", "stdlib/defs/core.def", ' + fingerprint + '},'] + [row.replace('FINGERPRINT', fingerprint) + (',' if i + 1 < len(buffer_rows) else '') for i, row in enumerate(buffer_rows)] + ['};']


def render(root):
    lines = ['/* Generated by gen_native_declarations.py; do not edit. */']
    array_lines, header, members = render_type(root, "array")
    string_lines, _, _ = render_type(root, "string")
    integer_lines, _, _ = render_type(root, "i64")
    scalar_lines = []
    for stem in ('f64', 'bool', 'rune'):
        generated, _, _ = render_type(root, stem)
        scalar_lines += generated
    lines += array_lines + string_lines + integer_lines + scalar_lines + render_atomic_identities(root) + render_task_identity(root) + render_channel_identity(root) + render_channel_result_identities(root) + render_iterator_identity(root) + render_type(root, 'map')[0] + render_type(root, 'set')[0]
    lines += render_file_stat_identity(root)
    lines += render_intrinsics(root)
    _, _, string_members = render_type(root, 'string')
    decoder = next(i for i, m in enumerate(string_members) if m.operation == 'STRING_FROM_UTF8')
    lines += render_unicode_callable(root, decoder)
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
