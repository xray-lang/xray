"""Derive private C emitter tables from maintained canonical C literals."""
from pathlib import Path
from hashlib import sha256
import argparse
import json
import re

PATTERNS = (b'goto invalid;', b'goto limit;')
TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"', re.S)
CALL = re.compile(r'\b(EMIT_STATIC|EMIT_FORMAT|EMIT_U_SHAPE|EMIT_ULL_SHAPE|EMIT_LITERAL|append)\s*\(\s*buffer\s*,\s*')


def decode(token):
    source, result, at = token[1:-1], bytearray(), 0
    escapes = {'n': 10, 'r': 13, 't': 9, 'b': 8, 'f': 12, 'v': 11,
               'a': 7, '"': 34, "'": 39, '\\': 92, '?': 63}
    while at < len(source):
        byte = source[at]
        at += 1
        if byte != '\\':
            if ord(byte) >= 128: raise ValueError('canonical literals must be ASCII')
            result.append(ord(byte))
            continue
        byte = source[at]
        at += 1
        if byte in escapes:
            result.append(escapes[byte])
        elif byte in '01234567':
            digits = byte
            while at < len(source) and len(digits) < 3 and source[at] in '01234567':
                digits += source[at]
                at += 1
            result.append(int(digits, 8))
        elif byte == 'x':
            start = at
            while at < len(source) and source[at] in '0123456789abcdefABCDEF': at += 1
            if start == at: raise ValueError('empty hexadecimal escape')
            result.append(int(source[start:at], 16))
        else:
            raise ValueError(f'unsupported canonical escape: {byte}')
    if 0 in result: raise ValueError('canonical emission span must not contain NUL')
    return bytes(result)


def segments(value):
    result, at, start = [], 0, 0
    while at < len(value):
        if value[at] != 37:
            at += 1
            continue
        if at > start: result.append(('TEXT', value[start:at]))
        if value.startswith(b'%%', at):
            result.append(('TEXT', b'%'))
            at += 2
        else:
            specs = ((b'%02x', 'HEX'), (b'%llu', 'LLU'), (b'%u', 'U'), (b'%d', 'D'), (b'%s', 'S'))
            found = next(((s, k) for s, k in specs if value.startswith(s, at)), None)
            if found is None: raise ValueError(f'unsupported format at {value[at:]!r}')
            spec, kind = found
            result.append((kind, b''))
            at += len(spec)
        start = at
    if start < len(value): result.append(('TEXT', value[start:]))
    return result


def shape_literal(text, at):
    if at >= len(text) or text[at] != ',': raise ValueError('shape operand separator missing')
    at += 1
    while at < len(text) and text[at].isspace(): at += 1
    values = []
    while True:
        token = TOKEN.match(text, at)
        if token is None: break
        values.append(decode(token[0]))
        at = token.end()
        while at < len(text) and text[at].isspace(): at += 1
    if not values: raise ValueError('shape operand must be a static string literal')
    return b''.join(values), at


def shape_value(text, at):
    at += 1
    start, stack = at, []
    quoted = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'')
    while at < len(text):
        if text.startswith('//', at):
            end = text.find('\n', at)
            if end < 0: raise ValueError('shape call has no closing delimiter')
            at = end + 1
            continue
        if text.startswith('/*', at):
            end = text.find('*/', at+2)
            if end < 0: raise ValueError('unterminated shape value comment')
            at = end + 2
            continue
        if text[at] in ('"', "'"):
            token = quoted.match(text, at)
            if token is None: raise ValueError('unterminated shape value literal')
            at = token.end()
            continue
        byte = text[at]
        if byte in '([{': stack.append(byte)
        elif byte in ')]}':
            if not stack:
                if byte != ')' or not text[start:at].strip():
                    raise ValueError('shape requires one nonempty typed value')
                return
            if stack.pop() != {')':'(',']':'[','}':'{'}[byte]:
                raise ValueError('shape value delimiters differ')
        elif byte == ',' and not stack:
            raise ValueError('shape has more than one typed value argument')
        at += 1
    raise ValueError('shape call has no closing delimiter')


def validate_shape(operation, text, at, value):
    kind = 'U' if operation == 'EMIT_U_SHAPE' else 'LLU'
    sequence = segments(value)
    conversions = [(i, k) for i, (k, v) in enumerate(sequence) if k != 'TEXT']
    if len(conversions) != 1 or conversions[0][1] != kind:
        raise ValueError('shape requires exactly one corresponding unsigned conversion')
    index = conversions[0][0]
    first = b''.join(v for k, v in sequence[:index])
    last = b''.join(v for k, v in sequence[index+1:])
    actual_first, next_at = shape_literal(text, at)
    actual_last, next_at = shape_literal(text, next_at)
    if b'%' in first + last or (first, last) != (actual_first, actual_last):
        raise ValueError('shape split differs from certified non-percent canonical bytes')
    if next_at >= len(text) or text[next_at] != ',':
        raise ValueError('shape requires one typed value argument')
    shape_value(text, next_at)
    return first, last


