"""Check retained nominal/JSON observations before product execution."""
import unittest
import complete_structural_inputs_model as model


class CompleteStructuralInputs(unittest.TestCase):
    def test_complete_observations(self):
        self.assertEqual(model.observations(), {
            'object_width_json_as_extra_key': [1, 2],
            'object_width_nullable_field_omission': ['Ann', 30, 'Bo', True],
            'object_destructure': [3, 4, 'Ada', 37, 'a', 'b', 'Ada', 37, 10, 20, 1, 99],
            'tbaa_raw_pointer_store_kills_array_load': [406, 103],
            'ws_connect_options_type_surface': [True],
            'string_from_utf8_slice': ['bin'],
            'slice_reinterpret_value_struct': [1, 1, 2],
        })

    def test_optional_and_unknown_field_boundaries(self):
        schema = {'x': (int, False), 'age': (int, True)}
        self.assertEqual(model.decode_schema_fields({'x': 2}, schema), {'x': 2, 'age': None})
        for fields in ({}, {'x': True}, {'x': 2, 'age': False}, {'x': 2, 'extra': 1}):
            with self.assertRaises(ValueError):
                model.decode_schema_fields(fields, schema)
        self.assertEqual(model.decode_schema_fields({'x': 2, 'extra': 1}, schema, ignore_unknown=True),
                         {'x': 2, 'age': None})

    def test_explicit_field_copies_retain_values_without_consuming_source(self):
        source=model.User('Ada',37,['a','b'])
        tags,=model.field_copy(source,'tags');tags[0]='changed'
        outer=model.OuterValue(1,model.InnerValue(99))
        inner,=model.field_copy(outer,'inner');inner.k=7
        self.assertEqual(source.tags,['a','b']);self.assertEqual(outer.inner.k,99)
        with self.assertRaisesRegex(ValueError,'declaration identity'):
            model.field_copy({'name':'Ada','age':37,'tags':['a','b']},'tags')
        with self.assertRaisesRegex(ValueError,'undeclared'):model.field_copy(source,'unknown')
        self.assertFalse(hasattr(model,'destructure'));self.assertFalse(hasattr(model,'record'))

    def test_nullable_omission_is_retained_as_a_legal_nominal_duty(self):
        self.assertEqual(model.Opt('Bo').age,None)
        self.assertEqual(model.Opt('Ann',30).age,30)
        with self.assertRaises(TypeError):model.Opt()
        for age in [True,'30',1<<63]:
            with self.assertRaises(ValueError):model.Opt('Ann',age)

    def test_alias_load_cannot_be_hoisted(self):
        self.assertEqual(model.clobbered_array_loads(0), (0, 1))
        self.assertEqual(model.clobbered_array_loads(1), (100, 100))
        self.assertEqual(model.clobbered_array_loads(4), (406, 103))
        self.assertNotEqual(model.clobbered_array_loads(4)[0], 4)

    def test_typed_pair_layout_uses_every_complete_element(self):
        self.assertEqual(model.decode_pairs(bytes(range(1, 17))),
                         [(67305985, 134678021), (202050057, 269422093)])
        with self.assertRaises(ValueError):
            model.decode_pairs(bytes(7))

    def test_every_original_is_bound_and_remains_a_positive_program(self):
        rows = model.expected_rows()
        self.assertEqual({row['source'] for row in rows}, set(model.PATHS.values()))
        self.assertEqual(sum(row['observation_count'] for row in rows), 25)
        for row in rows:
            self.assertEqual(row['source'], row['original_source'])
            self.assertTrue(row['original_run_corpus'])
            self.assertEqual(row['expected']['exit_code'], 0)
            self.assertEqual(row['expected']['stderr_utf8'], '')
        pair = next(row for row in rows if row['id'].endswith('slice_reinterpret_value_struct'))
        self.assertEqual(pair['required_target'], 'windows-x86_64')


if __name__ == '__main__':
    unittest.main()
