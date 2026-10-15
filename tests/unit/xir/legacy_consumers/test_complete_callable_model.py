"""Pin complete independent traces and state distinctions before product runs."""
import math
import unittest
import complete_callable_model as model


class CallableTraces(unittest.TestCase):
    def test_complete_program_traces(self):
        expected = {
            'call_through_function_parameter':'42\n6\n',
            'capture_forms':'hi!\n1\n2\n3\n1\n4\n15\n104\n12\n',
            'nested_closure_transitive_capture':'21\n',
            'bare_lambda_expression_positions':'42\n310\n42\n',
            'default_param_as_function_value':'a:8080\nb:9\n',
            'function_type_surface':'42\n42\n20\n-10\n12\n-6\n36\n1001\n[100, 200, 300]\n',
            'lambda_forms':'42\n42\n7\n12\n99\n50\n42\n7\n20\n10\n30\n101\n',
            'lambda_inference_sources':'3\n12\n8\n9\n1.0\n10\n20\n10\n11\n11\n',
            'named_function_forms':'7\nlog explicit-unit\npong\nlocalhost:8080 tls=false\nlocalhost:443 tls=false\nlocalhost:443 tls=true\n3\n2\n42\nxyz\n15\n',
            'parenthesized_function_type':'1\n42\ntrue\n2\n',
            'string_result_direct_call':'pos\nneg\npos\npos\n',
            'unit_return_expression':'inner\ninner\n1\n2\n',
            'unit_value_rendering':'()\n(1, 2)\n',
        }
        traces = model.observations()
        self.assertEqual(set(traces),set(expected))
        self.assertEqual(sum(map(len,traces.values())),73)
        for name,text in expected.items():
            with self.subTest(program=name):
                self.assertEqual(''.join(model.render(value)+'\n' for value in traces[name]),text)

    def test_factory_cells_are_independent(self):
        first=model.counter();second=model.counter()
        self.assertEqual([first(),first(),second(),first(),second()],[1,2,1,3,2])

    def test_unit_and_singleton_keep_tuple_identity(self):
        self.assertEqual(model.render(()),'()')
        self.assertEqual(model.render((1,)),'(1,)')
        self.assertEqual(model.render([1,2]),'[1, 2]')
        for value in [None,object(),math.inf,math.nan]:
            with self.subTest(value_type=type(value).__name__):
                with self.assertRaises(ValueError):model.render(value)

    def test_repeated_derivation_keeps_fresh_cells(self):
        self.assertEqual(model.observations(),model.observations())


if __name__=='__main__':unittest.main()
