"""Check logical cleanup boundaries and lazy pull observations before product runs."""
import unittest
import complete_cleanup_generator_model as model


class CleanupGeneratorTraces(unittest.TestCase):
    def test_complete_cleanup_traces(self):
        expected={
            'defer_async_await':['before-await 7','after-await 8','defer-after 8','defer-builtin 7','close conn 7','80','done'],
            'defer_block_assignment':['body 1','cleanup 99'],
            'defer_coroutine_local_panic_handler':['cleanup caught panic','7'],
            'defer_error_absorbed_inside_body':['cleanup-error-absorbed','body-error-survives-cleanup','done'],
            'defer_late_binding':['body x=99','saved=1','late=99','done'],
            'defer_nested_cleanup_and_local_control':['body','loop 2','outer','nested'],
            'defer_panic_caught_inside_body':['body-ran','panic-absorbed-inside-defer','done'],
            'defer_receiver_late_binding':['body done','closing conn 2'],
            'defer_uncaught_panic':['inner-defer','outer-defer']}
        actual=model.cleanup_observations();self.assertEqual(actual,expected)

    def test_complete_generator_traces(self):
        expected={
            'early_stop':['-- drained','enter','produce 0','take 0','produce 1','take 1','produce 2','take 2','body-end',
                '-- break on first','enter','produce 0','take 0','-- break in the middle','enter','produce 0','take 0','produce 1','take 1','produce 2','take 2',
                '-- never driven','created','-- restart','enter','produce 0','produce 1','body-end','enter','produce 0','produce 1','body-end','sums 1 1'],
            'generator_in_coroutine':['await 10','scope 48','main 3','early 100'],
            'iterator_exhausted':['yielded 5','has false','next 432 '+model.NEXT_MESSAGE,'next-again 432','has-after false','empty-has false','empty-next 432','direct 5','nth-in-range 5','nth 432 '+model.NTH_MESSAGE],
            'iterator_param_loop_carried':['10','0','1','130','10'],
            'nested_generator':['-- drained','inner 0','outer 0','take 0','inner 1','outer 1','take 2','inner 2','outer 2','take 4','inner-end','outer-end','sum 6',
                '-- outer abandoned','inner 0','outer 0','take 0','inner 1','outer 1','take 2',
                '-- three levels','inner 0','outer 0','deep 1','inner 1','outer 1','deep 3','inner-end','outer-end'],
            'value_yield':['start 3','yield 0','take 0','yield 1','take 1','yield 2','take 2','done 3','sum 3','start 5','yield 0','partial 0','yield 1','partial 1','partial-sum 1'],
            'yield_throw':['x 1','caught boom']}
        actual=model.generator_observations();self.assertEqual(set(actual),set(expected))
        for name,lines in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],lines)

    def test_repeated_peek_and_sticky_exhaustion(self):
        effects=[]
        def source():effects.append('first');yield 5;effects.append('end')
        pull=model.Pull(source());self.assertEqual(effects,[])
        self.assertTrue(pull.has_next());self.assertTrue(pull.has_next());self.assertEqual(effects,['first'])
        self.assertEqual(pull.next(),5);self.assertFalse(pull.has_next());self.assertFalse(pull.has_next())
        self.assertEqual(effects,['first','end'])
        for operation,message in [(pull.next,model.NEXT_MESSAGE),(lambda:pull.nth(3),model.NTH_MESSAGE)]:
            with self.subTest(message=message):
                with self.assertRaisesRegex(model.PanicChannel,'Iterator'):operation()

    def test_error_channel_and_unwind_order(self):
        events=[]
        with self.assertRaises(model.ValueErrorChannel):
            with model.CleanupScope() as outer:
                outer.defer(lambda:events.append('outer'))
                with model.CleanupScope() as inner:
                    inner.defer(lambda:events.append('inner'));raise model.ValueErrorChannel('body')
        self.assertEqual(events,['inner','outer'])
        self.assertEqual(model.observations(),model.observations())


if __name__=='__main__':unittest.main()
