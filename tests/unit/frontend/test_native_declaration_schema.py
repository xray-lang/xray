"""Verify source-backed native value declarations before compiler admission."""
from pathlib import Path
import sys
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


if __name__ == '__main__':
    unittest.main()
