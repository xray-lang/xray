from pathlib import Path
from types import SimpleNamespace
import copy,itertools,json,re,unittest
from unittest import mock
import complete_frame_release_model as model
import complete_target_selection as selection
import run_frame_release_driver as driver


class CompleteFrameRelease(unittest.TestCase):
    def result(self,text):return SimpleNamespace(returncode=0,timed_out=False,stderr=b'',stdout=text.encode())

    def test_all200_generators_take64and66_then_abandon_without_another_body_statement(self):
        trace=model.generator_drop_trace()
        self.assertEqual(trace['payload_values'],[64,65,66]);self.assertEqual(trace['per_round_values'],[64,66])
        self.assertEqual((trace['sum'],trace['yield_observations'],trace['logical_abandons']),(26000,400,200))
        self.assertEqual(trace['suspended_i_at_abandon'],1);self.assertEqual(trace['post_break_generator_body_resumes'],0)
        self.assertFalse(trace['physical_byte_or_release_count_measured'])
        self.assertEqual(model.generator_drop_trace(1,3,2)['sum'],8)
        for values in ((201,64,2),(200,1,2),(True,64,2),(200,64,0)):
            with self.assertRaises(ValueError):model.generator_drop_trace(*values)

    def test_all6561_cancel_checkpoint_shapes_keep8_results_without_claiming8_suspended_frames(self):
        count=0
        for positions in itertools.product(model.CANCEL_POSITIONS,repeat=8):
            trace=model.cancelled_drop_trace(positions);count+=1
            self.assertEqual(trace['count'],8);self.assertEqual(trace['results'],['Cancelled']*8)
            self.assertFalse(trace['suspended_frames_guaranteed_by_sleep'])
            self.assertTrue(trace['channel_has_no_sender_until_after_result'])
        self.assertEqual(count,6561)
        for positions in ((),('not_started',)*7,('completed',)*8):
            with self.assertRaises(ValueError):model.cancelled_drop_trace(positions)

    def test_50_plain_awaits_return50_independently_of_unobserved_handles_completing(self):
        for finished in ((False,)*50,(True,)*50,tuple(index%2==0 for index in range(50))):
            trace=model.dropped_task_trace(forgotten_finished=finished)
            self.assertEqual((trace['return'],trace['spawned_workers'],trace['plain_awaits'],trace['dropped_unawaited_handles']),(50,100,50,50))
            self.assertFalse(trace['forgotten_completion_required_by_return'])
        self.assertEqual(model.dropped_task_trace((3,)*49+(2,))['return'],-1)
        for values in ((3,)*49,(3,)*49+(True,)):
            with self.assertRaises(ValueError):model.dropped_task_trace(values)

    def test_original_strict65536_bound_uses_actual_counter_inputs_without_inventing_readings(self):
        self.assertTrue(model.meter_verdict(1000,1000));self.assertTrue(model.meter_verdict(1000,999))
        self.assertTrue(model.meter_verdict(1000,1000+65535))
        self.assertFalse(model.meter_verdict(1000,1000+65536));self.assertFalse(model.meter_verdict(1000,1000+65537))
        for values in ((True,0),(-1,0),(0,-1),((1<<63)-1,0),(0,1<<63)):
            with self.assertRaises(ValueError):model.meter_verdict(*values)
        row=model.expected_rows()[0]
        self.assertEqual((row['independently_derived_algorithmic_output_slots'],row['required_actual_meter_predicate_output_slots']),(3,1))
        self.assertTrue(row['required_semantic_trace']['expected_true_is_required_acceptance_condition_not_predicted_byte_count'])

    def test_counterexample_small_or_other_domain_leaks_can_pass_original_predicate(self):
        self.assertTrue(model.meter_verdict(0,4800))
        parent_base,parent_after,unobserved_child_bytes=0,0,100000
        self.assertTrue(model.meter_verdict(parent_base,parent_after));self.assertGreater(unobserved_child_bytes,65536)
        facts=model.expected_rows()[0]['required_semantic_trace']
        self.assertTrue(facts['small_leak_can_still_satisfy_original_bound'])
        self.assertTrue(facts['child_domain_leak_can_be_invisible_to_parent_counter'])
        self.assertTrue(facts['AOT_counter_not_assumed_constant_zero'])
        self.assertFalse(facts['full_physical_release_runtime_FI_safety_provider_mixed_qualified'])

    def test_whole_original_six_helpers_three_drop_shapes_all_rounds_sleeps_and_meter_reads_retained(self):
        source=(model.ROOT/model.SOURCE).read_text(encoding='utf-8')
        code=re.sub(r'/\*.*?\*/|//[^\n]*','',source,flags=re.S)
        for token,count in (('fn ',6),('go ',3),('awaitResult()',1),('await awaited',1),('.cancel()',1),('.close()',1),('yield ',1),('print(',4),('runtime.liveBytes()',2)):
            self.assertEqual(code.count(token),count,token)
        for token in ('counts(64)','taken == 2','drop_generators(200)','drop_cancelled(8)','drop_tasks(50)',
                      'Channel<i64>(1)','time.sleep(1)','time.sleep(5)','after < base + 65536'):
            self.assertIn(token,code)
        self.assertNotIn('@test',code)
        self.assertEqual(model.expected_values(),[26000,8,50,True])

    def test_original_every_public_meter_and_current_domain_authority_cannot_drift_or_be_dropped(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            def changed(path):return saved(path)+b'changed' if path==model.ROOT/name else saved(path)
            with self.subTest(path=name),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
        with mock.patch.object(model,'_INPUTS',{}),self.assertRaises(ValueError):model.expected_rows()

    def test_every_algorithmic_and_resource_line_exit_stderr_and_tail_is_required(self):
        row=model.expected_rows()[0];text=row['expected']['stdout_utf8'];self.assertTrue(model.matches(row,self.result(text)))
        lines=text.splitlines()
        for index in range(4):
            changed=list(lines);changed[index]='wrong';self.assertFalse(model.matches(row,self.result('\n'.join(changed)+'\n')))
        for text in ('26000\n8\n50\nfalse\n','26000\n8\n50\n','26000\n8\n50\ntrue\nextra\n'):
            self.assertFalse(model.matches(row,self.result(text)))
        for attr,value in (('returncode',1),('stderr',b'error'),('timed_out',True)):
            result=self.result(row['expected']['stdout_utf8']);setattr(result,attr,value);self.assertFalse(model.matches(row,result))
        self.assertFalse(model.matches(row,None))

    def test_owned_manifest_complete_domain_and_resource_role_metadata_cannot_be_forged(self):
        bound=json.loads((Path(__file__).parent/'frame_release_driver_cases.json').read_text());driver.validate_cases(bound)
        for key,value in (('source','other.xr'),('arguments',['x']),('stdin_hex','31'),('companions',[{'path':'x'}]),('stdout_utf8','wrong\n')):
            changed=copy.deepcopy(bound);changed['cases'][0][key]=value
            with self.assertRaises(ValueError):driver.validate_cases(changed)
        row=model.expected_rows()[0]
        for key,value in (('id','other'),('source','other.xr'),('original_source','other.xr'),('observation_count',3),
                          ('independently_derived_algorithmic_output_slots',4),('required_actual_meter_predicate_output_slots',0)):
            changed=copy.deepcopy(row);changed[key]=value
            with self.assertRaises(ValueError):model.expected_outputs(changed)
        changed=copy.deepcopy(row);changed['required_semantic_trace']['resource_predicate']='after <= base + 65536'
        with self.assertRaises(ValueError):model.expected_outputs(changed)
        for options in ([],row['expected']['stdout_utf8_options']*2,['26000\n8\n50\nfalse\n']):
            changed=copy.deepcopy(row);changed['expected']['stdout_utf8_options']=options
            with self.assertRaises(ValueError):model.expected_outputs(changed)

    def test_full741_selector_and_task_factory_keep_whole_program_without_physical_proof_transfer(self):
        import complete_task_regression_model as tasks
        row=model.expected_rows()[0];selected={'rejection_oracle_required':False,'canonical_port_selected':False}
        actual=self.result(row['expected']['stdout_utf8'])
        self.assertEqual(selection.observation_status(selected,row,actual,actual,True),'PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE')
        self.assertEqual(selection.observation_status(selected,row,actual,actual,False),'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        self.assertEqual([item for item in tasks.expected_rows() if item['id']==model.IDENTITY],[row])
        self.assertFalse(row['required_semantic_trace']['actual_suspended_startup_timer_progress_or_final_detached_drain_qualified'])


if __name__=='__main__':unittest.main()
