"""Verify source-backed native value declarations before compiler admission."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
import gen_native_declarations as schema
import embed_native_types as embed
import gen_api_inventory as inventory


class NativeDeclarations(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / 'stdlib/types/array.xr').read_text(encoding='utf-8')
        cls.prelude = (ROOT / 'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')

    def test_fresh_deterministic_owned_projection(self):
        first, second = schema.render(ROOT), schema.render(ROOT)
        self.assertEqual(first, second)
        for name, text in first.items():
            self.assertEqual((ROOT / name).read_text(encoding='utf-8'), text)
        header, members = schema.parse_source(self.source, self.prelude)
        self.assertEqual(header[:3], (1, 'Array', ('T',)))
        self.assertEqual(len(members), 32)
        self.assertEqual({m.operation for m in members if m.operation != 'NONE'},
                         {'ARRAY_GET', 'ARRAY_SET', 'ARRAY_PUSH', 'ARRAY_CAPACITY',
                          'ARRAY_WITH_CAPACITY', 'ARRAY_RESERVE'} | set(schema.array_recipe_contracts('T')))
        self.assertEqual(next(m for m in members if m.name == 'map').operation, 'ARRAY_MAP')
        self.assertNotIn('xr_native_def_array', embed.render(ROOT / 'stdlib/types'))

    def test_integer_definition_and_recipe_rejections(self):
        source = (ROOT / 'stdlib/types/i64.xr').read_text(encoding='utf-8')
        header, members = schema.parse_source(source, self.prelude)
        self.assertEqual(header[:3], (12, 'i64', ()))
        executable = [m for m in members if m.operation != 'NONE']
        self.assertEqual(len(executable), 25)
        self.assertEqual(next(m for m in members if m.name == 'sqrt').operation, 'INTEGER_SQRT')
        self.assertEqual(next(m for m in members if m.name == 'toBigInt').operation, 'NONE')
        for changed in [source.replace('wrappingAdd(other: i64)', 'wrappingAdd(other: f64)'),
                        source.replace('abs() -> i64', 'abs() -> f64'),
                        source.replace('INTEGER_CHECKED_ADD allocation=may_heap', 'INTEGER_CHECKED_ADD allocation=no_heap'),
                        source.replace('INTEGER_ABS allocation=no_heap failures=none', 'INTEGER_ABS allocation=no_heap failures=limit'),
                        source.replace('popcount() -> i64', 'popcount() -> u64'),
                        source.replace('leadingZeros() -> i64', 'leadingZeros(n: i64) -> i64'),
                        source.replace('trailingZeros() -> i64', 'ref trailingZeros() -> i64'),
                        source.replace('INTEGER_BYTESWAP allocation=no_heap', 'INTEGER_BYTESWAP allocation=may_heap'),
                        source.replace('rotateLeft(n: i64)', 'rotateLeft(n: u64)'),
                        source.replace('rotateRight(n: i64)', 'rotateRight(n?: i64)'),
                        source.replace('sqrt() -> f64', 'sqrt() -> i64'),
                        source.replace('sqrt() -> f64', 'sqrt(n: i64) -> f64'),
                        source.replace('sqrt() -> f64', 'ref sqrt() -> f64'),
                        source.replace('INTEGER_SQRT allocation=no_heap', 'INTEGER_SQRT allocation=may_heap'),
                        source.replace('// @native-type id=12', '// @native-type id=13')]:
            with self.assertRaises(ValueError):
                schema.parse_source(changed, self.prelude)

    def test_array_recipes_have_exact_symbolic_shapes(self):
        _, members = schema.parse_source(self.source, self.prelude)
        expected = {
            'map': ('CALLBACK_MAP', 'ARRAY_RESULT'),
            'filter': ('CALLBACK_PREDICATE_INDEXED', 'ARRAY_ELEMENT'),
            'reduce': ('CALLBACK_REDUCE', 'RESULT_VARIABLE'),
            'forEach': ('CALLBACK_VISIT', 'UNIT'),
            'find': ('CALLBACK_PREDICATE', 'NULLABLE_ELEMENT'),
            'findIndex': ('CALLBACK_PREDICATE', 'I64'),
            'every': ('CALLBACK_PREDICATE', 'BOOL'),
            'some': ('CALLBACK_PREDICATE', 'BOOL'),
        }
        for member in members:
            if member.name in expected:
                self.assertEqual((schema.simple_term(member.parameters[0][1], 'T'),
                                  schema.simple_term(member.result, 'T')), expected[member.name])
                self.assertEqual(member.receiver, 'READ')
            if member.operation != 'NONE':
                self.assertNotEqual(schema.simple_term(member.result, 'T'), 'UNADMITTED')
                for parameter in member.parameters:
                    self.assertNotEqual(schema.simple_term(parameter[1], 'T'), 'UNADMITTED')
        reduce = next(member for member in members if member.name == 'reduce')
        self.assertEqual(schema.simple_term(reduce.parameters[1][1], 'T'), 'RESULT_VARIABLE')
        reverse = next(member for member in members if member.name == 'reverse')
        unshift = next(member for member in members if member.name == 'unshift')
        self.assertEqual((reverse.receiver, reverse.ownership, len(reverse.parameters),
                          schema.simple_term(reverse.result, 'T')), ('REF', 'owned', 0, 'ARRAY_ELEMENT'))
        self.assertEqual((unshift.receiver, unshift.ownership, len(unshift.parameters),
                          schema.simple_term(unshift.parameters[0][1], 'T'),
                          schema.simple_term(unshift.result, 'T')), ('REF', 'unit', 1, 'ELEMENT', 'UNIT'))
        resize = next(member for member in members if member.name == 'resize')
        self.assertEqual((resize.receiver, resize.ownership, len(resize.parameters),
                          schema.simple_term(resize.parameters[0][1], 'T'),
                          schema.simple_term(resize.parameters[1][1], 'T'),
                          schema.simple_term(resize.result, 'T')),
                         ('REF', 'owned', 2, 'I64', 'ELEMENT', 'ARRAY_ELEMENT'))
        entries = next(member for member in members if member.name == 'entries')
        self.assertEqual((entries.receiver, entries.ownership, len(entries.parameters),
                          entries.operation, schema.simple_term(entries.result, 'T')),
                         ('READ', 'owned', 0, 'ARRAY_ENTRIES', 'ARRAY_INDEX_ELEMENT'))
        clear = next(member for member in members if member.name == 'clear')
        self.assertEqual((clear.receiver, clear.ownership, clear.result), ('REF', 'unit', ('tuple', ())))
        self.assertEqual({m.name for m in members if m.operation == 'NONE'},
                         {'ptr', 'mutPtr', 'sort', 'toString',
                          'iterator', 'entriesIterator'})

    def test_binary64_owned_text_conversion_contract(self):
        source = (ROOT / 'stdlib/types/f64.xr').read_text(encoding='utf-8')
        header, members = schema.parse_source(source, self.prelude)
        self.assertEqual(header[:3], (13, 'f64', ()))
        admitted = [m for m in members if m.operation != 'NONE']
        self.assertEqual(len(admitted), 8)
        member = next(m for m in admitted if m.operation == 'F64_TRY_PARSE')
        self.assertEqual((member.name, member.operation, member.static, member.lowered,
                          member.allocation, member.failures, member.ownership),
                         ('tryParse', 'F64_TRY_PARSE', True, True, 'may_heap', 'allocation,retain,limit', 'owned'))
        self.assertEqual(schema.simple_term(member.result, ''), 'NULLABLE_F64')
        for before, after in [
                ('id=13', 'id=5'), ('class f64', 'struct f64'),
                ('static tryParse', 'tryParse'), ('tryParse(text: string)', 'tryParse(text: i64)'),
                ('tryParse(text: string)', 'tryParse(text?: string)'),
                ('tryParse(text: string)', 'tryParse(text: string, extra: string)'),
                ('-> f64?', '-> f64'), ('allocation=may_heap', 'allocation=no_heap'),
                ('failures=allocation,retain,limit', 'failures=none'), ('ownership=owned', 'ownership=unit')]:
            with self.subTest(after=after), self.assertRaises(ValueError):
                schema.parse_source(source.replace(before, after), self.prelude)
        with self.assertRaises(ValueError):
            schema.parse_source(source.replace('// @lowered', ''), self.prelude)


    def test_array_recipe_shape_permission_and_result_rejections(self):
        with self.assertRaises(ValueError):
            schema.array_recipe_contracts('U')
        cases = [
            ('map(fn: fn(item: T, index: i64) -> U) -> Array<U>',
             'map(fn: fn(item: T, index: i64) -> Unknown) -> Array<Unknown>'),
            ('map(fn: fn(item: T, index: i64) -> U) -> Array<U>',
             'map(fn: fn(item: T, index: i64) -> T) -> Array<T>'),
            ('map(fn: fn(item: T, index: i64) -> U) -> Array<U>',
             'map(fn: fn(item: T, index: i64) -> U) -> Array<T>'),
            ('filter(fn: fn(item: T, index: i64) -> bool)', 'filter(fn: fn(item: T, index: i64) -> i64)'),
            ('reduce(fn: fn(acc: U, item: T) -> U, initial: U) -> U',
             'reduce(fn: fn(acc: T, item: T) -> T, initial: T) -> T'),
            ('reduce(fn: fn(acc: U, item: T) -> U, initial: U) -> U',
             'reduce(fn: fn(acc: U, item: T) -> U, initial: T) -> U'),
            ('reduce(fn: fn(acc: U, item: T) -> U, initial: U) -> U',
             'reduce(initial: U, fn: fn(acc: U, item: T) -> U) -> U'),
            ('forEach(fn: fn(item: T, index: i64))', 'forEach(fn: fn(item: T, index: i64) -> T)'),
            ('find(fn: fn(item: T) -> bool) -> T?', 'find(fn: fn(item: T) -> bool) -> T'),
            ('findIndex(fn: fn(item: T) -> bool) -> i64', 'findIndex(fn: fn(item: T) -> bool) -> bool'),
            ('every(fn: fn(item: T) -> bool)', 'every(fn: fn(item: T, index: i64) -> bool)'),
            ('some(fn: fn(item: T) -> bool)', 'some(fn: fn(item: T) -> U)'),
            ('contains(value: T)', 'contains(value?: T)'),
            ('indexOf(value: T)', 'indexOf(...value: T)'),
            ('join(separator?: string)', 'join(separator: string)'),
            ('join(separator?: string)', 'join(separator?: T)'),
            ('ref pop() -> T?', 'pop() -> T?'),
            ('ref pop() -> T?', 'ref pop() -> T'),
            ('ref pop() -> T?', 'ref pop() -> T??'),
            ('ref pop() -> T?', 'ref pop(value: T) -> T?'),
            ('ref shift() -> T?', 'shift() -> T?'),
            ('ref shift() -> T?', 'ref shift(value?: T) -> T?'),
            ('ref shift() -> T?', 'ref shift() -> Unknown?'),
            ('ref clear()', 'clear()'),
            ('ref clear()', 'ref clear(value: T)'),
            ('ref reverse() -> Array<T>', 'reverse() -> Array<T>'),
            ('ref reverse() -> Array<T>', 'ref reverse() -> T'),
            ('ref reverse() -> Array<T>', 'ref reverse() -> Array<Unknown>'),
            ('ref reverse() -> Array<T>', 'ref reverse(value: T) -> Array<T>'),
            ('ref unshift(value: T)', 'unshift(value: T)'),
            ('ref unshift(value: T)', 'ref unshift(value?: T)'),
            ('ref unshift(value: T)', 'ref unshift(...value: T)'),
            ('ref unshift(value: T)', 'ref unshift(value: Unknown)'),
            ('ref unshift(value: T)', 'ref unshift(value: T) -> Array<T>'),
            ('ref unshift(value: T)', 'ref unshift(value: T, index: i64)'),
            ('ref resize(length: i64, fill: T) -> Array<T>', 'resize(length: i64, fill: T) -> Array<T>'),
            ('ref resize(length: i64, fill: T) -> Array<T>', 'ref resize(length: i32, fill: T) -> Array<T>'),
            ('ref resize(length: i64, fill: T) -> Array<T>', 'ref resize(length: i64, fill?: T) -> Array<T>'),
            ('ref resize(length: i64, fill: T) -> Array<T>', 'ref resize(length: i64, ...fill: T) -> Array<T>'),
            ('ref resize(length: i64, fill: T) -> Array<T>', 'ref resize(fill: T, length: i64) -> Array<T>'),
            ('ref resize(length: i64, fill: T) -> Array<T>', 'ref resize(length: i64, fill: Unknown) -> Array<T>'),
            ('ref resize(length: i64, fill: T) -> Array<T>', 'ref resize(length: i64, fill: T) -> T'),
            ('entries() -> Array<(i64, T)>', 'ref entries() -> Array<(i64, T)>'),
            ('entries() -> Array<(i64, T)>', 'entries(value: T) -> Array<(i64, T)>'),
            ('entries() -> Array<(i64, T)>', 'entries() -> Array<(i32, T)>'),
            ('entries() -> Array<(i64, T)>', 'entries() -> Array<(T, i64)>'),
            ('entries() -> Array<(i64, T)>', 'entries() -> Array<(i64, Unknown)>'),
            ('entries() -> Array<(i64, T)>', 'entries() -> Array<T>'),
            ('entries() -> Array<(i64, T)>', 'entries() -> Array<(i64, T?)>'),
            ('struct Array<T>', 'struct Array<U>'),
        ]
        for before, after in cases:
            with self.subTest(before=before, after=after), self.assertRaises(ValueError):
                schema.parse_source(self.source.replace(before, after), self.prelude)
        _, members = schema.parse_source(self.source, self.prelude)
        for member in members:
            if member.operation not in schema.array_recipe_contracts('T'):
                continue
            declaration = ('ref ' if member.receiver == 'REF' else '') + member.name + member.signature
            marker = (f'// @native-operation {member.operation} allocation={member.allocation} '
                      f'failures={member.failures} ownership={member.ownership}')
            mutations = [
                (declaration, ('move ' if member.receiver == 'REF' else 'ref ') + member.name + member.signature),
                (declaration, 'static ' + member.name + member.signature),
                (marker, marker.replace('allocation=may_heap', 'allocation=no_heap')),
                (marker, marker.replace(f'failures={member.failures}', 'failures=none')),
                (marker, marker.replace(f'ownership={member.ownership}',
                                        'ownership=owned' if member.ownership == 'unit' else 'ownership=unit')),
                (marker, ''),
            ]
            for before, after in mutations:
                with self.subTest(operation=member.operation, mutation=after), self.assertRaises(ValueError):
                    schema.parse_source(self.source.replace(before, after), self.prelude)

    def test_string_value_identity_and_full_member_spans(self):
        source = (ROOT / 'stdlib/types/string.xr').read_text(encoding='utf-8')
        header, members = schema.parse_source(source, self.prelude)
        self.assertEqual(header[:3], (2, 'string', ()))
        self.assertEqual(len(members), 19)
        rows = source.splitlines()
        self.assertEqual(rows[header[3] - 1][header[4] - 1:], 'string {')
        for member in members:
            self.assertTrue(rows[member.line - 1][member.column - 1:].startswith(member.name))
        admitted = [m for m in members if m.operation != 'NONE']
        self.assertEqual({m.name for m in admitted}, {'contains', 'startsWith', 'endsWith', 'indexOf', 'lastIndexOf', 'toString', 'repeat', 'copyBytes', 'replace', 'replaceAll', 'slice'})
        for member in admitted:
            self.assertEqual(schema.simple_term(member.result, ''), 'I64' if member.name in {'indexOf', 'lastIndexOf'} else 'ARRAY_U8' if member.name=='copyBytes' else 'STRING' if member.name in {'toString','repeat','replace','replaceAll','slice'} else 'BOOL')
            self.assertEqual(member.allocation, 'may_heap' if member.name in {'repeat','copyBytes','replace','replaceAll','slice'} else 'no_heap')
            self.assertEqual(member.failures, {'indexOf':'bounds,limit','lastIndexOf':'limit','toString':'retain','repeat':'allocation,retain,limit','copyBytes':'allocation,retain,limit','replace':'allocation,retain,limit','replaceAll':'allocation,retain,limit','slice':'bounds,allocation,retain,limit'}.get(member.name,'none'))
        self.assertNotIn('xr_native_def_string[]', embed.render(ROOT / 'stdlib/types'))
        for before, after in [('struct string', 'class string'), ('id=2', 'id=1'),
                              ('struct string {', 'struct string<T> {'),
                              ('contains(search: string)', 'contains(search: i64)'),
                              ('startsWith(search: string)', 'ref startsWith(search: string)'),
                              ('failures=none', 'failures=allocation'),
                              ('-> bool', '-> string'),
                              ('STRING_ENDS_WITH', 'ARRAY_PUSH'),
                              ('InvalidUtf8', 'InvalidUtf8('),
                              ('start?: i64', 'start: i64'), ('start?: i64', 'start?: string'),
                              ('indexOf(search: string', 'indexOf(search?: string'),
                              ('lastIndexOf(search: string', 'lastIndexOf(search?: string'),
                              ('failures=bounds,limit', 'failures=none'),
                              ('repeat(count: i64)', 'repeat(count: u64)'),
                              ('toString() -> string', 'ref toString() -> string'),
                              ('STRING_REPEAT allocation=may_heap', 'STRING_REPEAT allocation=no_heap'),
                              ('slice(start: i64, end?: i64)', 'slice(start: i32, end?: i64)'),
                              ('slice(start: i64, end?: i64)', 'slice(start: i64, end: i64)'),
                              ('STRING_SLICE allocation=may_heap', 'STRING_SLICE allocation=no_heap'),
                              ('STRING_SLICE allocation=may_heap failures=bounds,allocation,retain,limit',
                               'STRING_SLICE allocation=may_heap failures=allocation,retain,limit')]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                schema.parse_source(source.replace(before, after), self.prelude)

    def test_reject_class_or_unregistered_type(self):
        for source, prelude in ((self.source.replace('struct Array', 'class Array'), self.prelude),
                                (self.source.replace('struct Array', 'struct Set'), self.prelude),
                                (self.source.replace('id=1', 'id=2'), self.prelude),
                                (self.source, self.prelude.replace('("Array",', '("Other",'))):
            with self.assertRaises(ValueError):
                schema.parse_source(source, prelude)

    def test_reject_wrong_admitted_signatures_and_contracts(self):
        cases = [('get(index: i64)', 'get(index: i32)'),
                 ('ref set(index:', 'set(index:'),
                 ('push(value: T) -> ()', 'push(value: U) -> ()'),
                 ('get(index: i64) -> T', 'get(index: i64) -> i64'),
                 ('ARRAY_SET allocation=may_heap', 'ARRAY_SET allocation=no_heap'),
                 ('ARRAY_GET allocation=may_heap', 'ARRAY_GET allocation=no_heap'),
                 ('failures=bounds,allocation,retain,limit', 'failures=bounds,retain,limit'),
                 ('ownership=owned', 'ownership=unit')]
        for before, after in cases:
            with self.subTest(before=before), self.assertRaises(ValueError):
                schema.parse_source(self.source.replace(before, after), self.prelude)

    def test_unused_rows_still_require_valid_syntax_and_unique_names(self):
        for before, after in [('iterator() -> Iterator<T>', 'iterator() -> Iterator<T'),
                              ('capacity: i64', 'get(index: i64) -> T'),
                              ('map(fn:', 'map(value@fn:')]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                schema.parse_source(self.source.replace(before, after), self.prelude)

    def test_reject_missing_duplicate_and_trailing_identity(self):
        for source in (self.source.replace('ARRAY_PUSH', 'UNKNOWN_OP'),
                       self.source.replace('ARRAY_SET', 'ARRAY_GET'),
                       self.source + '\nclass Imposter {}\n',
                       self.source.replace('// @native-type id=1', '')):
            with self.assertRaises(ValueError):
                schema.parse_source(source, self.prelude)

    def test_legacy_embedding_preserves_unchanged_outputs(self):
        with tempfile.TemporaryDirectory(prefix='xray-native-embedding-') as directory:
            root = Path(directory)
            source = root / 'atomic.xr'
            source.write_text('class Atomic<T> {\n    load() -> T\n}\n', encoding='utf-8')
            output = root / 'generated.inc.c'
            command = [sys.executable, str(Path(embed.__file__)), str(root), '--output', str(output)]
            subprocess.run(command, check=True, capture_output=True)
            for newline in (b'\n', b'\r\n'):
                output.write_bytes(output.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', newline))
                os.utime(output, ns=(1600000000123456700, 1600000000123456700))
                expected = output.read_bytes(), output.stat().st_mtime_ns
                subprocess.run(command, check=True, capture_output=True)
                self.assertEqual((output.read_bytes(), output.stat().st_mtime_ns), expected)
            source.write_text('// Changed provenance.\n' + source.read_text(encoding='utf-8'), encoding='utf-8')
            subprocess.run(command, check=True, capture_output=True)
            self.assertIn('Changed provenance', output.read_text(encoding='utf-8'))
            self.assertNotEqual(output.stat().st_mtime_ns, expected[1])
            subprocess.run([sys.executable, str(Path(embed.__file__)), str(root), '--check', str(output)],
                           check=True, capture_output=True)

    def test_iterator_identity_has_exact_pull_recipes(self):
        rows = schema.render_iterator_identity(ROOT)
        self.assertEqual(rows, schema.render_iterator_identity(ROOT))
        projected = '\n'.join(rows)
        self.assertIn('9, XR_NATIVE_DECLARATION_IDENTITY, "Iterator", xr_native_iterator_type_parameters, 2', projected)
        self.assertIn('xr_native_iterator_type_parameters[] = {"T", "E"};', projected)
        self.assertIn('() throws(E) -> bool', projected)
        self.assertIn('() throws(E) -> T', projected)
        self.assertEqual(projected.count('XR_NATIVE_OPERATION_NONE'), 2)
        self.assertEqual(projected.count('XR_NATIVE_OPERATION_ITERATOR_HAS_NEXT'), 1)
        self.assertEqual(projected.count('XR_NATIVE_OPERATION_ITERATOR_NEXT'), 1)
        self.assertEqual(projected.count('XR_NATIVE_OPERATION_ITERATOR_NTH'), 1)
        self.assertNotIn('XR_NATIVE_OPERATION_ARRAY_', projected)
        with tempfile.TemporaryDirectory(prefix='xray-iterator-identity-') as directory:
            root = Path(directory)
            for relative in ['stdlib/types/iterator.xr', 'stdlib/prelude/builtin_symbols.def']:
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / relative).read_bytes())
            source = root / 'stdlib/types/iterator.xr'
            original = source.read_text(encoding='utf-8')
            self.assertEqual(schema.render_iterator_identity(root), rows)
            for old, new in [('next() throws(E) -> T', 'next() throws(E) -> i64'),
                             ('Iterator<T, E: Error = Never>', 'Iterator<T, E: Error>'),
                             ('hasNext() throws(E)', 'hasNext()'),
                             ('next() throws(E)', 'next() throws(Error)'),
                             ('iterator() -> Iterator<T, E>', 'iterator() -> Iterator<T>'),
                             ('// @lowered\n    nth(', '// removed\n    nth('),
                             ('// @lowered\n    hasNext()', '// removed\n    hasNext()'),
                             ('// Built-in', '// @native-type id=6\n// Built-in')]:
                source.write_text(original.replace(old, new, 1), encoding='utf-8')
                with self.assertRaises(ValueError):
                    schema.render_iterator_identity(root)

    def test_retired_native_declaration_identity_cannot_be_reintroduced(self):
        retired = """// @native-type id=16
