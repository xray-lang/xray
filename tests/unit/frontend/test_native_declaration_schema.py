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
                         {'ARRAY_GET', 'ARRAY_SET', 'ARRAY_PUSH'})
        self.assertEqual(next(m for m in members if m.name == 'map').operation, 'NONE')
        self.assertNotIn('xr_native_def_array', embed.render(ROOT / 'stdlib/types'))

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
        self.assertEqual({m.name for m in admitted}, {'contains', 'startsWith', 'endsWith'})
        for member in admitted:
            self.assertEqual(schema.simple_term(member.result, ''), 'BOOL')
            self.assertEqual(member.allocation, 'no_heap')
            self.assertEqual(member.failures, 'none')
        self.assertNotIn('xr_native_def_string[]', embed.render(ROOT / 'stdlib/types'))
        for before, after in [('struct string', 'class string'), ('id=2', 'id=1'),
                              ('struct string {', 'struct string<T> {'),
                              ('contains(search: string)', 'contains(search: i64)'),
                              ('startsWith(search: string)', 'ref startsWith(search: string)'),
                              ('failures=none', 'failures=allocation'),
                              ('-> bool', '-> string'),
                              ('STRING_ENDS_WITH', 'ARRAY_PUSH'),
                              ('InvalidUtf8', 'InvalidUtf8(')]:
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
            for relative in ['stdlib/types/array.xr', 'stdlib/types/string.xr', 'stdlib/prelude/builtin_symbols.def']:
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
