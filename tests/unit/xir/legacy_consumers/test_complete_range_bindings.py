"""Check whole original Range traces before any product execution."""
import unittest
import independent_complete_models as complete
import complete_text_range_model as ranges


class CompleteRangeBindings(unittest.TestCase):
    def test_complete_precedence_and_iteration(self):
        values = ranges.observations()
        self.assertEqual(values['range_precedence'], [
            '0..4', '0..2', '0..6', '1..=4', '2..9', '0..3', '0..4',
            6, 60, True, True])
        self.assertEqual(values['ranges'], [
            7, True, True, False, 2, None, None, None, '2..9', 7, 2, 8, 35])

    def test_binding_keeps_both_whole_original_programs(self):
        rows = {row['id']: row for row in complete.expected_rows()}
        for name, count in [('range_precedence', 11), ('ranges', 13)]:
            row = rows['complete_original_text_range_' + name]
            self.assertEqual(row['source'], 'tests/diff/cases/basic/' + name + '.xr')
            self.assertEqual(row['original_source'], row['source'])
            self.assertTrue(row['original_run_corpus'])
            self.assertEqual(row['observation_count'], count)
            self.assertEqual(row['expected']['exit_code'], 0)
            self.assertEqual(row['expected']['stderr_utf8'], '')
        self.assertEqual(rows['complete_original_text_range_ranges']['expected']['stdout_utf8'],
                         '7\ntrue\ntrue\nfalse\n2\nnull\nnull\nnull\n2..9\n7\n2\n8\n35\n')


if __name__ == '__main__':
    unittest.main()