def read_canonical(text):
    spans, formats, calls = {b''}, set(), []
    for call in CALL.finditer(text):
        operation, at, named = call[1], call.end(), None
        if operation in ('EMIT_STATIC', 'EMIT_FORMAT', 'EMIT_U_SHAPE', 'EMIT_ULL_SHAPE'):
            match = re.match(r'(e[st]_[0-9a-f]{16})\s*,\s*', text[at:])
            if match is None: raise ValueError('marked canonical call has no stable literal name')
            named, at = match[1], at + match.end()
        parts = []
        while True:
            token = TOKEN.match(text, at)
            if token is None: break
            parts.append(decode(token[0]))
            at = token.end()
            while at < len(text) and text[at].isspace(): at += 1
        if not parts:
            if named is not None: raise ValueError('marked call requires string literals')
            continue  # The original nonliteral conditional format stays generic.
        value = b''.join(parts)
        literal = operation in ('EMIT_STATIC', 'EMIT_LITERAL')
        if literal and b'%' in value: raise ValueError('literal call contains a format conversion')
        expected = ('es_' if literal else 'et_') + sha256(value).hexdigest()[:16]
        if named is not None and named != expected: raise ValueError('stale canonical name; update it from literal SHA')
        if operation in ('EMIT_U_SHAPE', 'EMIT_ULL_SHAPE'):
            validate_shape(operation, text, at, value)
        parts = [('TEXT', value)] if literal else segments(value)
        spans.update(v for k, v in parts if k == 'TEXT')
        if not literal: formats.add(value)
        calls.append({'operation': operation, 'canonical_sha256': sha256(value).hexdigest(),
                      'line': text[:call.start()].count('\n') + 1})
    if not calls: raise ValueError('no maintained canonical inputs')
    return sorted(spans, key=lambda v: sha256(v).hexdigest()), sorted(formats, key=lambda v: sha256(v).hexdigest()), calls


def trace(pattern, value, initial):
    match, found, result = initial, False, [initial]
    for byte in value:
        if not found:
            match = match + 1 if byte == pattern[match] else int(byte == pattern[0])
            if match == len(pattern): match, found = 0, True
        result.append(128 if found else match)
    return result


def table(pattern, value):
    width = min(len(pattern), len(value) + 1)
    result = []
    for initial in range(len(pattern)): result.extend(trace(pattern, value, initial)[:width])
    result.extend(trace(pattern, value, 0))
    return result


def suffix_reference(pattern, value, initial):
    content = pattern[:initial] + value
    if pattern in content: return 128
    for count in range(len(pattern) - 1, 0, -1):
        if content.endswith(pattern[:count]): return count
    return 0


def verify_tables(spans):
    checks = 0
    for value in spans:
        for pattern in PATTERNS:
            width, length = min(len(pattern), len(value) + 1), len(pattern)
            data = table(pattern, value)
            for initial in range(length):
                for prefix in range(len(value) + 1):
                    short = data[initial * width + min(prefix, length - 1)]
                    zero = data[length * width + prefix]
                    actual = short if prefix < length or short & 128 else zero
                    if actual != suffix_reference(pattern, value[:prefix], initial):
                        raise ValueError('independent suffix/subsequence table mismatch')
                    checks += 1
    return checks


def spelling(value):
    result = '"'
    escape = {10: '\\n', 13: '\\r', 9: '\\t', 34: '\\"', 92: '\\\\', 63: '\\?'}
    for byte in value:
        result += escape.get(byte, chr(byte) if 32 <= byte <= 126 else f'\\{byte:03o}')
    return result + '"'


def chunks(values, count):
    return [values[i:i + count] for i in range(0, len(values), count)]


