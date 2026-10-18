"""Whole parallel workload values, unchanged counts and runtime-proof limits."""
from provenance_test_support import provenance_input_path
from pathlib import Path
import itertools,json,unittest
from unittest import mock
import complete_parallel_workload_model as m


class CompleteParallelWorkloads(unittest.TestCase):
    def test_seven_complete_originals_and_all_thirteen_named_bodies(self):
        rows=[row for row in m.expected_rows() if row['source'] not in m._PLAN_PROGRAMS.values()];named=m.named_expected_rows();bodies=m.bodies()
        self.assertEqual((len(rows),len(named),len(bodies)),(7,5,13))
        self.assertEqual(sum(x['observation_count'] for x in rows),2)
        self.assertEqual(sum(len(x['required_original_assertions']) for x in named),219)
        self.assertTrue(all(x['passed'] for body in bodies.values() for x in body['assertions']))
        self.assertEqual([row['expected']['stdout_utf8'] for row in rows[-2:]],['405312\n','42\n'])

    def test_declaration_runs_do_not_execute_named_bodies(self):
        for row in m.expected_rows()[:5]:
            self.assertEqual(row['expected']['stdout_utf8'],'');self.assertEqual(row['observation_count'],0)
            self.assertFalse(row['separate_named_body_obligation']['executed_by_run'])
        self.assertEqual([len(x['expected']['required_test_names']) for x in m.named_expected_rows()],[1,7,1,3,1])

    def test_original_fifo_producer_consumer_keeps_all_six_values(self):
        trace=m.bodies()['test_main']
        self.assertEqual(trace['stdout_utf8'],'100\n101\n102\n200\n201\n202\ntrue\ndone\n')
        self.assertEqual(trace['received_values'],[100,101,102,200,201,202]);self.assertEqual(trace['channel_capacity'],10)
        self.assertEqual((trace['try_send_calls'],trace['try_recv_calls']),(6,6))
        self.assertEqual(trace['original_producer_loop_iterations'],[3,3]);self.assertEqual(trace['explicit_channel_close_calls'],0)

    def test_all330_concat_tasks_and_original_five_step_loops_remain(self):
        names=m._NAMED['1146_multicore_string_concat'];bodies=m.bodies();counts=[50,50,50,30,50,50,50]
        self.assertEqual([bodies[x]['spawned_tasks'] for x in names],counts);self.assertEqual(sum(counts),330)
        for name,count in zip(names,counts):
            trace=bodies[name];self.assertEqual(trace['await_all_inputs'],count);self.assertEqual(len(trace['full_task_results']),count)
            self.assertEqual(trace['original_per_index_predicates'],[True]*count)
            self.assertEqual(trace['assertions'][0]['actual'],count)
        self.assertEqual(bodies['test_concurrent_loop_concat']['original_inner_loop_iterations'],150)
        self.assertEqual(bodies['test_concurrent_threshold_boundary']['full_task_results'],list(range(60,80,2))*5)

    def test_full_concat_values_preserve_short_long_mixed_loop_and_nested_arrays(self):
        self.assertEqual(m.concat_value('short',17),'task_17_done')
        self.assertEqual(m.concat_value('long',17),'x'*50+'_id17_'+'y'*50)
        self.assertEqual(m.concat_value('mixed',2),'short_2');self.assertEqual(m.concat_value('mixed',3),'x'*50+'_long_3_'+'y'*50)
        self.assertEqual(m.concat_value('loop',2),'start_2_0_1_2_3_4_end')
        self.assertEqual(m.concat_value('stress',17),['s17e','x'*40+'_17_'+'y'*40,'17_a_b_c'])
        isolation=m.concat_value('isolation',49);self.assertEqual(isolation['result'],'X'*30+'_49_'+'X'*30)
        self.assertEqual(isolation['id'],49);self.assertTrue(isolation['prefix_ok'] and isolation['suffix_ok'] and isolation['contains_id'])

    def test_ordered_await_all_preserves_task_identity_for_every_small_order(self):
        values=['a','b','c','d']
        for order in itertools.permutations(range(4)):self.assertEqual(m.ordered_results(values,order),values)
        for order in [[0,1,2],[0,1,2,2],[0,1,2,4]]:
            with self.assertRaises(ValueError):m.ordered_results(values,order)

    def test_corrupted_concat_result_breaks_original_correct_count(self):
        saved=m.concat_value
        def corrupted(kind,index):return 'wrong' if kind=='short' and index==49 else saved(kind,index)
        with mock.patch.object(m,'concat_value',corrupted),self.assertRaisesRegex(ValueError,'Original workload assertion'):
            m.expected_rows()
        # Preserve the original weaker predicates; do not invent new checks.
        self.assertTrue(m.concat_valid('loop',2,'start_2_end'))
        self.assertTrue(m.concat_valid('isolation',2,{'prefix_ok':True,'suffix_ok':True,'contains_id':True}))

    def test_all24_worker_arrivals_sort_to_original_output_and_all12_sleeps_remain(self):
        trace=m.bodies()['test_preempt_test']
        self.assertEqual(trace['all24_completion_order_sorted_outputs'],[[0,1,2,3]]*24)
        self.assertEqual(trace['required_sleep_calls_ms'],[5]*12)
        self.assertEqual((trace['spawned_tasks'],trace['original_worker_iterations']),(4,3))
        self.assertEqual(trace['stdout_utf8'],'启动 4 个协程...\n协程 0 完成\n协程 1 完成\n协程 2 完成\n协程 3 完成\n调度测试通过：4/4 协程完成\n')
        self.assertIn('no wall-clock predicate invented',trace['actual_sleep_preemption_and_wakeup'])

    def test_plain_scopes_keep13500_tasks_reports_and675000_inner_iterations(self):
        bodies=m.bodies();failed=bodies['test_scope_loses_no_children'];success=bodies['test_scope_all_success']
        self.assertEqual((failed['original_outer_iterations'],failed['children_per_scope'],failed['channel_capacity']),(50,250,250))
        self.assertEqual((success['original_outer_iterations'],success['children_per_scope'],success['channel_capacity']),(5,200,200))
        self.assertEqual(failed['required_report_sends']+success['required_report_sends'],13500)
        self.assertEqual(failed['required_report_receives']+success['required_report_receives'],13500)
        self.assertEqual(failed['required_child_arithmetic_iterations']+success['required_child_arithmetic_iterations'],675000)
        self.assertEqual((len(failed['assertions']),len(success['assertions'])),(100,10))
        self.assertEqual(failed['required_channel_close_calls']+success['required_channel_close_calls'],55)

    def test_commutative_scope_counts_reject_lost_or_changed_children(self):
        values=[-1]*200+[1]*50
        for offset in [0,1,50,200,249]:self.assertEqual(m.report_counts(values[offset:]+values[:offset],250),(200,50))
        with self.assertRaises(ValueError):m.report_counts(values[:-1],250)
        with self.assertRaises(ValueError):m.report_counts([0]+values[1:],250)

    def test_linked_scope_preserves_bounds_without_predicting_started_children(self):
        trace=m.bodies()['test_linked_first_error_deterministic']
        self.assertEqual((trace['original_outer_iterations'],trace['declared_child_launch_bound_per_scope']),(50,200))
        self.assertEqual(trace['declared_arithmetic_loop_iterations_per_started_child'],50)
        self.assertEqual(trace['possible_original_error_payloads'],['err_'+str(i) for i in range(200)])
        self.assertEqual(len(trace['assertions']),100);self.assertEqual(trace['exact_first_child_identity'],'NOT_PREDICTED')
        self.assertIn('NOT_PREDICTED',trace['actual_started_and_completed_children'])

    def test_complete2048_spill_chain_is_not_replaced_by_a_shallow_sample(self):
        trace=m.bodies()['test_inject_queue_spill_chain']
        self.assertEqual(trace['original_depth'],2048);self.assertEqual(trace['recurrence_values'],list(range(2049)))
        self.assertEqual((trace['required_recursive_go_calls'],trace['required_root_go_calls'],trace['required_await_calls']),(2048,1,2049))
        self.assertIn('not measured overflow',trace['actual_queue_overflow_and_stack_safety'])

    def test_all256_buffer_tuple_transfers_preserve_sizes_checksum_and_views(self):
        trace=m.tuple_traces()['1174_channel_tuple_buffer_transfer'];frames=trace['required_tuple_frames']
        self.assertEqual(trace['checksum'],405312);self.assertEqual(trace['requested_size_multiplicities'],{'32':86,'256':85,'4096':85})
        self.assertEqual(len(frames),256);self.assertEqual([x['sequence'] for x in frames],list(range(256)))
        self.assertTrue(all(x['sequence']==x['received_sequence']==x['awaited_publisher_result'] and x['asBytes_length']==x['requested_zeroed_bytes'] for x in frames))
        for field in ['required_zeroed_allocations','required_send_calls','required_recv_calls','required_await_calls']:self.assertEqual(trace[field],256)
        self.assertEqual(trace['channel_capacity'],1);self.assertEqual(trace['required_channel_close_calls'],1)
        self.assertEqual(trace['payload_content_bytes_observed'],0)

    def test_every_scalar_tuple_arrival_preserves_exact_fields_and_total(self):
        trace=m.tuple_traces()['1175_channel_tuple_scalar_transfer']
        self.assertEqual(trace['full_published_triples'],[[1,2,3],[2,3,4],[3,4,5],[4,5,6]])
        self.assertEqual(trace['all24_completion_totals'],[42]*24)
        self.assertEqual((trace['required_send_calls'],trace['required_recv_calls'],trace['required_child_tasks']),(4,4,4))
        self.assertEqual(trace['required_channel_close_calls'],1)

    def test_every_complete_source_and_authority_is_pinned(self):
        saved=Path.read_bytes
        for name in m._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==provenance_input_path(m.ROOT, name) else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()

    def test_real_products_keep_all_names_and_unchanged_source_entries(self):
        cases={x['id']:x for x in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']}
        for row in m.expected_rows()+m.named_expected_rows():
            case=cases[row['id']];named='required_test_names' in row['expected']
            self.assertEqual(case['source'],row['source']);self.assertEqual(case['source'],case['preserved_original_identity']['source'])
            self.assertEqual(case['entry'],'test' if named else 'run');self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')
            if named:self.assertNotIn('source_context',case)
            else:self.assertEqual(case['source_context'],'original_absolute')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8'])


    def test_complete_nested_plan_error_reuses_both_original_plans(self):
        trace=m.nested_plan_cleanup_trace()
        self.assertEqual(trace['observations'],['seq-concurrent',True,'vm-concurrent',True])
        self.assertEqual(trace['original_requested_workers'],[1,2]);self.assertEqual(trace['original_print_expressions'],10)
        self.assertEqual([x['requested_outer_range_count'] for x in trace['scenarios']],[4,20000])
        self.assertEqual([x['trigger_index'] for x in trace['scenarios']],[0,10000])
        for row in trace['scenarios']:
            self.assertEqual(row['inner_callback_calls'],0);self.assertEqual(row['reference_post_failure_indices'],[0,1,2,3])
            self.assertEqual(row['reference_dispatch_ends'],2);self.assertFalse(row['reference_active_after_reuse'])
        self.assertFalse(trace['runtime_scheduling_ownership_release_cancellation_FI_provider_qualified'])

    def test_complete_map_andmapInto_panics_keep_both_full20000_ranges_and_reuse(self):
        trace=m.panic_plan_cleanup_trace()
        self.assertEqual(trace['observations'],[True]*6);self.assertEqual(trace['original_map_andmapInto_range_count'],20000)
        self.assertEqual(trace['original_total_declared_work_items'],40008);self.assertEqual(trace['original_print_expressions'],9)
        self.assertEqual([x['method'] for x in trace['failures']],['map','mapInto'])
        self.assertEqual([x['failing_index'] for x in trace['failures']],[10000,10000])
        self.assertEqual(trace['post_failure_callback_counts'],[4,4]);self.assertEqual(trace['reference_dispatch_ends'],4)
        self.assertFalse(trace['reference_active_after_reuse']);self.assertEqual(trace['unobserved_mapInto_partial_output_contents_or_rollback'],'NOT_ASSERTED')

    def test_plan_cleanup_observations_do_not_depend_on_reference_order_or_lane_count(self):
        for reverse in (False,True):
            for lanes in (1,2):
                with self.subTest(reverse=reverse,lanes=lanes):
                    self.assertEqual(m.nested_plan_cleanup_trace(reverse,lanes)['observations'],['seq-concurrent',True,'vm-concurrent',True])
                    self.assertEqual(m.panic_plan_cleanup_trace(reverse,lanes)['observations'],[True]*6)
        for lanes in (0,3,True,1.0):
            with self.subTest(lanes=lanes),self.assertRaises(ValueError):m.LogicalCleanupPlan(lanes)

    def test_rejected_nested_begin_cannot_end_the_still_active_outer_dispatch(self):
        plan=m.LogicalCleanupPlan(1);inside=[]
        def outer(state,index):
            try:plan.dispatch('forEach',1,lambda state,index:inside.append('bad'))
            except m.PlanDispatchFailure as error:self.assertEqual(error.variant,'ConcurrentDispatch')
            self.assertTrue(plan.active);self.assertEqual(plan.completed_ends,0)
        plan.dispatch('forEach',1,outer);self.assertEqual(inside,[]);self.assertFalse(plan.active);self.assertEqual(plan.completed_ends,1)

    def test_plan_ordinary_error_and_panic_channels_are_distinct_and_both_unwind(self):
        self.assertTrue(issubclass(m.PlanDispatchFailure,Exception));self.assertFalse(issubclass(m.PlanDivisionPanic,Exception))
        for error in (m.PlanDispatchFailure('ConcurrentDispatch'),m.plan_division_panic()):
            plan=m.LogicalCleanupPlan(2)
            def fail(state,index):raise error
            with self.assertRaises(type(error)):plan.dispatch('map',20000,fail)
            self.assertFalse(plan.active);self.assertEqual(plan.completed_ends,1)
            self.assertEqual(plan.dispatch('map',4,lambda state,index:index),[0,1,2,3])

    def test_plan_mapInto_clears_only_after_a_successful_begin(self):
        plan=m.LogicalCleanupPlan(1);out=[99];plan.active=True
        with self.assertRaises(m.PlanDispatchFailure):plan.dispatch('mapInto',4,lambda state,index:index,output=out)
        self.assertEqual(out,[99]);self.assertTrue(plan.active);self.assertEqual(plan.completed_ends,0)
        plan.active=False;plan.dispatch('mapInto',4,lambda state,index:index,output=out);self.assertEqual(out,[0,1,2,3])

    def test_plan_cleanup_keeps_all_original_source_prints_and_registered_observations(self):
        rows={row['original_source']:row for row in m.expected_rows() if row['source'] in m._PLAN_PROGRAMS.values()}
        for name,source in ((name,path) for name,path in m._PLAN_PROGRAMS.items() if name!=m._ACTIVE_CLOSE_NAME):
            text=(m.ROOT/source).read_text(encoding='utf-8');row=rows[source]
            self.assertEqual(row['source'],source);self.assertIsNone(row['required_target']);self.assertEqual(row['expected']['exit_code'],0)
            self.assertEqual(text.count('print('),10 if 'nested' in name else 9)
            self.assertIn('0..20000',text);self.assertIn('i == 10000',text)
            self.assertEqual(text.count('plan.mapInto('),0 if 'nested' in name else 1)
            self.assertEqual(row['observation_count'],4 if 'nested' in name else 6)

    def test_plan_panic_predicates_follow_pinned_code_and_message_and_independent_trace(self):
        with mock.patch.object(m,'plan_division_panic',lambda:m.PlanDivisionPanic(421,'changed')):
            self.assertEqual(m.panic_plan_cleanup_trace()['observations'],[False,False,True,False,False,True])
        saved=m.nested_plan_cleanup_trace
        with mock.patch.object(m,'nested_plan_cleanup_trace',lambda:{**saved(),'observations':['changed']}):
            row=next(x for x in m.expected_rows() if 'nested_dispatch' in x['source'])
            self.assertEqual(row['expected']['stdout_utf8'],'changed\n')

    def test_complete_active_close_preserves_all8items_helper_andthree_outputs(self):
        trace=m.active_plan_close_trace()
        self.assertEqual(trace['observations'],[True,'closed','done'])
        self.assertEqual(trace['original_reduction_count'],8);self.assertEqual(trace['reference_visited_indices'],list(range(8)))
        self.assertEqual(trace['original_helper_calls'],['closePlan']);self.assertEqual(trace['original_close_trigger_index'],3)
        event=trace['reference_during_close_events'][0]
        self.assertTrue(event['active']);self.assertTrue(event['closed']);self.assertTrue(event['release_pending'])
        self.assertEqual(event['state_count_still_available'],2);self.assertEqual(event['states_already_cleared'],0)
        self.assertEqual(trace['reference_after_end'],{'active':False,'closed':True,'release_pending':False,'state_count':0,'state_clears':1})
        self.assertEqual((trace['postclose_callback_calls'],trace['reference_completed_dispatch_ends'],trace['reference_close_calls'],trace['reference_final_state_clears']),(0,1,2,1))
        self.assertFalse(trace['runtime_state_release_ownership_lifetime_safety_FI_provider_qualified'])

    def test_active_close_only_observes_lower_bound_not_exact_lane_sum(self):
        for reverse in (False,True):
            for lanes in (1,2):
                with self.subTest(reverse=reverse,lanes=lanes):
                    trace=m.active_plan_close_trace(reverse,lanes)
                    self.assertEqual(trace['observations'],[True,'closed','done'])
                    self.assertEqual(trace['reference_visited_indices'],list(range(8)))
                    self.assertEqual(trace['original_observed_sum_predicate'],'sum>=108;exactsum_NOT_ASSERTED')
        # Each actual nonnegative lane contributes at least10,independent of identity/order.
        for lanes in itertools.product((0,1,7),repeat=8):
            self.assertGreaterEqual(sum(lane+10+index for index,lane in enumerate(lanes)),108)

    def test_active_close_rejects_early_state_clear_and_closed_begin_never_ends(self):
        original=m.LogicalCleanupPlan.close
        def premature(plan):
            original(plan)
            if plan.active:plan.states.clear()
        with mock.patch.object(m.LogicalCleanupPlan,'close',premature),self.assertRaises(ZeroDivisionError):m.active_plan_close_trace()
        plan=m.LogicalCleanupPlan(1,lambda lane:10);plan.close();plan.close()
        self.assertEqual(plan.state_clears,1);self.assertEqual(plan.completed_ends,0)
        with self.assertRaises(m.PlanDispatchFailure):plan.dispatch('forEach',1,lambda state,index:None)
        self.assertEqual(plan.completed_ends,0)

    def test_active_close_whole_original_and_registration_keep_all6print_expressions(self):
        source=m._ACTIVE_CLOSE_ORIGINAL;text=(m.ROOT/source).read_text(encoding='utf-8')
        self.assertEqual(text.count('print('),6);self.assertIn('plan.reduce(0..8, 0',text);self.assertIn('if (i == 3)',text)
        self.assertIn('fn closePlan()',text);self.assertEqual(text.count('plan.close()'),2)
        self.assertIn('print(sum >= 108)',text);self.assertIn('plan.forEach(0..1',text)
        row=next(x for x in m.expected_rows() if x['source']==source)
        self.assertEqual(row['expected'],{'exit_code':0,'stdout_utf8':'true\nclosed\ndone\n','stderr_utf8':''})
        self.assertEqual(row['source'],row['original_source']);self.assertEqual(row['observation_count'],3)
        case=next(x for x in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases'] if x['id']==row['id'])
        self.assertEqual(case['source'],source);self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')

if __name__=='__main__':unittest.main()
