"""Pin full original alias values and declaration-only override admission."""
import unittest
import independent_behavior_models as model


class OriginalAliasAndOverride(unittest.TestCase):
    def test_generic_alias_values(self):
        values, _ = model.observations()['generic_type_alias']
        self.assertEqual(values, [42, 'xray', 'n=7', 3.5])
        row = next(row for row in model.expected_rows()
                   if row['id'] == 'independent_behavior_generic_type_alias')
        self.assertEqual(row['source'], 'tests/diff/cases/semantics/generic_type_alias.xr')
        self.assertEqual(row['expected'], {
            'exit_code': 0, 'stdout_utf8': '42\nxray\nn=7\n3.5\n', 'stderr_utf8': ''})
        self.assertEqual(row['observation_count'], 4)

    def test_override_declarations_do_not_call_methods(self):
        row = next(row for row in model.expected_rows()
                   if row['id'] == 'independent_behavior_oop_override_checked_valid')
        self.assertEqual(row['source'], 'tests/diff/cases/semantics/oop/override_checked_valid.xr')
        self.assertEqual(row['expected'], {
            'exit_code': 0, 'stdout_utf8': 'auto-override-ok\n', 'stderr_utf8': ''})
        self.assertEqual(row['observation_count'], 1)


if __name__ == '__main__':
    unittest.main()
