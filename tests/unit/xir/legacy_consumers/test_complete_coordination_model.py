"""Full coordination requirements, retained source conflict and named execution."""
from pathlib import Path
import json,unittest
from unittest import mock
import complete_coordination_model as m


class CompleteCoordination(unittest.TestCase):
    def test_all_six_complete_entries_and_five_named_bodies(self):
        rows=m.expected_rows();named=m.named_expected_rows()
        self.assertEqual((len(rows),len(named)),(6,5));self.assertEqual(sum(x['observation_count'] for x in rows),1)
        traces=m.body_traces();self.assertEqual({k:len(v['assertions']) for k,v in traces.items()},
            {'work_queue_native_methods':17,'1157_work_queue_generic_class':4,'1158_work_queue_native':33,
             '1159_work_queue_blocking_pop':4,'1160_work_queue_close_fanout':4,'1165_result_group_native':18})
        self.assertEqual(sum(len(x['assertions']) for x in traces.values()),80)
        for trace in traces.values():self.assertTrue(all(x['passed'] and x['actual']==x['expected'] for x in trace['assertions']))
        self.assertEqual(rows[0]['expected']['stdout_utf8'],'workqueue native ok\n')

    def test_declaration_entry_never_claims_named_body_execution(self):
        for row in m.expected_rows()[1:]:
            self.assertEqual(row['expected']['stdout_utf8'],'');self.assertEqual(row['observation_count'],0)
            self.assertFalse(row['separate_named_body_obligation']['executed_by_run'])
            self.assertEqual(row['separate_named_body_obligation']['qualification'],'OPEN_REQUIRES_NAMED_TEST_EXECUTION')
        self.assertEqual(sum(len(x['required_original_assertion_results']) for x in m.named_expected_rows()),63)

    def test_original_local_lifo_and_unique_remaining_shard_results(self):
        trace=m.native_trace(True)
        self.assertEqual(trace['pop_values'],[2,3,1]);self.assertEqual(trace['pushRange_values'],[14,15,12,13,10,11])
        self.assertEqual(trace['final_length'],0);self.assertTrue(trace['closed'])
        q=m.LegacyQueue(4,2);q.push(9,2);self.assertEqual(q.pop(0),(9,True));self.assertEqual(q.pop(0),(None,False))
        q.close();self.assertFalse(q.push(1,0));self.assertEqual(q.push_range(20,2,0),0)

    def test_current_fifo_projection_stays_an_explicit_legacy_conflict(self):
        witness=m.fifo_source_witness()
        self.assertEqual(witness['current_source_projected_pops'],[1,2,3])
        self.assertEqual(witness['current_source_projected_range'],[10,11,12,13,14,15])
        self.assertFalse(witness['equivalent']);self.assertNotEqual(witness['current_source_projected_pops'],witness['required_original_pops'])
        for row in [m.expected_rows()[0],m.expected_rows()[2]]:self.assertEqual(row['current_source_policy_witness'],witness)
        self.assertIn('NOT_PRODUCT_RUNTIME',witness['classification'])

    def test_fifo_substitution_fails_the_original_complete_body(self):
        def fifo(queue,worker):
            own=queue.shards[worker%len(queue.shards)]
            return (own.pop(0),True) if own else (None,False)
        with mock.patch.object(m.LegacyQueue,'pop',fifo),self.assertRaisesRegex(ValueError,'Original coordination assertion'):
            m.expected_rows()

    def test_no_multivictim_tie_policy_is_invented(self):
        q=m.LegacyQueue(4,2);q.push(1,0);q.push(2,1)
        with self.assertRaisesRegex(ValueError,'multi-victim'):q.pop(2)
        with self.assertRaises(ValueError):q.push(1,-1)
        with self.assertRaises(ValueError):m.LegacyQueue(0,2)
        with self.assertRaises(ValueError):m.LegacyQueue(4,0)

    def test_generic_channel_queue_preserves_all_four_channels_and_calls(self):
        trace=m.generic_trace();self.assertEqual([x['actual'] for x in trace['assertions']],[10,20,30,None])
        self.assertEqual((trace['constructed_channels'],trace['channel_capacity'],trace['closed_channels']),(4,8,4))
        self.assertTrue(trace['channel_values_drained'])

    def test_singleton_blocking_value_invariance_covers_both_start_orders(self):
        trace=m.blocking_trace();variants=trace['finite_completion_variants']
        self.assertEqual([x['event_order'] for x in variants],[['consumer','producer'],['producer','consumer']])
        for value in variants:
            self.assertEqual([x['actual'] for x in value['assertions']],[True,42,42,None]);self.assertEqual(value['final_length'],0)
        self.assertEqual(trace['required_original_sleep_calls'],[1]);self.assertEqual(trace['spawned_tasks'],2)
        self.assertIn('does not prove',trace['actual_block_and_wakeup'])

    def test_all120_close_and_task_start_orders_preserve_four_await_results(self):
        trace=m.fanout_trace();variants=trace['finite_completion_variants']
        self.assertEqual(len(variants),120);self.assertEqual(len({tuple(x['events']) for x in variants}),120)
        self.assertTrue(all(x['await_all_values']==[-1]*4 for x in variants))
        self.assertEqual(trace['required_original_sleep_calls'],[1]);self.assertEqual(trace['spawned_tasks'],4)
        self.assertEqual({x['events'].index('close') for x in variants},set(range(5)))

    def test_result_batches_preserve_all18_original_assertions(self):
        trace=m.result_group_trace()
        self.assertEqual(trace['accepted_values'],[4,5,3]);self.assertEqual(trace['received_batches'],[9,3])
        self.assertEqual([x['actual'] for x in trace['assertions']],
            [2,0,True,True,0,1,True,1,2,9,0,0,True,True,True,3,None,True])
        self.assertTrue(trace['closed'])

    def test_partial_batch_counter_projection_is_not_silently_repaired(self):
        trace=m.result_group_trace();self.assertEqual(trace['partial_counter_source_arithmetic_projection'],-1)
        self.assertIn('no pendingCount read',trace['unobserved_partial_counter_policy'])
        self.assertNotIn('pending after partial recv',[x['label'] for x in trace['assertions']])

    def test_every_complete_source_and_normative_input_is_pinned(self):
        saved=Path.read_bytes
        for name in m._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()

    def test_required_named_identity_and_original_argument_obligations(self):
        rows=m.named_expected_rows()
        self.assertEqual({x['source'] for x in rows},{m._PROGRAMS[k] for k in m._NAMED})
        self.assertEqual({x['expected']['required_test_names'][0] for x in rows},set(m._NAMED.values()))
        self.assertTrue(all(x['expected']['exit_code']==0 and x['expected']['stdout_utf8']=='' for x in rows))
        with mock.patch.object(m,'_NAMED',{}),self.assertRaises(ValueError):m.named_expected_rows()

    def test_product_entries_keep_unmodified_run_and_separate_real_test_routes(self):
        folder=Path(__file__).parent;cases={x['id']:x for x in json.loads((folder/'product_driver_cases.json').read_text())['cases']}
        for row in m.expected_rows()+m.named_expected_rows():
            case=cases[row['id']];self.assertEqual(case['source'],row['source'])
            self.assertEqual(case['source'],case['preserved_original_identity']['source'])
            self.assertEqual(case['entry'],'test' if 'required_test_names' in row['expected'] else 'run')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8']);self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')
            if case['entry']=='test':self.assertNotIn('source_context',case)
            else:self.assertEqual(case['source_context'],'original_absolute')


if __name__=='__main__':unittest.main()
