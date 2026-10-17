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
        expected.update({name:'' for name in model._NAMED_CHANNEL_PROGRAMS})
        expected.update({'1132_task_monitor':'','1133_linked_scope_cancel':'','1157_select_after_scope_cancel':'scope-cancel-select-after ok\n','1164_channel_recv_raw_match_nullable_payload':'ok\n','1172_module_readonly_capture':'module!\n'})
        traces=model.observations()
        self.assertEqual(set(expected),set(traces));self.assertEqual(sum(map(len,traces.values())),166)
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
        entries={x['id']:x for x in model.expected_rows()};named=[row for row in model.named_expected_rows() if row['id'] not in {model._STACK_NAMED_ID,model._PLAIN_SCOPE_NAMED_ID}];self.assertEqual(len(named),13)
        for row in named:
            name=row['id'].removeprefix('complete_task_named_')
            self.assertEqual(entries['complete_task_regression_'+name]['expected']['stdout_utf8'],'')
            self.assertFalse(entries['complete_task_regression_'+name]['separate_named_body_obligation']['executed_by_run'])
            self.assertEqual(row['expected']['required_test_names'],model.named_functions(name))
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
        for named,rows in [(False,model.expected_rows()),(True,[row for row in model.named_expected_rows() if row['id'] not in {model._STACK_NAMED_ID,model._PLAIN_SCOPE_NAMED_ID}])]:
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
        polled=traces['1138_get_result'];self.assertTrue(polled['body_may_have_started_before_poll'])
        self.assertEqual(polled['blocked_completed_receives'],1);self.assertEqual(polled['cancelled_completed_actions'],0)
        self.assertTrue(polled['pending_poll_itself_evaluates_no_completion_action'])
        self.assertIn('1109_await_any',model._NAMES)
        for name in ('1116_task_status','task_result_failed_error_value','suspended_frame_release'):self.assertNotIn(name,model._NAMES)

    def test_full_refill_graph_counts_match_independent_bounded_walk_formula(self):
        from math import comb
        proof=model.buffered_select_refill_proof(50,10)
        choose=lambda k:comb(100,k) if 0<=k<=100 else 0
        paths=sum(choose(50+12*j)-choose(51+12*j) for j in range(-10,11))
        self.assertEqual(proof['valid_operation_interleavings'],paths)
        self.assertEqual(paths,883645330487391898920795212)
        self.assertEqual((proof['received_count'],proof['received_sum'],proof['reachable_states']),(50,1225,506))
        self.assertEqual(model.buffered_select_refill_proof(3,1)['valid_operation_interleavings'],1)
        self.assertEqual(model.buffered_select_refill_proof(3,3)['valid_operation_interleavings'],5)
        self.assertFalse(proof['real_select_refill_wakeup_fairness_and_resources_qualified'])

    def test_dynamic_capacity_uses_both_complete_original_functions(self):
        facts=model.channel_named_requirements()['1154_dynamic_channel_capacity']
        self.assertEqual(len(facts['assertion_results']),14);self.assertTrue(all(facts['assertion_results']))
        first,second=facts['semantic_trace']['functions']
        self.assertEqual([first['capacity_from_original_variable'],second['capacity_from_original_variable']],[3,2])
        self.assertEqual(first['send_values'],first['receive_values']);self.assertEqual(first['receive_values'],[1,2,3])
        self.assertEqual(second['receive_values'],[4,5]);self.assertTrue(first['closed'] and second['closed'])
        self.assertEqual(model.named_functions('1154_dynamic_channel_capacity'),['test_dynamic_channel_capacity','test_bare_dynamic_channel_capacity'])

    def test_close_select_covers_both_registration_orders_without_assuming_a_park(self):
        checks,trace=model.close_select_assertions();self.assertEqual(checks,[True,True,True])
        self.assertEqual(trace['registration_orders_checked'],2);self.assertTrue(all(all(values) for values in trace['all_order_assertions']))
        self.assertEqual(trace['original_yield_count'],16);self.assertEqual(trace['original_done_timeout_ms'],2000)
        self.assertEqual(trace['channel_capacities'],[0,0,1]);self.assertEqual(trace['ready_rendezvous_completed_sends'],1)
        self.assertTrue(trace['sixteen_yields_do_not_prove_a_physical_park'])
        self.assertFalse(trace['real_close_wake_latency_and_registration_qualified'])

    def test_multiple_waiters_reobserve_one_value_and_zero_capacity_sends_finish_in_order(self):
        for value,aggregate,want in ((7,False,[314,'done']),(9,True,[318,'done'])):
            trace=model.multiple_waiter_trace(value,aggregate)
            self.assertEqual(trace['outputs'],want);self.assertEqual(trace['completion_value_evaluations'],1)
            self.assertEqual(trace['all_waiter_completion_orders'],2);self.assertEqual(trace['original_delayed_sleep_ms'],80)
            self.assertTrue(trace['does_not_prove_both_waiters_parked_before_task_completed'])
        checks,trace=model.send_twice_assertions();self.assertEqual(checks,[True,True,True])
        self.assertEqual(trace['capacity'],0);self.assertEqual(trace['receive_values'],[21,34])
        self.assertEqual(trace['completed_rendezvous_sends'],2);self.assertEqual(trace['return_value'],55)
        self.assertFalse(trace['opcode_register_integrity_and_runtime_lifetime_qualified'])

    def test_any_success_named_body_retains_failure_observation_without_payload_projection(self):
        body=model.channel_named_requirements()['1137_await_any_success']
        self.assertEqual(body['stdout_utf8'],'test1: single success\n  result: 42\ntest2: pre-completed tasks\n  result: 42\ntest3: skip failure\n  result: 42\ndone\n')
        self.assertEqual(body['assertion_results'],[True])
        self.assertTrue(body['semantic_trace']['business_failure_payload_not_projected_or_retyped'])
        self.assertTrue(body['semantic_trace']['public_TaskResult_failed_payload_shape_not_qualified'])

    def test_every_new_named_body_keeps_assert_count_and_separate_script_identity(self):
        import re
        facts=model.channel_named_requirements();entries={row['original_source']:row for row in model.expected_rows()}
        named={row['original_source']:row for row in model.named_expected_rows()}
        self.assertEqual(len(facts),7);self.assertEqual(sum(len(model.named_functions(name)) for name in facts),8)
        self.assertEqual(sum(len(body['assertion_results']) for body in facts.values()),23)
        self.assertEqual(sum(len(body.get('stdout_utf8','').splitlines()) for body in facts.values()),11)
        for name,path in model._NAMED_CHANNEL_PROGRAMS.items():
            text=(model.ROOT/path).read_text(encoding='utf-8');code=re.sub(r'//[^\n]*','',text)
            self.assertEqual(len(re.findall(r'\bassert(?:Equal)?\s*\(',code)),len(facts[name]['assertion_results']))
            for function in model.named_functions(name):self.assertIsNone(re.search(r'(?m)^'+function+r'\s*\(',code))
            self.assertEqual(entries[path]['expected']['stdout_utf8'],'');self.assertFalse(entries[path]['separate_named_body_obligation']['executed_by_run'])
            self.assertEqual(named[path]['expected']['required_test_names'],model.named_functions(name))
            self.assertIn('required_semantic_trace',named[path])


    def test_monitor_matches_only_selected_branches_and_sticky_completed_value(self):
        facts=model.task_monitor_requirements();self.assertEqual(facts['assertion_results'],[True]*5)
        trace=facts['semantic_trace'];self.assertEqual(trace['selected_match_branches'],['Success','Failed','Success'])
        self.assertEqual(trace['completed_task_terminal_flags'],['Success','Failed','Success'])
        self.assertEqual(trace['completed_success_values'],[42,42]);self.assertEqual(trace['completed_action_evaluations'],3)
        self.assertTrue(trace['already_completed_poll_evaluates_no_action']);self.assertEqual(trace['unselected_original_assert_false_branches'],3)
        self.assertTrue(trace['business_failure_payload_not_projected_or_retyped']);self.assertTrue(trace['public_TaskResult_failed_payload_shape_not_qualified'])

    def test_linked_scope_catch_is_invariant_for_every_slow_loop_prefix(self):
        facts=model.linked_scope_prefix_proof();self.assertEqual(facts['assertion_results'],[True]*3)
        trace=facts['semantic_trace'];cases=trace['slow_sibling_prefix_and_completion_cases']
        self.assertEqual([row['increments'] for row in cases[:-1]],list(range(101)))
        self.assertTrue(all(row['terminal']=='Cancelled' and row['failure_caught'] for row in cases[:-1]))
        self.assertEqual(cases[-1],{'increments':100,'terminal':'Success','failure_caught':True})
        self.assertEqual(trace['completed_success_values'],[[42,42],[42,42]])
        for count in (0,1,7):
            smaller=model.linked_scope_prefix_proof(count)['semantic_trace']['slow_sibling_prefix_and_completion_cases']
            self.assertEqual(len(smaller),count+2);self.assertEqual(smaller[-1]['increments'],count)
        for invalid in (-1,True):
            with self.assertRaises(ValueError):model.linked_scope_prefix_proof(invalid)
        self.assertFalse(trace['real_cancellation_checkpoints_scheduling_and_cleanup_qualified'])

    def test_cancel_select_retains_all_twenty_one_scopes_and_original_large_timeout(self):
        facts=model.linked_cancel_select_trace();self.assertEqual(facts['assertion_results'],[True,True])
        trace=facts['semantic_trace'];self.assertEqual(trace['original_select_timeout_ms'],1000000)
        self.assertEqual(trace['original_caught_loop_rounds'],20);self.assertEqual(len(trace['rounds']),21)
        self.assertEqual(trace['independent_registration_order_combinations'],2097152)
        for index,item in enumerate(trace['rounds']):
            self.assertEqual(item['original_round'],index);self.assertEqual([path['select_registered_before_failure'] for path in item['registration_orders']],[False,True])
            for path in item['registration_orders']:
                self.assertEqual((path['sibling_terminal'],path['scope_children_remaining'],path['active_wait_registrations']),('Cancelled',0,0))
                self.assertTrue(path['stale_deadline_has_no_second_completion'])
        self.assertFalse(trace['real_cancel_wakeup_registration_retraction_lifetime_and_resources_qualified'])
        for invalid in (-1,True):
            with self.assertRaises(ValueError):model.linked_cancel_select_trace(invalid)

    def test_nullable_channel_value_is_distinct_from_empty_closed_and_zero(self):
        facts=model.nullable_receive_trace();self.assertEqual(facts['assertion_results'],[True]*4)
        trace=facts['semantic_trace'];self.assertEqual(trace['receive_statuses'],['Value','Value','Closed'])
        self.assertEqual(trace['original_receive_payloads'],[0,None]);self.assertTrue(trace['null_payload_is_ready'])
        self.assertTrue(trace['selected_null_is_payload_not_empty_or_closed']);self.assertTrue(trace['all_original_channels_closed'])
        data=model.Channel(1);self.assertEqual(data.try_recv(),('Empty',None));data.send(None)
        self.assertEqual(data.try_recv(),('Value',None));data.close();self.assertEqual(data.try_recv(),('Closed',None))
        self.assertFalse(trace['real_raw_recv_lowering_and_runtime_payload_tags_qualified'])

    def test_module_readonly_import_companion_and_fallback_are_preserved(self):
        row=next(row for row in model.expected_rows() if row['source'].endswith('/1172_module_readonly_capture.xr'))
        self.assertEqual(row['expected']['stdout_utf8'],'module!\n');self.assertEqual(len(row['required_companions']),1)
        companion=row['required_companions'][0];self.assertEqual(companion['path'],model.BASE+'modules/module_readonly_capture.xr')
        self.assertEqual(companion['lf_sha256'],model._INPUTS[companion['path']])
        trace=row['required_semantic_trace'];self.assertFalse(trace['fallback_taken']);self.assertEqual(trace['completed_child_value'],'module!')
        self.assertFalse(trace['real_module_publication_ownership_and_task_transport_qualified'])
        with mock.patch.object(model,'_COMPANIONS',{}),self.assertRaises(ValueError):model.expected_rows()

    def test_new_complete_sources_preserve_branch_assertions_root_calls_and_named_routes(self):
        import re
        named=model.cancel_named_requirements();roots=model.cancel_root_traces();self.assertEqual(sum(len(row['assertion_results']) for row in named.values()),8)
        self.assertEqual(sum(len(row['assertion_results']) for row in roots.values()),6)
        counts={'1132_task_monitor':8,'1133_linked_scope_cancel':3,'1157_select_after_scope_cancel':2,'1164_channel_recv_raw_match_nullable_payload':4,'1172_module_readonly_capture':0}
        for name,path in model._CANCEL_PROGRAMS.items():
            source=(model.ROOT/path).read_text(encoding='utf-8');code=re.sub(r'//[^\n]*','',source)
            self.assertEqual(len(re.findall(r'\bassert(?:Equal)?\s*\(',code)),counts[name])
            if name in named:
                for function in model.named_functions(name):self.assertIsNone(re.search(r'(?m)^'+function+r'\s*\(',code))
            elif name in roots:
                self.assertTrue(any(row['source']==path and row['observation_count']==1 for row in model.expected_rows()))
        self.assertEqual(model._CANCEL_NAMED,{'1132_task_monitor':'test_task_monitor','1133_linked_scope_cancel':'test_linked_scope_cancel'})


    def test_complete_stack_worker_chain_keeps_all_source_local_values(self):
        for seed in (0,1,31,100,200):
            trace=model.stack_worker(seed)
            self.assertEqual(trace['worker_locals'],list(range(seed,seed+10)))
            self.assertEqual(trace['middle_locals'],list(range(seed+9,seed+25)))
            self.assertEqual(trace['wide_locals'],list(range(seed+24,seed+54)))
            self.assertEqual(trace['result'],56*seed+1464)
        self.assertEqual([model.stack_worker(n)['result'] for n in (0,100,200)],[1464,7064,12664])
        for value in (-1,201,True):
            with self.assertRaises(ValueError):model.stack_worker(value)

    def test_complete_three_stack_bodies_preserve_32reuse_andall36assertions(self):
        trace=model.stack_call_trace();values=trace['per_test_values']
        self.assertEqual(values[model._STACK_TESTS[0]],[1464,7064,12664,5600,5600])
        self.assertEqual(values[model._STACK_TESTS[1]],[1464])
        self.assertEqual(values[model._STACK_TESTS[2]],[1464+56*i for i in range(32)])
        self.assertEqual(values[model._STACK_TESTS[2]][-1],3200)
        self.assertEqual(sum(map(len,values.values())),38);self.assertEqual(trace['assertion_results'],[True]*36)
        self.assertEqual((trace['source_worker_calls'],trace['source_go_calls'],trace['source_await_calls']),(36,35,35))
        self.assertEqual((trace['source_middle_calls'],trace['source_wide_calls'],trace['original_reuse_iterations']),(36,36,32))
        self.assertEqual(trace['source_local_declarations'],{'worker':10,'middle':16,'wide':30})
        self.assertFalse(trace['actual_frame_layout_growth_pool_reuse_FI_resources_lifetime_safety_providers_or_mixed_qualified'])

    def test_wide_corruption_passes_old_assertions_but_fails_exact_stack_observations(self):
        wrong=model.stack_call_trace(lambda seed:sum(seed+i for i in range(29)))
        self.assertEqual(wrong['assertion_results'],[True]*36)
        self.assertEqual(wrong['per_test_values'][model._STACK_TESTS[0]],[1411,6911,12411,5500,5500])
        self.assertNotEqual(wrong['per_test_values'],model.stack_call_trace()['per_test_values'])
        for order in __import__('itertools').permutations((0,100,200)):
            completed={seed:model.stack_worker(seed)['result'] for seed in order}
            self.assertEqual([completed[seed] for seed in (0,100,200)],[1464,7064,12664])

    def test_stack_original_helper_and_observed_bodies_are_exactly_pinned(self):
        saved=Path.read_bytes
        for name in model._STACK_INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'changed' if path==model.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.validate_stack_inputs()
        with mock.patch.object(model,'_STACK_INPUTS',{}),self.assertRaises(ValueError):model.validate_stack_inputs()
        with mock.patch.object(model,'_STACK_TRANSFORMS',[]),self.assertRaises(ValueError):model.validate_stack_inputs()
        original=(model.ROOT/model._STACK_SOURCE).read_text(encoding='utf-8');observed=(model.ROOT/model._STACK_FIXTURE).read_text(encoding='utf-8')
        self.assertEqual(original.count('@test'),3);self.assertEqual(original.count('print('),0)
        self.assertEqual(observed.count('print('),7);self.assertIn('while (i < 32)',observed)
        self.assertEqual((model.ROOT/model._STACK_HELPER).read_bytes(),(model.ROOT/model._STACK_FIXTURE_HELPER).read_bytes())

    def test_stack_standalone_andall_named_observations_have_separate_product_routes(self):
        cases={row['id']:row for row in json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_bytes())['cases']}
        entry=model.stack_entry_expected_row();named=model.stack_named_expected_row();run=cases[entry['id']];test=cases[named['id']]
        self.assertEqual((run['source'],run['entry'],run['source_context']),(model._STACK_SOURCE,'run','original_absolute'))
        self.assertEqual(run['stdout_utf8'],'');self.assertFalse(entry['separate_named_body_obligation']['executed_by_run'])
        self.assertEqual((test['source'],test['entry']),(model._STACK_FIXTURE,'test'))
        self.assertEqual(named['expected']['required_test_names'],list(model._STACK_TESTS))
        self.assertEqual(test['stdout_utf8'],named['expected']['stdout_utf8']);self.assertEqual(len(test['stdout_utf8'].splitlines()),38)
        self.assertEqual([x['expected'] for x in test['independent_observations']['values']],[v for values in model.stack_call_trace()['per_test_values'].values() for v in values])
        self.assertEqual(test['companions'],[{'path':model._STACK_FIXTURE_HELPER,'lf_sha256':model._STACK_INPUTS[model._STACK_FIXTURE_HELPER]}])
        self.assertEqual(run['companions'],[{'path':model._STACK_HELPER,'lf_sha256':model._STACK_INPUTS[model._STACK_HELPER]}])


    def test_plain_scope_all_six_report_orders_preserve_failure_and_success_siblings(self):
        for order in __import__('itertools').permutations((1,10,100)):
            trace=model.plain_scope_child_error_trace(order)
            self.assertEqual(trace['values'],[111,True,5,True])
            self.assertEqual(trace['semantic_postconditions'],[True]*4)
            self.assertTrue(trace['root_can_terminate'])
            self.assertEqual([x['value'] for x in trace['plain_scope_reports']],list(order))
            self.assertEqual([x['terminal'] for x in trace['plain_scope_reports']].count('Failed'),1)
        trace=model.plain_scope_child_error_trace()
        self.assertEqual((trace['source_go_calls'],trace['source_awaitResult_calls'],trace['source_plain_scopes']),(5,2,3))
        self.assertEqual((trace['original_receive_count'],trace['original_close_calls']),(4,2))
        self.assertEqual(trace['original_send_values'],[1,10,100,5])
        self.assertEqual(trace['original_Channel_capacities'],[3,1])
        self.assertEqual(trace['original_slow_sleep_ms'],1000)
        self.assertEqual((trace['original_static_assertions'],trace['original_positive_assertion_executions'],trace['original_failure_assertion_branches_avoided']),(4,2,2))

    def test_linked_failure_policy_cannot_replace_the_complete_plain_scope(self):
        wrong=[model.plain_scope_child_error_trace(order,cancel_siblings=True)
               for order in __import__('itertools').permutations((1,10,100))]
        self.assertEqual(sum(not trace['root_can_terminate'] for trace in wrong),4)
        self.assertEqual(sum(trace['root_can_terminate'] for trace in wrong),2)
        for trace in wrong:
            if not trace['root_can_terminate']:
                self.assertEqual(trace['values'],[])
                self.assertFalse(trace['semantic_postconditions'][0])
        escaped=model.plain_scope_child_error_trace(failure_escapes_scope=True)
        self.assertFalse(escaped['root_can_terminate'])
        self.assertEqual(escaped['values'],[])

    def test_plain_scope_task_result_tags_and_parent_completion_are_not_interchangeable(self):
        failed=model.plain_scope_child_error_trace(failed_tag='Success')
        self.assertEqual(failed['values'],[111,False,5,True])
        cancelled=model.plain_scope_child_error_trace(cancel_tag='Success')
        self.assertEqual(cancelled['values'],[111,True,5,False])
        for where in ('not_started','sleeping'):
            trace=model.plain_scope_child_error_trace(cancel_position=where)
            self.assertEqual(trace['values'],[111,True,5,True])
            self.assertFalse(trace['TaskResult_Failed_payload_layout_or_Error_PanicInfo_conversion_qualified'])
            self.assertFalse(trace['real_scheduling_cancel_checkpoints_cleanup_FI_resource_lifetime_safety_provider_mixed_qualified'])
        with self.assertRaises(ValueError):model.plain_scope_child_error_trace((1,10))
        with self.assertRaises(ValueError):model.plain_scope_child_error_trace(cancel_position='completed')

    def test_plain_scope_every_original_and_observation_authority_is_pinned(self):
        saved=Path.read_bytes
        for name in model._PLAIN_SCOPE_INPUTS:
            def changed(path):return saved(path)+b'changed' if path==model.ROOT/name else saved(path)
            with self.subTest(path=name),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):
                model.validate_plain_scope_inputs()
        with mock.patch.object(model,'_PLAIN_SCOPE_INPUTS',{}),self.assertRaises(ValueError):
            model.validate_plain_scope_inputs()
        with mock.patch.object(model,'_PLAIN_SCOPE_TRANSFORMS',[]),self.assertRaises(ValueError):
            model.validate_plain_scope_inputs()

    def test_plain_scope_observer_keeps_all_calls_and_does_not_drain_a_channel_twice(self):
        import re
        original=(model.ROOT/model._PLAIN_SCOPE_SOURCE).read_text(encoding='utf-8')
        observed=(model.ROOT/model._PLAIN_SCOPE_FIXTURE).read_text(encoding='utf-8')
        self.assertEqual(original.count('print('),0)
        self.assertEqual(observed.count('print('),4)
        self.assertEqual(re.findall(r'^fn\s+(\w+)',original,re.M),re.findall(r'^fn\s+(\w+)',observed,re.M))
        for text in (original,observed):
            self.assertEqual(len(re.findall(r'\bassert(?:Equal)?\(',text)),4)
            self.assertEqual(text.count('recv_sum(ch, 3)'),1)
            self.assertEqual(text.count('recv_sum(ch, 1)'),1)
            self.assertIn('time.sleep(1000)',text)
            self.assertIn('for (var i = 0; i < count; i++)',text)
            self.assertEqual(text.count('@test'),1)

    def test_plain_scope_empty_entry_and_complete_named_body_have_separate_routes(self):
        cases={row['id']:row for row in json.loads((model.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_bytes())['cases']}
        entry=model.plain_scope_entry_expected_row()
        named=model.plain_scope_named_expected_row()
        self.assertEqual(cases[entry['id']]['source'],model._PLAIN_SCOPE_SOURCE)
        self.assertEqual((cases[entry['id']]['entry'],cases[entry['id']]['source_context']),('run','original_absolute'))
        self.assertEqual(entry['expected']['stdout_utf8'],'')
        self.assertFalse(entry['separate_named_body_obligation']['executed_by_run'])
        self.assertEqual((cases[named['id']]['source'],cases[named['id']]['entry']),(model._PLAIN_SCOPE_FIXTURE,'test'))
        self.assertEqual(named['expected']['stdout_utf8'],'111\ntrue\n5\ntrue\n')
        self.assertEqual(named['expected']['required_test_names'],[model._PLAIN_SCOPE_TEST])
        self.assertEqual([x['expected'] for x in cases[named['id']]['independent_observations']['values']],[111,True,5,True])

if __name__=='__main__':unittest.main()
