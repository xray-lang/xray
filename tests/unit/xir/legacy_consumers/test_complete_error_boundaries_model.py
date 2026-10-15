from decimal import Decimal
import unittest
import complete_error_boundaries_model as model

class ErrorBoundaries(unittest.TestCase):
    def test_complete_observations(self):
        expected={'array_index_set_append_oob':['[Uncaught Panic] E0430: array index out of range: 3 (length 3)'],
         'array_index_set_gap_oob':['[Uncaught Panic] E0430: array index out of range: 5 (length 3)'],
         'array_index_set_negative_oob':['[Uncaught Panic] E0430: array index out of range: -1 (length 3)'],
         'fixed_array_index_oob_panic_shape':['fixed array index out of range: -1 (length 4)',430,'fixed array index out of range: 4 (length 4)',430,'fixed array index out of range: 5 (length 4)',430],
         'string_number_conversion_panic_shape':[-7,'i64.InvalidSyntax','overflow.OutOfRange','1500.0','inf','f64.InvalidSyntax',True,True]}
        self.assertEqual(sum(map(len,expected.values())),17)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(model.observations()[name],values)

    def test_strict_integer_syntax_and_range(self):
        for text,variant in [('12abc','InvalidSyntax'),('0x12','InvalidSyntax'),('','InvalidSyntax'),('1.0','InvalidSyntax'),('9223372036854775808','OutOfRange'),('-9223372036854775809','OutOfRange')]:
            with self.subTest(text=text):
                with self.assertRaises(model.ParseFailure) as error:model.parse_integer(text)
                self.assertEqual(error.exception.variant,variant)
        self.assertEqual(model.parse_integer('-9223372036854775808'),-(1<<63));self.assertEqual(model.parse_integer('+9223372036854775807'),(1<<63)-1)

    def test_strict_float_grammar_and_unambiguous_values(self):
        for text in ['1e+','nan','inf','0x1p2','1.2.3','']:
            with self.subTest(text=text):
                with self.assertRaises(model.ParseFailure):model.parse_float(text)
        self.assertEqual(model.parse_float('.5'),Decimal('0.5'));self.assertEqual(model.parse_float('1.'),Decimal(1))
        self.assertEqual(model.parse_float('1.5e3'),Decimal(1500));self.assertEqual(model.render_float(model.parse_float('1e400')),'inf')

    def test_bounds_are_checked_before_success_continuations(self):
        for length,index,valid in [(3,-1,False),(3,2,True),(3,3,False),(3,5,False),(4,4,False)]:
            with self.subTest(length=length,index=index):self.assertEqual(model.checked_index(length,index) is None,valid)

if __name__=='__main__':unittest.main()
