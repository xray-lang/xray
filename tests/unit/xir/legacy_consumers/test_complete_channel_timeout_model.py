from provenance_test_support import provenance_input_path
from pathlib import Path
from types import SimpleNamespace
import copy,json,re,unittest
from unittest import mock
import complete_channel_timeout_model as model
import complete_target_selection as selection
import run_channel_timeout_driver as driver


class CompleteChannelTimeout(unittest.TestCase):
    def result(self,text):return SimpleNamespace(returncode=0,timed_out=False,stderr=b'',stdout=text.encode())

    def test_first_four_original_channels_keep_ready_empty_space_and_full_postconditions(self):
        trace=model.fixed_prefix_trace()
        self.assertEqual(trace['lines'],['test1: recvTimeout with data','  result: 42 ok: true',
            'test2: recvTimeout timeout','  timeout','test3: sendTimeout with space',
            '  success: true','  value: 100','test4: sendTimeout timeout','  success: false',
            'test5: producer-consumer with timeout'])
        self.assertEqual(trace['queues_after_first_four'],[[],[],[],[1]])
        self.assertTrue(trace['first_four_channels_still_open'])

    def test_all76_states107_edges_yield7_whole_paths_with_replayable_single_winner_witnesses(self):
        traces,states,edges=model.enumerate_traces();self.assertEqual((states,edges,len(traces)),(76,107,7))
        for (log,count),witness in traces.items():
            state=(0,0,(),None,0,False,())
            for action in witness:
                choices=[following for label,following in model.successors(state) if label==action]
                self.assertEqual(len(choices),1);state=choices[0]
            self.assertEqual((state[0],state[1],state[4],state[5],state[-1]),(6,3,count,True,log))
            self.assertEqual([value for event,value in log if event=='consume'],list(range(1,count+1)))
        self.assertEqual(sorted({len(text.splitlines()) for text in model.output_options()}),[12,13,14])

    def test_only_third_send_can_timeout_and_buffered_second_item_cannot_be_skipped(self):
        traces,_,_=model.enumerate_traces()
        failure={(log,count) for log,count in traces if ('timeout',3) in log}
        self.assertEqual(len(failure),4)
        self.assertEqual({count for log,count in failure},{0,2})
        self.assertEqual({log.index(('timeout',3)) for log,count in failure if count==2},{0,1,2})
        self.assertEqual({count for log,count in traces if ('timeout',3) not in log},{1,2,3})
        self.assertTrue(all(sum(event=='timeout' for event,_ in log)<=1 for log,_ in traces))

    def test_buffer_receive_promotes_pending_sender_atomically_before_another_timeout(self):
        state=(3,0,(1,2),None,0,False,())
        choices=dict(model.successors(state));self.assertIn('send3-timeout-wins',choices)
        received=choices['receive-buffer-1']
        self.assertEqual(received[:4],(5,2,(2,3),1))
        self.assertNotIn('send3-timeout-wins',dict(model.successors(received)))
        failed=choices['send3-timeout-wins'];self.assertEqual(failed[:3],(4,0,(1,2)))

    def test_direct_delivery_or_close_resolves_one_waiter_and_closed_buffers_still_drain(self):
        waiting=(0,1,(),None,0,False,())
        delivered=dict(model.successors(waiting))['send-direct-1']
        self.assertEqual(delivered[:4],(1,2,(),1))
        self.assertNotIn('receive-timeout-wins',dict(model.successors(delivered)))
        closed=dict(model.successors((5,1,(),None,0,False,())))['close']
        self.assertEqual((closed[0],closed[1],closed[5]),(6,3,True))
        buffered=dict(model.successors((6,0,(1,2),None,0,True,())))['receive-buffer-1']
        self.assertEqual(buffered[:4],(6,2,(2,),1))

    def test_complete_original_wrapper_root_all15_static_prints_operations_and_budgets_retained(self):
        original=(model.ROOT/model.SOURCE).read_text(encoding='utf-8')
        source=re.sub(r'/\*.*?\*/|//[^\n]*','',original,flags=re.S)
        self.assertEqual(len(re.findall(r'\bfn\b',source)),3)
        for token,count in (('go ',2),('.sendTimeout(',3),('.recvTimeout(',3),('await ',2),('print(',15),('@test',1),('.close()',1)):
            self.assertEqual(source.count(token),count,token)
        for token in ('Channel(1)','Channel(2)','ch1.recvTimeout(1000)','ch2.recvTimeout(100)',
                      'ch3.sendTimeout(100, 1000)','ch4.sendTimeout(2, 100)',
                      'ch5.sendTimeout(i, 500)','ch5.recvTimeout(200)','for (i in 1..4)'):
            self.assertIn(token,source)
        self.assertEqual(source.count('Channel(1)'),4);self.assertTrue(source.rstrip().endswith('test_channel_timeout()'))

    def test_every_preserved_source_and_authority_is_required_and_cannot_drift(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            def changed(path):return saved(path)+b'changed' if path==provenance_input_path(model.ROOT, name) else saved(path)
            with self.subTest(path=name),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
        with mock.patch.object(model,'_INPUTS',{}),self.assertRaises(ValueError):model.expected_rows()

    def test_every_variable_length_complete_output_slot_and_final_count_exit_stderr_are_required(self):
        row=model.expected_rows()[0]
        for text in model.output_options():
            self.assertTrue(model.matches(row,self.result(text)))
            lines=text.splitlines()
            for index in range(len(lines)):
                changed=list(lines);changed[index]+='wrong';self.assertFalse(model.matches(row,self.result('\n'.join(changed)+'\n')))
            for value in (text+'extra\n','\n'.join(lines[:-1])+'\n'):
                self.assertFalse(model.matches(row,self.result(value)))
        for eventlog,count in (((('timeout',3),('consume',1)),1),((),0),((('consume',2),),1),((('consume',1),('consume',1)),2)):
            self.assertFalse(model.matches(row,self.result(model.render(eventlog,count))))
        for attr,value in (('returncode',1),('stderr',b'error'),('timed_out',True)):
            result=self.result(row['expected']['stdout_utf8']);setattr(result,attr,value);self.assertFalse(model.matches(row,result))
        self.assertFalse(model.matches(row,None))

    def test_owned_manifest_and_closed_domain_reject_foreign_changed_shrunk_or_expanded_inputs(self):
        bound=json.loads((Path(__file__).parent/'channel_timeout_driver_cases.json').read_text());driver.validate_cases(bound)
        for key,value in (('source','other.xr'),('arguments',['x']),('stdin_hex','31'),('companions',[{'path':'x'}]),('stdout_utf8','wrong\n')):
            changed=copy.deepcopy(bound);changed['cases'][0][key]=value
            with self.assertRaises(ValueError):driver.validate_cases(changed)
        row=model.expected_rows()[0]
        for key,value in (('id','other'),('source','other.xr'),('original_source','other.xr'),('observation_count',12),('complete_output_line_counts',[14])):
            changed=copy.deepcopy(row);changed[key]=value
            with self.assertRaises(ValueError):model.expected_outputs(changed)
        values=row['expected']['stdout_utf8_options']
        for options in (values[:-1],values+[values[0]],values+['extra\n'],list(reversed(values))):
            changed=copy.deepcopy(row);changed['expected']['stdout_utf8_options']=options
            with self.assertRaises(ValueError):model.expected_outputs(changed)

    def test_full741_selector_and_task_factory_keep_whole_paths_without_granting_named_or_clock_proofs(self):
        import complete_task_regression_model as task
        row=model.expected_rows()[0];selected={'rejection_oracle_required':False,'canonical_port_selected':False}
        options=model.output_options()
        self.assertEqual(selection.observation_status(selected,row,self.result(options[0]),self.result(options[-1]),True),'PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE')
        self.assertEqual(selection.observation_status(selected,row,self.result(options[0]),self.result(options[-1]),False),'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        self.assertEqual([item for item in task.expected_rows() if item['id']==model.IDENTITY],[row])
        self.assertFalse(row['separate_original_named_collection_obligation']['qualified_by_root_run'])
        self.assertEqual(row['observation_count'],len(row['expected']['stdout_utf8'].splitlines()))
        self.assertFalse(row['required_semantic_trace']['actual_timer_latency_fairness_physical_release_or_runtime_FI_safety_provider_mixed_qualified'])


if __name__=='__main__':unittest.main()
