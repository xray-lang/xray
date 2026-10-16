"""Whole parallel workload values, unchanged counts and runtime-proof limits."""
from pathlib import Path
import itertools,json,unittest
from unittest import mock
import complete_parallel_workload_model as m


class CompleteParallelWorkloads(unittest.TestCase):
    def test_seven_complete_originals_and_all_thirteen_named_bodies(self):
        rows=m.expected_rows();named=m.named_expected_rows();bodies=m.bodies()
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
                def changed(path):return saved(path)+b'\nchanged\n' if path==m.ROOT/name else saved(path)
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


if __name__=='__main__':unittest.main()
