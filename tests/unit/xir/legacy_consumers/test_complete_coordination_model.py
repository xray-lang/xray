"""Full coordination requirements, retained source conflict and named execution."""
from pathlib import Path
import json,unittest
from unittest import mock
import complete_coordination_model as m


class CompleteCoordination(unittest.TestCase):
    def test_six_legacy_and_four_system_entries_keep_five_named_bodies(self):
        rows=m.expected_rows();named=m.named_expected_rows()
        self.assertEqual((len(rows),len(named)),(10,5));self.assertEqual(sum(x['observation_count'] for x in rows),28)
        traces=m.body_traces();self.assertEqual({k:len(v['assertions']) for k,v in traces.items()},
            {'work_queue_native_methods':17,'1157_work_queue_generic_class':4,'1158_work_queue_native':33,
             '1159_work_queue_blocking_pop':4,'1160_work_queue_close_fanout':4,'1165_result_group_native':18})
        self.assertEqual(sum(len(x['assertions']) for x in traces.values()),80)
        for trace in traces.values():self.assertTrue(all(x['passed'] and x['actual']==x['expected'] for x in trace['assertions']))
        self.assertEqual(rows[0]['expected']['stdout_utf8'],'workqueue native ok\n')

    def test_declaration_entry_never_claims_named_body_execution(self):
        for row in m.expected_rows()[1:6]:
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




