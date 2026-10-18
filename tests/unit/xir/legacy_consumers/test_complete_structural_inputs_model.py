"""Check whole-program structural observations before product execution."""
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
        self.assertEqual(model.record({'x': 2}, schema), {'x': 2, 'age': None})
        for fields in ({}, {'x': True}, {'x': 2, 'age': False}, {'x': 2, 'extra': 1}):
            with self.assertRaises(ValueError):
                model.record(fields, schema)
        self.assertEqual(model.record({'x': 2, 'extra': 1}, schema, ignore_unknown=True),
                         {'x': 2, 'age': None})

    def test_destructure_retains_values_without_consuming_source(self):
        source = {'tags': ['a', 'b'], 'inner': {'k': 99}}
        fields = model.destructure(source, {'tags': 'items', 'inner': 'nested'})
        fields['items'][0] = 'changed'
        fields['nested']['k'] = 7
        self.assertEqual(source, {'tags': ['a', 'b'], 'inner': {'k': 99}})

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
