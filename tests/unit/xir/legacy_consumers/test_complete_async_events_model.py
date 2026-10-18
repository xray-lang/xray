from provenance_test_support import provenance_input_path
import unittest
import complete_async_events_model as model
import complete_channel_model as sleep_model

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


    def test_complete_sleep_channel_preserves_all_four_observations_and_operations(self):
        trace=sleep_model.sleep_channel_trace()
        self.assertEqual(trace['observations'],['Recv.Value(11)','sent','recv-value','empty-timeout'])
        self.assertEqual([row for row in trace['events'] if row[0]=='sleep_complete'],[('sleep_complete',5,x) for x in range(1,5)])
        self.assertEqual([row for row in trace['events'] if row[0]=='discard_sleep_status'],[('discard_sleep_status',x) for x in range(1,5)])
        self.assertEqual([row for row in trace['events'] if row[0]=='channel_enter'],[('channel_enter',x) for x in ('send','recv','sendTimeout','recvTimeout','recvTimeout')])
        self.assertIn(('channel_wait_timeout',10,5),trace['events']);self.assertEqual(trace['events'][-1],('close',))
        self.assertEqual(trace['final_queue'],[]);self.assertTrue(trace['closed'])
        self.assertEqual((trace['original_prints'],trace['original_match_expressions']),(4,3))
        self.assertFalse(trace['actual_scheduler_timeout_latency_cleanup_FI_safety_providers_or_mixed_qualified'])

    def test_stale_sleep_status_and_dropped_real_timeout_are_distinct_negative_controls(self):
        class RetainSleep(sleep_model.SleepChannelBoundary):
            def enter(self,operation):self.events.append(('channel_enter',operation))
        stale=RetainSleep();stale.sleep(5)
        self.assertEqual(stale.send_timeout(22,50),'Timeout');self.assertEqual(stale.channel.queue,[])
        stale=RetainSleep();stale.channel.send(22);stale.sleep(5)
        self.assertEqual(stale.recv_timeout(50),('Timeout',None));self.assertEqual(stale.channel.queue,[22])
        with self.assertRaises(sleep_model.WouldBlock):sleep_model.sleep_channel_trace(RetainSleep)
        class DropEveryTimeout(sleep_model.SleepChannelBoundary):
            def channel_wait_result(self):return 'Empty',None
        wrong=sleep_model.sleep_channel_trace(DropEveryTimeout)['observations']
        self.assertEqual(wrong[:3],sleep_model.sleep_channel_trace()['observations'][:3])
        self.assertEqual(wrong[3],'empty-value');self.assertNotEqual(wrong,sleep_model.sleep_channel_trace()['observations'])

    def test_sleep_channel_original_andevery_authority_is_additively_pinned(self):
        from pathlib import Path
        from unittest import mock
        saved=Path.read_bytes
        for name in sleep_model._SLEEP_CHANNEL_INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'changed' if path==provenance_input_path(sleep_model.ROOT,name) else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):sleep_model.sleep_channel_expected_row()
        with mock.patch.object(sleep_model,'_SLEEP_CHANNEL_INPUTS',{}),self.assertRaises(ValueError):sleep_model.sleep_channel_expected_row()

    def test_sleep_channel_product_keeps_the_direct_enum_print_andwhole_original(self):
        import json
        row=sleep_model.sleep_channel_expected_row()
        case=next(x for x in json.loads((sleep_model.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_bytes())['cases'] if x['id']==row['id'])
        self.assertEqual((case['source'],case['entry'],case['source_context']),(sleep_model._SLEEP_CHANNEL_SOURCE,'run','original_absolute'))
        self.assertEqual((case['arguments'],case['stdin_hex'],case['companions']),([],'',[]))
        self.assertEqual((case['expected_exit_code'],case['stderr_utf8'],case['stdout_utf8']),(0,'','Recv.Value(11)\nsent\nrecv-value\nempty-timeout\n'))
        self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8']);self.assertIsNone(row['required_target'])
        self.assertEqual(sleep_model.channel_recv_text(('Value',11)),'Recv.Value(11)')
        self.assertEqual(sleep_model.channel_recv_text(('Timeout',None)),'Recv.Timeout')

if __name__=='__main__':unittest.main()
