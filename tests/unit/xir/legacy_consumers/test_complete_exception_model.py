"""Pin independent source-level cleanup ordering and channel distinctions."""
import unittest
import complete_exception_model as model

class ExceptionTraces(unittest.TestCase):
    def test_complete_traces(self):
        expected={'array_class_throw_cleanup_lifetime':[True],'core_assertion_channels':['assertion-channels-ok'],
         'defer_nested_function_call':['body','nested-cleanup','done'],'div_by_zero':[],
         'nested_defer':['inner-body','inner-defer','outer-defer','caught:boom','done'],
         'panic_edge_owner_cleanup':[1,2,'caught'],'try_catch':['try','caught:boom','after'],
         'uncaught_top_level_error':['before']}
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),17)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_wrong_channel_and_normal_return_are_assertion_panics(self):
        def typed():raise model.TypedError('E','Bad')
        def panic():raise model.Panic('bad')
        for required,action in [('error',panic),('panic',typed),('error',lambda:None),('panic',lambda:None)]:
            with self.subTest(required=required,action=action.__name__):
                with self.assertRaises(model.Panic):model.assertion_channel(required,action)
        for required,action in [('error',typed),('panic',panic)]:
            with self.subTest(required=required):self.assertIsNone(model.assertion_channel(required,action))

    def test_exact_termination_requires_runtime_boundary(self):
        for name in sorted(model._NAMES):
            with self.subTest(program=name):
                expected=(1,'[Uncaught Panic] E0420: division by zero\n') if name=='div_by_zero' else (1,'[Uncaught Error] TopErr.Failed\n') if name=='uncaught_top_level_error' else (0,'')
                self.assertEqual(model.termination(name),expected)

if __name__=='__main__':unittest.main()