class CompleteSynchronization(unittest.TestCase):
    def test_all_four_whole_programs_have27_observations_and_original_context(self):
        rows=m.system_expected_rows();self.assertEqual(len(rows),4)
        self.assertEqual(sum(row['observation_count'] for row in rows),27)
        cases={row['id']:row for row in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']}
        for row in rows:
            case=cases[row['id']];self.assertEqual(case['source'],row['source'])
            self.assertEqual(case['preserved_original_identity']['source'],row['source'])
            self.assertEqual(case['source_lf_sha256'],m._INPUTS[row['source']])
            self.assertEqual(case['entry'],'run');self.assertEqual(case['source_context'],'original_absolute')
            self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8'])

    def test_recursive_failure_resets_own_flag_and_separate_set_skips_initializer(self):
        trace=m.threadlocal_trace();self.assertEqual(trace['outputs'],['recursive',7])
        self.assertEqual(trace['failed_initializer_calls'],1)
        self.assertEqual(trace['failed_initializing_flags'],{});self.assertEqual(trace['failed_cached_values'],{})
        self.assertEqual(trace['second_instance_initializer_calls'],0)

    def test_initializer_retry_and_per_thread_identity(self):
        calls=[]
        def initialize():
            calls.append(1)
            if len(calls)==1:raise LookupError('failure')
            return len(calls)
        local=m.LocalInitialization(initialize)
        with self.assertRaises(LookupError):local.get('a')
        self.assertEqual(local.states,{})
        self.assertEqual(local.get('a'),2);self.assertEqual(local.get('a'),2)
        self.assertEqual(local.get('b'),3);self.assertEqual(local.values,{'a':2,'b':3})
        self.assertEqual(local.initializations,3)

    def test_recursive_error_is_deterministic_and_does_not_cache(self):
        local=m.LocalInitialization(lambda:local.get('main'))
        for _ in range(2):
            with self.assertRaises(m.RecursiveInitialization):local.get('main')
            self.assertEqual(local.states,{});self.assertEqual(local.values,{})
        self.assertEqual(local.initializations,2)

    def test_scoped_cell_releases_after_throw_and_does_not_replace(self):
        cell=m.SerializedCell(10)
        def fail(value):raise ArithmeticError('callback')
        with self.assertRaises(ArithmeticError):cell.access(fail,replace=True)
        self.assertFalse(cell.held);self.assertEqual((cell.value,cell.writes),(10,0))
        self.assertEqual(cell.access(lambda v:v+1,try_only=True),11)

    def test_nested_try_lock_never_calls_body_or_mutates(self):
        cell=m.SerializedCell(10);calls=[]
        self.assertIsNone(cell.access(lambda value:cell.access(lambda nested:calls.append(nested),try_only=True)))
        self.assertEqual(calls,[]);self.assertFalse(cell.held)
        self.assertEqual((cell.value,cell.writes),(10,0))
        with self.assertRaises(ValueError):cell.access(lambda value:cell.access(lambda v:v))
        self.assertFalse(cell.held)

    def test_timeout_preserves_one_million_ns_and_deadline_equality(self):
        trace=m.timed_condition_trace();self.assertEqual(trace['outputs'],[False,True])
        self.assertEqual(trace['timeout_ns'],1000000)
        self.assertEqual(trace['clock_samples'],[0,1,999999,1000000])
        self.assertEqual((trace['signals_before_wait_return'],trace['signals_after_unlock'],trace['final_epoch']),(0,2,2))
        self.assertTrue(trace['lock_reacquired_before_return'])
        self.assertIn('OPEN',trace['actual_clock_wait_yield_and_native_spurious_behavior'])

    def test_all_four_workers_keep200_iterations_and800_critical_sections(self):
        trace=m.os_thread_trace();variants=trace['mutual_exclusion_variants']
        self.assertEqual(len(variants),24);self.assertEqual(len({tuple(x['worker_block_order']) for x in variants}),24)
        self.assertTrue(all(x['iterations']==[200]*4 and x['guarded']==800 and x['completed_critical_sections']==800 for x in variants))
        self.assertEqual((trace['spawned_threads'],trace['joined_threads']),(10,10))
        self.assertEqual(trace['interleaving_value_induction'],{'initial':0,'step_delta':1,'exact_completed_transactions':800,'final':800})

    def test_dropped_increment_fails_whole_original_value_requirement(self):
        with mock.patch.object(m.SerializedCell,'replace',lambda cell,body:cell.value),self.assertRaisesRegex(ValueError,'four-worker200-loop'):
            m.os_thread_trace()

    def test_condition_value_invariance_keeps_both_arrival_orders(self):
        variants=m.os_thread_trace()['condition_variants']
        self.assertEqual([x['producer_first'] for x in variants],[False,True])
        self.assertEqual([x['consumer_waits'] for x in variants],[1,0])
        self.assertTrue(all(x['observed_state']==7 and x['producer_join']==9 and x['monitor_released'] for x in variants))

    def test_barrier_requires_all_parties_and_returns_true_to_every_original_thread(self):
        barrier=m.BarrierCompletion(3)
        for thread in [0,1]:barrier.arrive(thread)
        with self.assertRaises(ValueError):barrier.result(0)
        with self.assertRaises(ValueError):barrier.arrive(0)
        barrier.arrive(2);self.assertEqual([barrier.result(thread) for thread in range(3)],[True]*3)
        with self.assertRaises(ValueError):barrier.result(3)
        with self.assertRaises(ValueError):barrier.arrive(3)
        for parties in [0,-1,True]:
            with self.assertRaises(ValueError):m.BarrierCompletion(parties)
        variants=m.os_thread_trace()['barrier_variants'];self.assertEqual(len(variants),6)
        self.assertEqual({tuple(x['arrival_order']) for x in variants},set(__import__('itertools').permutations(range(3))))
        self.assertTrue(all(x['post_barrier_reads']==[3]*3 and x['sum']==9 and x['all_wait_results']==[True]*3 for x in variants))

    def test_once_keeps_three_contenders_and_one_successful_callback(self):
        variants=m.os_thread_trace()['once_variants'];self.assertEqual(len(variants),6)
        self.assertTrue(all(x['callback_calls']==1 and x['thread_loads']==[1]*3 and x['count']==1 for x in variants))
        once=m.OnceCompletion()
        def fail():raise RuntimeError('callback failure')
        with self.assertRaises(RuntimeError):once.call(fail)
        self.assertFalse(once.done);self.assertEqual(once.calls,0)
        once.call(lambda:None);once.call(fail);self.assertEqual(once.calls,1)

    def test_imported_sync_keeps_generic_callbacks_all_imports_and_wait_order(self):
        trace=m.sync_import_trace()
        self.assertEqual(trace['outputs'],[20,11,-7,15,'a!','ab','abc',11,1,True,100,42,'done'])
        self.assertEqual(trace['rw_value_after_nonmutating_write'],'ab')
        self.assertEqual(trace['spawned_coroutines'],4);self.assertEqual(trace['latch_initial_counts'],[2,1,1])
        self.assertEqual(trace['all_imports_preserved'],['Semaphore','CountdownLatch','EventCount','WorkQueue','ResultGroup','Mutex','RwLock','Once','Barrier','Condvar'])
        self.assertEqual([x['writer_add_order'] for x in trace['fair_writer_variants']],[[1,10],[10,1]])
        self.assertTrue(all(x['result']==11 and x['latch_remaining']==0 for x in trace['fair_writer_variants']))
        self.assertTrue(all(x['output_order']==[42,'done'] and x['consumed_latch_remaining']==0 for x in trace['condition_variants']))

    def test_removed_full_program_or_authority_is_rejected(self):
        with mock.patch.dict(m._SYSTEM_PROGRAMS,{},clear=True),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.dict(m._SYSTEM_INPUTS,{},clear=True),self.assertRaises(ValueError):m.expected_rows()


if __name__=='__main__':unittest.main()
