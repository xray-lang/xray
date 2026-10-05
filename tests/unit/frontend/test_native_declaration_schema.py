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
        self.assertEqual(header[:3], (1, 'Array', 'T'))
        self.assertEqual(len(members), 32)
        self.assertEqual({m.operation for m in members if m.operation != 'NONE'},
                         {'ARRAY_GET', 'ARRAY_SET', 'ARRAY_PUSH'} | set(schema.array_recipe_contracts('T')))
        self.assertEqual(next(m for m in members if m.name == 'map').operation, 'ARRAY_MAP')
        self.assertNotIn('xr_native_def_array', embed.render(ROOT / 'stdlib/types'))

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
        clear = next(member for member in members if member.name == 'clear')
        self.assertEqual((clear.receiver, clear.ownership, clear.result), ('REF', 'unit', ('tuple', ())))
        self.assertEqual({m.name for m in members if m.operation == 'NONE'},
                         {'withCapacity', 'capacity', 'ptr', 'mutPtr',
                          'reserve', 'resize', 'concat', 'sort', 'fill', 'toString',
                          'iterator', 'entriesIterator', 'entries'})

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
        self.assertEqual(header[:3], (2, 'string', ''))
        self.assertEqual(len(members), 19)
        rows = source.splitlines()
        self.assertEqual(rows[header[3] - 1][header[4] - 1:], 'string {')
        for member in members:
            self.assertTrue(rows[member.line - 1][member.column - 1:].startswith(member.name))
        admitted = [m for m in members if m.operation != 'NONE']
        self.assertEqual({m.name for m in admitted}, {'contains', 'startsWith', 'endsWith', 'indexOf', 'lastIndexOf'})
        for member in admitted:
            self.assertEqual(schema.simple_term(member.result, ''), 'I64' if member.name in {'indexOf', 'lastIndexOf'} else 'BOOL')
            self.assertEqual(member.allocation, 'no_heap')
            self.assertEqual(member.failures, 'bounds,limit' if member.name == 'indexOf' else 'limit' if member.name == 'lastIndexOf' else 'none')
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
                              ('failures=bounds,limit', 'failures=none')]:
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

    def test_generation_preserves_unchanged_outputs(self):
        with tempfile.TemporaryDirectory(prefix='xray-native-generation-') as directory:
            root = Path(directory)
            for relative in ['stdlib/types/array.xr', 'stdlib/types/string.xr', 'stdlib/types/atomic.xr', 'stdlib/prelude/builtin_symbols.def']:
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


if __name__ == '__main__':
    unittest.main()