class StringBuilder {
    // @native-operation STRING_BUILDER_APPEND allocation=may_heap failures=allocation,retain,limit ownership=owned
    append(value: string | rune | i64 | f64 | bool | null) -> StringBuilder
    // @native-operation STRING_BUILDER_TO_STRING allocation=no_heap failures=retain,limit ownership=owned
    toString() -> string
    // @native-operation STRING_BUILDER_CLEAR allocation=no_heap failures=retain,limit ownership=owned
    clear() -> StringBuilder
}
"""
        forged_binding = '\nXR_BUILTIN_PRELUDE_TYPE("StringBuilder", 0, XR_TINSTANCE, SIMPLE)\n'
        for prelude in (self.prelude, self.prelude + forged_binding):
            with self.subTest(forged_binding=prelude != self.prelude):
                with self.assertRaises(ValueError):
                    schema.parse_source(retired, prelude)

    def test_projection_omits_retired_class_and_keeps_range(self):
        outputs = schema.render(ROOT)
        prelude = outputs['src/shared/xprelude_source.inc.c']
        declarations = outputs['src/shared/xnative_declarations.inc.c']
        self.assertNotIn('class StringBuilder', prelude)
        self.assertNotIn('xray-native:prelude/StringBuilder', declarations)
        self.assertIn('class Range', prelude)

    def test_generation_preserves_unchanged_outputs(self):
        with tempfile.TemporaryDirectory(prefix='xray-native-generation-') as directory:
            root = Path(directory)
            for relative in ['stdlib/types/array.xr', 'stdlib/types/string.xr', 'stdlib/types/atomic.xr',
                             'stdlib/types/coroutine.xr', 'stdlib/types/channel.xr', 'stdlib/types/iterator.xr',
                             'stdlib/types/i64.xr', 'stdlib/types/f64.xr', 'stdlib/types/bool.xr',
                             'stdlib/types/rune.xr', 'stdlib/types/map.xr', 'stdlib/types/set.xr',
                             'stdlib/prelude/range.xr', 'stdlib/prelude/builtin_symbols.def']:
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / relative).read_bytes())
            outputs = schema.render(root)
            for relative in outputs:
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
            command = [sys.executable, str(ROOT / 'scripts/gen_native_declarations.py'),
                       '--root', str(root)]
            subprocess.run(command, check=True, capture_output=True)
            # --check already accepts equivalent line endings. Generating must
            # not dirty a valid checkout or invalidate its compiler dependents.
            first = root / next(iter(outputs))
            first.write_bytes(first.read_bytes().replace(b'\n', b'\r\n'))
            expected = {}
            for relative in outputs:
                target = root / relative
                os.utime(target, ns=(1600000000123456700, 1600000000123456700))
                expected[relative] = (target.read_bytes(), target.stat().st_mtime_ns)
            subprocess.run(command + ['--check'], check=True, capture_output=True)
            subprocess.run(command, check=True, capture_output=True)
            for relative, (content, timestamp) in expected.items():
                target = root / relative
                self.assertEqual(target.read_bytes(), content, relative)
                self.assertEqual(target.stat().st_mtime_ns, timestamp, relative)

            source = root / 'stdlib/types/array.xr'
            source.write_text('// Changed source provenance.\n' + self.source, encoding='utf-8')
            wanted = schema.render(root)
            self.assertNotEqual(wanted, outputs)
            self.assertNotEqual(subprocess.run(command + ['--check'], capture_output=True).returncode, 0)
            subprocess.run(command, check=True, capture_output=True)
            subprocess.run(command + ['--check'], check=True, capture_output=True)
            for relative, text in wanted.items():
                target = root / relative
                self.assertEqual(target.read_text(encoding='utf-8'), text)
                if text == outputs[relative]:
                    self.assertEqual(target.stat().st_mtime_ns, expected[relative][1], relative)
                else:
                    self.assertNotEqual(target.stat().st_mtime_ns, expected[relative][1], relative)

    def test_scalar_complete_contracts_and_fail_closed_mutations(self):
        for stem, identity, total, admitted in [('f64',13,12,8),('bool',14,1,1),('rune',15,6,2)]:
            source = (ROOT / 'stdlib/types' / (stem+'.xr')).read_text(encoding='utf-8')
            header, members = schema.parse_source(source, self.prelude)
            self.assertEqual((identity,stem,()),header[:3])
            self.assertEqual(total,len(members))
            self.assertEqual(admitted,len([m for m in members if m.operation!='NONE']))
            self.assertNotIn('xr_native_def_'+stem,embed.render(ROOT / 'stdlib/types'))
        bad = [
            ('f64','abs() -> f64','abs() -> i64'),
            ('f64','FLOAT_TO_I64 allocation=no_heap failures=numeric_range','FLOAT_TO_I64 allocation=no_heap failures=none'),
            ('f64','FLOAT_FLOOR allocation=no_heap','FLOAT_FLOOR allocation=may_heap'),
            ('f64','FLOAT_IS_NAN','UNKNOWN_OPERATION'),
            ('bool','toString() -> string','toString() -> bool'),
            ('rune','toUInt32() -> u32','toUInt32() -> i64'),
            ('rune','toString() -> string','ref toString() -> string'),
        ]
        for stem, before, after in bad:
            source=(ROOT / 'stdlib/types' / (stem+'.xr')).read_text(encoding='utf-8')
            self.assertIn(before,source)
            with self.subTest(stem=stem,before=before), self.assertRaises(ValueError):
                schema.parse_source(source.replace(before,after),self.prelude)


    def test_channel_identity_covers_complete_source_with_exact_execution_recipes(self):
        with tempfile.TemporaryDirectory(prefix='xray-channel-identity-') as directory:
            root = Path(directory)
            for relative in ['stdlib/types/channel.xr', 'stdlib/prelude/builtin_symbols.def']:
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / relative).read_bytes())
            source_path = root / 'stdlib/types/channel.xr'
            source = source_path.read_text(encoding='utf-8')
            original = '\n'.join(schema.render_channel_identity(root))
            self.assertIn('6, XR_NATIVE_DECLARATION_IDENTITY, "Channel", xr_native_channel_type_parameters, 1,', original)
            self.assertIn('xr_native_channel_type_parameters[] = {"T"};', original)
            self.assertEqual(original.count('XR_NATIVE_OPERATION_NONE'), 0)
            for operation in ['CHANNEL_CAPACITY', 'CHANNEL_IS_CLOSED', 'CHANNEL_CLOSE', 'CHANNEL_TRY_SEND',
                              'CHANNEL_TRY_RECV', 'CHANNEL_SEND', 'CHANNEL_RECV', 'CHANNEL_RECV_OR',
                              'CHANNEL_SEND_TIMEOUT', 'CHANNEL_RECV_TIMEOUT']:
                self.assertEqual(original.count('XR_NATIVE_OPERATION_' + operation + ','), 1)
            for member in ['sendTimeout', 'recvTimeout']:
                rows = [row for row in original.splitlines() if '"' + member + '",' in row]
                self.assertEqual(len(rows), 1)
                operation = 'CHANNEL_SEND_TIMEOUT' if member == 'sendTimeout' else 'CHANNEL_RECV_TIMEOUT'
                self.assertIn('XR_NATIVE_OPERATION_' + operation, rows[0])
                self.assertIn('allocation,retain,limit,numeric_range,host', rows[0])
            for before, after in [('send(value: T)', 'send(value: i64)'),
                                  ('sendTimeout(value: T, timeout: i64)', 'sendTimeout(value: T, timeout: i32)'),
                                  ('recvTimeout(timeout: i64)', 'recvTimeout()'),
                                  ('recvTimeout(timeout: i64) -> Recv<T>', 'recvTimeout(timeout: i64) -> T'),
                                  ('capacity: i64', 'capacity: i32'),
                                  ('recv() -> Recv<T>', 'recv() -> T'),
                                  ('class Channel<T>', 'class Channel'),
                                  ('close()', 'ref close()'),
                                  ('class Channel<T>', '// @native-operation CHANNEL_NEW\nclass Channel<T>')]:
                with self.subTest(before=before):
                    mutated = source.replace(before, after)
                    self.assertNotEqual(mutated, source)
                    source_path.write_text(mutated, encoding='utf-8')
                    with self.assertRaises(ValueError):
                        schema.render_channel_identity(root)
            source_path.write_text('// Full source provenance.\n' + source, encoding='utf-8')
            self.assertNotEqual('\n'.join(schema.render_channel_identity(root)), original)

    def test_channel_result_registry_has_exact_payload_and_variant_authority(self):
        with tempfile.TemporaryDirectory(prefix='xray-channel-results-') as directory:
            root = Path(directory)
            target = root / 'stdlib/prelude/builtin_symbols.def'
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(self.prelude, encoding='utf-8')
            _, rows = schema.channel_result_rows(root)
            self.assertEqual(rows[-2:], [
                ('Recv', 'RECV', [('Value', 'TYPE_PARAM_0'), ('Empty', 'NONE'),
                                  ('Timeout', 'NONE'), ('Closed', 'NONE')], 7, 1),
                ('SendResult', 'SEND_RESULT', [('Sent', 'NONE'), ('Full', 'NONE'),
                                              ('Timeout', 'NONE'), ('Closed', 'NONE')], 8, 0)])
            for before, after in [('"Recv", 1, RECV', '"Recv", 0, RECV'),
                                  ('"Recv", 1, RECV', '"Recv", 1, SEND_RESULT'),
                                  ('"Value", TYPE_PARAM_0', '"Value", NONE'),
                                  ('"Value", TYPE_PARAM_0', '"Other", TYPE_PARAM_0'),
                                  ('"Sent", NONE', '"Sent", TYPE_PARAM_0'),
                                  ('"SendResult", 0, SEND_RESULT', '"SendResult", 1, SEND_RESULT')]:
                with self.subTest(before=before):
                    mutated = self.prelude.replace(before, after)
                    self.assertNotEqual(mutated, self.prelude)
                    target.write_text(mutated, encoding='utf-8')
                    with self.assertRaises(ValueError):
                        schema.channel_result_rows(root)
            target.write_text(self.prelude + '\nXR_BUILTIN_ENUM("Recv", 1, RECV,)\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                schema.channel_result_rows(root)


if __name__ == '__main__':
    unittest.main()
