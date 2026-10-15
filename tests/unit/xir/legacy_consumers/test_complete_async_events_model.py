import unittest
import complete_async_events_model as model

class AsyncEventTraces(unittest.TestCase):
    def test_complete_traces(self):
        expected={'condvar_compose':[42,3,'done'],'go_lambda_local_scope':[10],'mutex_cross_coroutine':[100],
         'await_non_rethrowing_forms_observe':['failed','failed',5],'await_tuple_destructure':[10,20,7],
         'await_value_error_keeps_channel':['caught direct: E.Boom','caught at await: E.Boom','caught propagated: E.Boom',14],
         'channel_for_in_recv_or':[15,42,-1,'xray','fallback'],'channel_recv_timeout_unbuffered':['timeout'],'channel_send_timeout_unbuffered':['timeout'],
         'coroutine_throws_unit_variant':['failed: unit variant','failed: payload variant'],'inline_match_try_recv_heap':[10,-1],
         'lazy_string_boundary':[1,11,True,True,False,1,True,True,False,'chan_2','task_3','seed_4_value','seed_4_value'],
         'linked_scope_error_propagates_at_exit':['caught in scope frame: E.Boom','caught from caller: E.Boom',5],
         'scope_child_failure_isolation':[11],'semaphore_double_acquire_defer_bool_slot':[7,1],
         'task_transfer_return_local':[3],'task_transfer_return_map_set':[11,True]}
        actual=model.observations();self.assertEqual(set(actual),set(expected));self.assertEqual(sum(map(len,actual.values())),48)
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(actual[name],values)

    def test_sticky_values_and_two_failure_channels(self):
        calls=[];task=model.Task(lambda:(calls.append('run'),[1,2,3])[1]);first=task.await_value();first[0]=99
        self.assertEqual(task.await_value(),[1,2,3]);self.assertEqual(calls,['run'])
        def panic():raise model.Panic('boom')
        for action,error_type in [(lambda:model.failure(),model.TypedError),(panic,model.Panic)]:
            with self.subTest(channel=error_type.__name__):
                task=model.Task(action);self.assertEqual(task.observe()[0],'Failed')
                with self.assertRaises(error_type):task.await_value()
                self.assertEqual(task.observe()[0],'Failed')
        failed,other=model.Task(lambda:model.failure()),model.Task(lambda:3)
        with self.assertRaises(model.TypedError):model.all_values([failed,other])
        self.assertEqual(other.state,'Cancelled')

    def test_plain_and_linked_scope_policy(self):
        for linked in [False,True]:
            with self.subTest(linked=linked):
                events=[];scope=model.Scope(linked);scope.add(lambda:model.failure());sibling=scope.add(lambda:events.append('sibling'))
                if linked:
                    with self.assertRaises(model.TypedError):scope.close()
                    self.assertEqual(sibling.state,'Cancelled');self.assertEqual(events,[])
                else:scope.close();self.assertEqual(events,['sibling'])

    def test_channel_fifo_copy_close_and_timeout(self):
        for operation,expected in [('recv','Timeout'),('send','Timeout'),('closed','Closed')]:
            with self.subTest(operation=operation):
                channel=model.Channel(0)
                if operation=='closed':channel.close()
                actual=channel.timed_send_without_partner(7) if operation=='send' else channel.timed_recv_without_partner()[0]
                self.assertEqual(actual,expected);self.assertEqual(channel.queue,[])
        channel=model.Channel(2);value=[1,2];channel.send(value);value[0]=99;channel.send([3]);channel.close()
        self.assertEqual(channel.recv(),('Value',[1,2]));self.assertEqual(channel.recv(),('Value',[3]));self.assertEqual(channel.recv(),('Closed',None))

    def test_barrier_paths_and_semaphore_integer_result(self):
        paths=model.condvar_observation_paths();self.assertEqual(len(paths),96);self.assertTrue(all(path==[42,3,'done'] for path in paths))
        semaphore=model.Semaphore(1);self.assertTrue(semaphore.acquire());self.assertEqual(semaphore.release(),1)

if __name__=='__main__':unittest.main()
