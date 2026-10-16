"""Independent complete task entry traces, named bodies and state distinctions."""
from pathlib import Path
import json
import unittest
from unittest import mock
import complete_task_regression_model as model


class TaskRegressionTraces(unittest.TestCase):
    def test_every_complete_entry_trace(self):
        expected={
          '1100_cancelled':'false\n',
          '1101_channel_basic':'true\ntrue\ntrue\ntrue\nfalse\n10\n20\n30\ntrue\nfalse\ndone\n',
          '1102_go_await':'30\n30\n60\ndone\n',
          '1103_defer':'start\nwork done\ncleanup 3\ncleanup 2\ncleanup 1\nend\n',
          '1104_coroutine_combined':'=== 协程综合测试 ===\nproducer done\n6\nfalse\n=== 测试完成 ===\ncleanup done\n',
          '1105_defer_args':'start\nwork done\nresult\n42\ncleanup 2\ncleanup 1\nend\n',
          '1106_select':'100\n200\nno data\ndone\n',
          '1107_go_deferred':'start\n1\n2\n3\n1\nend\n',
          '1108_await_all':'4\n9\n16\n29\ndone\n',
          '1110_scope':'10\n20\n1\ndone\n',
          '1111_comprehensive':'1\n2\n6\nworking\nresource2\nresource1\n100\n',
          '1112_go_lambda_call':'100\n200\n300\ndone\n',
          '1113_select_send':'sent 100\nsent 200\nbuffer full\n100\n200\ntrue\ndone\n',
          '1114_select_after':'timeout\n42\nimmediate timeout\ndone\n',
          '1115_cancel':'t1 cancelled: true\nt1 status: TaskStatus.Cancelled\nt2 success: true\nt2 result: 42\ndone\n',
          '1118_comprehensive':'=== go/await ===\ntask1 result: 42\n=== status ===\ndone: true\nstatus: TaskStatus.Success\nresult: 100\n=== cancel ===\ncancelled: true\ncancelled status: TaskStatus.Cancelled\n=== channel ===\nrecv: 1 2 3\n=== defer ===\nworking...\ncleanup!\n=== await all ===\nall results: [1, 2, 3]\n=== await any ===\nfirst result: winner\n=== done ===\n',
          '1119_await_all_syntax':'test1: await all basic\n  r0: 10\n  r1: 20\n  r2: 30\n  sum: 60\ntest2: await all two tasks\n  r0: 10\n  r1: 20\n',
          '1120_scope_basic':'=== scope 基础测试 ===\nscope 结束\n任务数量: 6\n所有任务完成\n=== 嵌套 scope 测试 ===\n嵌套 scope 结果数量: 5\n',
          '1121_defer_block':'=== defer 块语法测试 ===\n开始\n结束\ndefer 块\n=== 多个 defer 块 ===\n执行中\n结束\ndefer 2\ndefer 1\n=== defer 混合测试 ===\n执行\ndefer 块 A\ncleanup 函数\n',
          '1122_coro_naming':'','1125_coro_local':'',
          '1128_yield':'test1: main yield\ntest1: after yield\ntest2: result = yielded\ntest3: 10 20 30\ntest4: sum = 10\ntest5: sums = 45 45 45\n',
          '1130_linked_scope':'','1131_linked_go_expr':''}
        expected.update({'await_array':'[100, 200, 300]\n[400, 500]\n600\n',
          'channel_send_timeout_partner_arrives':'sent\nsecond-timeout\n','deterministic_timer_schedule_order':'2\n3\n1\n',
          'poll_try_recv_safepoint':'42\n','poll_try_send_safepoint':'1\n42\n',
          '1109_await_any':'100\n200\n200\n999\ndone\n',
          '1138_get_result':'before pending: true\nafter: 21\nagain: 21\ncancelled: true\ncompleted: 42\n',
          '1139_error_isolation':'test1: error isolation\n  done: true\n  failed: true\ntest2: error does not affect other coroutines\n  result: 42\n',
          '1140_task_error':'t1.done: true\nt1.failed: true\nt2.done: true\nt2.result: 42\n'})
        traces=model.observations()
        self.assertEqual(set(expected),set(traces));self.assertEqual(sum(map(len,traces.values())),163)
        for name,text in expected.items():
            with self.subTest(program=name):self.assertEqual(''.join(model.render(v)+'\n' for v in traces[name]),text)

    def test_completion_order_is_not_assumed_to_be_lazy_or_fifo(self):
        self.assertEqual(model.ordered_values([30,20,10]),[30,20,10])
        self.assertEqual(model.ordered_values([1,1,2]),[1,1,2])
        self.assertEqual(set(model.joined_counter_paths()),{6})
        self.assertEqual(len(model.joined_counter_paths()),6)

    def test_repeat_await_and_completed_cancel_preserve_one_result(self):
        calls=[]
        task=model.Task(lambda:calls.append('called') or [1,2])
        first=task.await_value();first.append(9);task.cancel()
        self.assertEqual(task.await_value(),[1,2]);self.assertEqual(calls,['called'])
        self.assertEqual(task.executions,1);self.assertEqual(task.poll()[0],'Success')

    def test_blocked_task_cancellation_does_not_fabricate_a_value(self):
        calls=[];ready=[];task=model.Task(lambda:calls.append('called') or 42,lambda:bool(ready))
        task.finish();self.assertEqual(task.state,'Running');self.assertEqual(task.poll(),('Pending',None))
        task.cancel();self.assertEqual(task.await_result(),('Cancelled',None));self.assertEqual(calls,[])
        with self.assertRaises(ValueError):task.await_value()

    def test_select_model_refuses_to_invent_a_competing_ready_priority(self):
        first,second=model.Channel(1),model.Channel(1);first.try_send(1);second.try_send(2)
        with self.assertRaises(ValueError):model.receive_only_ready([first,second],'default')
        self.assertEqual(first.queue,[1]);self.assertEqual(second.queue,[2])
        for name in ['1116_task_status','1117_await_timeout']:
            self.assertNotIn(name,model._NAMES)

    def test_complete_cleanup_order_and_typed_status_display(self):
        self.assertEqual(model.close_with_defer(['body'],[['first'],['second',42]]),['body','second',42,'first'])
        self.assertEqual(model.line('status:',model.Enum('TaskStatus','Success')),'status: TaskStatus.Success')

    def test_timing_and_yield_requirements_are_retained_without_fake_measurements(self):
        rows={x['id']:x for x in model.expected_rows()}
        self.assertEqual(rows['complete_task_regression_1114_select_after']['required_runtime_timing_predicates'],
            [{'expression':'elapsed >= 20','required':True},{'expression':'elapsed < 500','required':True}])
        self.assertEqual(rows['complete_task_regression_1128_yield']['required_original_yield_checkpoints'],39)

    def test_declaration_only_modules_have_separate_full_named_body_requirements(self):
        entries={x['id']:x for x in model.expected_rows()};named=model.named_expected_rows();self.assertEqual(len(named),4)
        for row in named:
            name=row['id'].removeprefix('complete_task_named_')
            self.assertEqual(entries['complete_task_regression_'+name]['expected']['stdout_utf8'],'')
            self.assertFalse(entries['complete_task_regression_'+name]['separate_named_body_obligation']['executed_by_run'])
            self.assertEqual(row['expected']['required_test_names'],[model._DECLARATION_ONLY[name]])
            self.assertTrue(all(row['required_original_assertion_results']))
        facts=model.named_requirements()
        self.assertEqual(facts['1122_coro_naming']['stdout_utf8'],'10\n10\n20\n30\n40\n50\ndone\n')
        self.assertEqual([len(facts[name]['assertion_results']) for name in ['1125_coro_local','1130_linked_scope','1131_linked_go_expr']],[9,4,4])

    def test_every_complete_source_and_authority_is_pinned(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==model.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()

    def test_exact_complete_program_set_cannot_be_swapped_or_dropped(self):
        swapped=dict(model._PROGRAMS);names=sorted(swapped);swapped[names[0]],swapped[names[1]]=swapped[names[1]],swapped[names[0]]
        for attribute,value in [('_PROGRAMS',swapped),('_PROGRAMS',{}),('_INPUTS',{})]:
            with self.subTest(attribute=attribute),mock.patch.object(model,attribute,value),self.assertRaises(ValueError):model.expected_rows()

    def test_complete_entries_and_named_bodies_use_their_own_real_routes(self):
        cases={row['id']:row for row in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']}
        for named,rows in [(False,model.expected_rows()),(True,model.named_expected_rows())]:
            for expected in rows:
                with self.subTest(case=expected['id']):
                    case=cases[expected['id']]
                    self.assertEqual(case['entry'],'test' if named else 'run')
                    self.assertEqual(case['source'],expected['original_source'])
                    self.assertEqual(case['stdout_utf8'],expected['expected']['stdout_utf8'])
                    self.assertEqual(case.get('source_context'),'original_absolute' if not named else None)


    def test_precompleted_any_is_limited_to_known_selection_paths(self):
        tasks=[model.Task(lambda n=n:n) for n in (200,300,400)]
        for task in tasks:task.finish()
        self.assertEqual(model.known_completed_any(tasks),200)
        self.assertEqual(model.known_completed_any(list(reversed(tasks))),400)
        pending=model.Task(lambda:999,lambda:False)
        self.assertEqual(model.known_completed_any([pending,tasks[1]]),300)
        with self.assertRaises(ValueError):model.known_completed_any([*tasks[:2],pending])
        with self.assertRaises(ValueError):model.known_completed_any([pending])
        self.assertEqual([task.executions for task in tasks],[1,1,1]);self.assertEqual(pending.executions,0)

    def test_cancelled_wait_generation_cannot_complete_a_later_operation(self):
        waits=model.WaitTokens();first=waits.begin(0,2000);self.assertEqual(waits.complete(first,'Sent'),'Sent')
        second=waits.begin(5,10);self.assertNotEqual(first,second)
        self.assertIsNone(waits.expire(second,14));self.assertIsNone(waits.expire(first,2000))
        self.assertEqual(waits.active,{second:15});self.assertEqual(waits.expire(second,15),'Timeout')
        self.assertIsNone(waits.expire(first,3000));self.assertIsNone(waits.complete(second,'Sent'))
        self.assertEqual(waits.completed,{first:'Sent',second:'Timeout'})

    def test_poll_output_is_invariant_over_arbitrary_finite_failure_prefixes(self):
        for failed in (0,1,2,7,31):
            with self.subTest(failed=failed):
                received=model.receive_poll_trace(failed);sent=model.send_poll_trace(failed)
                self.assertEqual(received['outputs'],[42]);self.assertEqual(sent['outputs'],[1,42])
                self.assertEqual(received['data_capacity'],0);self.assertEqual(received['zero_capacity_buffered_items'],0)
                self.assertEqual(received['completed_rendezvous_sends'],1);self.assertEqual(sent['drained_initial_value'],7)
                self.assertTrue(all(received['assertion_results']+sent['assertion_results']))
                self.assertTrue(received['source_poll_loop_has_no_new_iteration_cap']);self.assertTrue(sent['channels_closed'])
        for invalid in (-1,True):
            with self.assertRaises(ValueError):model.receive_poll_trace(invalid)
            with self.assertRaises(ValueError):model.send_poll_trace(invalid)

    def test_zero_capacity_pending_send_is_not_a_buffered_message(self):
        channel=model.Rendezvous();self.assertEqual(channel.try_recv(),('Empty',None));channel.offer(42)
        self.assertEqual(channel.completed_sends,0);self.assertEqual(channel.try_recv(),('Value',42))
        self.assertEqual(channel.completed_sends,1);self.assertEqual(channel.try_recv(),('Empty',None))
        channel.close();self.assertEqual(channel.try_recv(),('Closed',None))
        with self.assertRaises(ValueError):channel.offer(7)

    def test_failed_task_observation_is_sticky_and_does_not_cancel_sibling(self):
        faulty=model.EventTask(lambda:1//0);self.assertFalse(faulty.done);self.assertEqual(faulty.poll(),('Pending',None))
        state,payload=faulty.await_result();self.assertEqual(state,'Failed');self.assertTrue(faulty.done)
        payload['observed_runtime_fault_code']=0;faulty.cancel();self.assertEqual(faulty.poll(),('Failed',{'observed_runtime_fault_code':420}))
        self.assertEqual(faulty.await_result(),('Failed',{'observed_runtime_fault_code':420}));self.assertEqual(faulty.executions,1)
        sibling=model.EventTask(lambda:42);self.assertEqual(sibling.await_value(),42);self.assertTrue(sibling.done)
        cancelled=model.EventTask(lambda:99,lambda:False);cancelled.cancel();self.assertEqual(cancelled.await_result(),('Cancelled',None));self.assertEqual(cancelled.executions,0)

    def test_complete_event_rows_keep_original_deadlines_assertions_and_roots(self):
        rows=[row for row in model.expected_rows() if row['source'] in model._EVENT_PROGRAMS.values()]
        self.assertEqual(len(rows),9);self.assertEqual(sum(row['observation_count'] for row in rows),30)
        traces=model.event_boundary_traces();partner=traces['channel_send_timeout_partner_arrives']
        self.assertEqual(partner['capacity'],0);self.assertEqual(partner['original_receiver_sleep_ms'],5)
        self.assertEqual(partner['original_send_timeout_ms'],[2000,10]);self.assertEqual(partner['transferred_values'],[7])
        self.assertEqual(traces['poll_try_recv_safepoint']['original_receive_timeout_ms'],1000)
        self.assertEqual(traces['poll_try_send_safepoint']['original_receive_timeout_ms'],[1000,1000])
        self.assertEqual(traces['deterministic_timer_schedule_order']['original_sleep_delays_ms'],[50,10,30])
        self.assertTrue(all(traces['deterministic_timer_schedule_order']['original_assertion_results']))
        self.assertEqual(traces['await_array']['original_compute_calls'],7)
        for row in rows:
            self.assertEqual(row['source'],row['original_source']);self.assertIn('required_semantic_trace',row)
        self.assertIn('1109_await_any',model._NAMES)
        for name in ('1116_task_status','task_result_failed_error_value','suspended_frame_release'):self.assertNotIn(name,model._NAMES)

if __name__=='__main__':unittest.main()
