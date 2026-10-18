from provenance_test_support import provenance_input_path
from pathlib import Path
from types import SimpleNamespace
import copy,itertools,json,unittest
from unittest import mock
import complete_await_timeout_model as model
import complete_target_selection as selection
import run_await_timeout_driver as driver


class CompleteAwaitTimeout(unittest.TestCase):
    def result(self,text):return SimpleNamespace(returncode=0,timed_out=False,stderr=b'',stdout=text.encode())

    def test_all8_complete_outcome_shapes_keep_already_done_value_and_final_plain_await(self):
        texts=set()
        for shape in itertools.product((False,True),repeat=3):
            trace=model.timeout_trace(*shape);texts.add(''.join(value+'\n' for value in trace['values']))
            self.assertEqual(len(trace['values']),8)
            self.assertEqual(trace['values'][5],'  result: 42')
            self.assertEqual(trace['outcome_tags'][2],'Success')
            self.assertEqual(trace['post_timeout_plain_await_slow_value'],7)
            self.assertTrue(trace['timeout_observation_did_not_cancel_slow'])
        self.assertEqual(texts,set(model.output_options()));self.assertEqual(len(texts),8)

    def test_normal_completion_and_deadline_outcomes_do_not_force_host_latency(self):
        for completed in (False,True):
            trace=model.timeout_trace(completed,completed,completed)
            self.assertEqual(trace['values'][1],'  result: '+('42' if completed else '-1'))
            self.assertEqual(trace['values'][7],'  timeout: '+('false' if completed else 'true'))
            self.assertFalse(trace['actual_timer_firing_latency_fairness_or_deadline_winner_measured'])
            self.assertFalse(trace['runtime_FI_resources_lifetime_cancellation_safety_provider_mixed_qualified'])
        with self.assertRaises(ValueError):model.timeout_trace(1,True,False)

    def test_timeout_is_an_observation_not_cancellation_and_completed_value_is_reusable(self):
        task=model.LogicalTask(7)
        self.assertEqual(task.await_timeout(1,False),('Timeout',None))
        self.assertFalse(task.completed);self.assertFalse(task.cancelled)
        self.assertEqual(task.await_value(),7)
        self.assertEqual(task.await_timeout(1,False),('Success',7))
        self.assertEqual(task.timeout_observations,1)
        for duration,shape in ((0,False),(-1,False),(True,False),(1,1)):
            with self.assertRaises(ValueError):task.await_timeout(duration,shape)

    def test_original_two_helpers_named_wrapper_all_prints_go_waits_and_durations_retained(self):
        source=(model.ROOT/model.SOURCE).read_text(encoding='utf-8')
        for token,count in (('fn ',3),('go ',4),('awaitTimeout(',5),('await task',2),('print(',8),('@test',1),('time.sleep(50)',1)):
            self.assertEqual(source.count(token),count)
        for token in ('task1.awaitTimeout(1000)','task2.awaitTimeout(100)','task3.awaitTimeout(100)','task4.awaitTimeout(1)'):
            self.assertIn(token,source)
        self.assertTrue(source.rstrip().endswith('test_await_timeout()'))
        row=model.expected_rows()[0]
        self.assertFalse(row['separate_original_named_collection_obligation']['qualified_by_root_run'])

    def test_complete_original_and_every_authority_cannot_be_changed_or_dropped(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            def changed(path):return saved(path)+b'changed' if path==provenance_input_path(model.ROOT, name) else saved(path)
            with self.subTest(name=name),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
        with mock.patch.object(model,'_INPUTS',{}),self.assertRaises(ValueError):model.expected_rows()

    def test_each8line_output_slot_metadata_exit_stderr_and_tail_remain_required(self):
        row=model.expected_rows()[0]
        for text in model.output_options():
            self.assertTrue(model.matches(row,self.result(text)))
            lines=text.splitlines()
            for index in range(8):
                changed=list(lines);changed[index]+='wrong'
                self.assertFalse(model.matches(row,self.result('\n'.join(changed)+'\n')))
            for changed in (text+'extra\n','\n'.join(lines[:-1])+'\n',text.replace('  result: 42\ntest4','  result: -1\ntest4')):
                self.assertFalse(model.matches(row,self.result(changed)))
        for attr,value in (('returncode',1),('stderr',b'error'),('timed_out',True)):
            result=self.result(row['expected']['stdout_utf8']);setattr(result,attr,value);self.assertFalse(model.matches(row,result))
        self.assertFalse(model.matches(row,None))

    def test_closed_manifest_and_owner_reject_foreign_or_changed_or_shrunk_domains(self):
        bound=json.loads((Path(__file__).parent/'await_timeout_driver_cases.json').read_text());driver.validate_cases(bound)
        for key,value in (('source','other.xr'),('arguments',['x']),('stdin_hex','31'),('companions',[{'path':'x'}]),('stdout_utf8','wrong\n')):
            changed=copy.deepcopy(bound);changed['cases'][0][key]=value
            with self.assertRaises(ValueError):driver.validate_cases(changed)
        row=model.expected_rows()[0]
        for key,value in (('id','other'),('source','other.xr'),('original_source','other.xr'),('observation_count',0)):
            changed=copy.deepcopy(row);changed[key]=value
            with self.assertRaises(ValueError):model.expected_outputs(changed)
        values=row['expected']['stdout_utf8_options']
        for options in (values[:-1],values+[values[0]],values+['extra\n'],list(reversed(values))):
            changed=copy.deepcopy(row);changed['expected']['stdout_utf8_options']=options
            with self.assertRaises(ValueError):model.expected_outputs(changed)

    def test_full741_selector_checks_each_backend_whole_domain_and_task_factory_delegates(self):
        import complete_task_regression_model as task
        row=model.expected_rows()[0];selected={'rejection_oracle_required':False,'canonical_port_selected':False}
        options=row['expected']['stdout_utf8_options']
        self.assertEqual(selection.observation_status(selected,row,self.result(options[0]),self.result(options[-1]),True),'PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE')
        self.assertEqual(selection.observation_status(selected,row,self.result(options[0]),self.result(options[-1]),False),'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        self.assertEqual([item for item in task.expected_rows() if item['id']==model.IDENTITY],[row])


if __name__=='__main__':unittest.main()