def generate(spans, formats):
    ids = {v: 'es_' + sha256(v).hexdigest()[:16] for v in spans}
    templates = {v: 'et_' + sha256(v).hexdigest()[:16] for v in formats}
    if len(set(ids.values())) != len(ids) or len(set(templates.values())) != len(templates):
        raise ValueError('canonical name collision')
    canonical = sha256(json.dumps([v.hex() for v in spans] + [v.hex() for v in formats]).encode()).hexdigest()
    out = ['/*', ' * xray - Lightweight typed scripting with native concurrency',
           ' * https://www.xray-lang.org', ' *',
           ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>',
           ' * Licensed under the MIT License', ' *',
           ' * xxir_emit_literal_tables.inc.c - Immutable compiler emission spans', ' *',
           ' * KEY CONCEPT:', ' *   Canonical call-site literals derive bounded prefix transitions without',
           ' *   runtime byte scans or per-emission metadata ownership.', ' *',
           ' * Generated by scripts/gen_xir_emit_literal_tables.py; edit canonical',
           ' * literals in xxir_emit_c.c, then regenerate. Compiler-only data.',
           f' * Canonical input SHA256: {canonical}', ' */',
           'typedef struct EmitSpan {', '    const char *text; size_t length;',
           '    const uint8_t *table[2];', '} EmitSpan;',
           'enum EmitPartKind { EP_TEXT, EP_U, EP_D, EP_LLU, EP_HEX, EP_S };',
           'typedef struct EmitPart { unsigned kind, span; } EmitPart;',
           'typedef struct EmitTemplate { const EmitPart *parts; size_t count; } EmitTemplate;',
           'enum {']
    out += ['    ' + ', '.join(f'{ids[v]}={i}' for i, v in enumerate(spans[start:start + 8], start)) + ','
            for start in range(0, len(spans), 8)]
    out += [f'    EMIT_SPAN_COUNT={len(spans)}', '};', 'enum {']
    out += ['    ' + ', '.join(f'{templates[v]}={i}' for i, v in enumerate(formats[start:start + 8], start)) + ','
            for start in range(0, len(formats), 8)]
    out += [f'    EMIT_TEMPLATE_COUNT={len(formats)}', '};']
    out += [f'static const char {ids[v]}_text[] = {spelling(v)};' for v in spans]
    pool, offsets = [], {}
    for value in spans:
        offsets[value] = []
        for pattern in PATTERNS:
            offsets[value].append(len(pool))
            pool.extend(table(pattern, value))
    if len(pool) > 4294967295: raise ValueError('transition offset exceeds uint32')
    out += ['static const uint8_t emit_literal_transitions[] = {']
    out += ['    ' + ','.join(map(str, row)) + ',' for row in chunks(pool, 128)]
    out += ['};', 'static const EmitSpan emit_literal_spans[] = {']
    out += [f'    {{{ids[v]}_text,{len(v)},{{emit_literal_transitions+{offsets[v][0]},'
            f'emit_literal_transitions+{offsets[v][1]}}}}},' for v in spans]
    out += ['};']
    parts, starts = [], {}
    for value in formats:
        sequence = segments(value)
        starts[value] = (len(parts), len(sequence))
        parts.extend(f'{{EP_{k},{ids[v] if k == "TEXT" else 0}}}' for k, v in sequence)
    out += ['static const EmitPart emit_literal_parts[] = {']
    out += ['    ' + ','.join(row) + ',' for row in chunks(parts, 8)]
    out += ['};', 'static const EmitTemplate emit_literal_templates[] = {']
    out += [f'    {{emit_literal_parts+{starts[v][0]},{starts[v][1]}}},' for v in formats]
    out += ['};', '_Static_assert(sizeof(emit_literal_spans)/sizeof(emit_literal_spans[0]) == EMIT_SPAN_COUNT, "span count");',
            '_Static_assert(sizeof(emit_literal_templates)/sizeof(emit_literal_templates[0]) == EMIT_TEMPLATE_COUNT, "template count");']
    if len(out) > 3000 or max(map(len, out)) >= 4095:
        raise ValueError('compiler data exceeds approved source line bounds')
    report = {'canonical_sha256': canonical, 'span_count': len(spans), 'template_count': len(formats),
              'transition_bytes': len(pool), 'physical_lines': len(out), 'maximum_source_line': max(map(len, out))}
    return '\n'.join(out) + '\n', report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'src/xir/xxir_emit_c.c')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--verify-model', action='store_true')
    args = parser.parse_args()
    before = args.source.read_bytes()
    spans, formats, calls = read_canonical(before.decode('utf-8'))
    result, report = generate(spans, formats)
    if args.verify_model: report['independent_prefix_checks'] = verify_tables(spans)
    output = args.output or args.source.with_name('xxir_emit_literal_tables.inc.c')
    if args.check:
        if output.read_bytes() != result.encode(): raise SystemExit('stale private emitter tables')
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(result, encoding='utf-8', newline='\n')
    if args.source.read_bytes() != before: raise SystemExit('canonical source changed during derivation')
    report.update({'canonical_calls': len(calls), 'source_changed': False,
                   'product_compiler_runtime_executed': False, 'output_sha256': sha256(result.encode()).hexdigest()})
    print(json.dumps(report, sort_keys=True))


if __name__ == '__main__': main()
