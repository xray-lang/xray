from provenance_test_support import provenance_input_path
from pathlib import Path
from types import SimpleNamespace
import copy,json,unittest
from unittest import mock
import complete_task_status_model as model
import complete_target_selection as selection
import run_task_status_driver as driver


class CompleteTaskStatusDomain(unittest.TestCase):
    def result(self,stdout):
        return SimpleNamespace(returncode=0,timed_out=False,stderr=b'',stdout=stdout)

    def test_complete_nonterminal_start_shapes_keep_all14_lines_and_both_helpers(self):
        outputs=model.output_options();self.assertEqual(len(set(outputs)),2)
        for state,text in zip(('Pending','Running'),outputs):
            trace=model.status_trace(state);self.assertEqual(len(trace['values']),14)
            self.assertEqual(text.splitlines()[2],'  status: TaskStatus.'+state)
            self.assertEqual(text.splitlines()[3:],['after await:','  done: true','  result: 21',
                'after cancel:','  done: true','  status: TaskStatus.Cancelled','  cancelled result: true',
                'completed:','  done: true','  status: TaskStatus.Success','  result: 42'])
            self.assertEqual((trace['original_workers'],trace['original_helpers'],trace['original_plain_awaits'],trace['original_awaitResult_calls']),(3,2,2,1))
            self.assertTrue(trace['cancellation_waits_on_no_sender_not_on_a_short_sleep'])
            self.assertFalse(trace['physical_scheduler_start_order_or_latency_measured'])

    def test_terminal_or_coro_states_cannot_replace_pre_send_task_status(self):
        for state in ('Success','Failed','Cancelled','Blocked','Timeout','Unknown',''):
            with self.assertRaises(ValueError):model.status_trace(state)

    def test_owner_rejects_changed_id_source_or_incomplete_expanded_reordered_domains(self):
        row=model.expected_rows()[0]
        for field,value in (('id','complete_network_fake'),('source','other.xr'),('original_source','other.xr'),('observation_count',13)):
            changed=copy.deepcopy(row);changed[field]=value
            with self.assertRaises(ValueError):model.expected_outputs(changed)
        options=row['expected']['stdout_utf8_options']
        for values in (options[:1],options+[options[0]],options+['extra\n'],list(reversed(options))):
            changed=copy.deepcopy(row);changed['expected']['stdout_utf8_options']=values
            with self.assertRaises(ValueError):model.expected_outputs(changed)
        changed=copy.deepcopy(row);changed['expected']['stdout_utf8']='prefix\n'
        with self.assertRaises(ValueError):model.expected_outputs(changed)

    def test_every_original_output_line_exit_and_stderr_remain_required(self):
        row=model.expected_rows()[0]
        for text in model.output_options():
            self.assertTrue(model.matches(row,self.result(text.encode())))
            lines=text.splitlines()
            for index in range(14):
                changed=list(lines);changed[index]+='-wrong'
                self.assertFalse(model.matches(row,self.result(('\n'.join(changed)+'\n').encode())))
            for bad in (text+'extra\n','\n'.join(lines[:-1])+'\n',text.replace('TaskStatus.','')):
                self.assertFalse(model.matches(row,self.result(bad.encode())))
        for attr,value in (('returncode',1),('timed_out',True),('stderr',b'error')):
            result=self.result(model.output_options()[0].encode());setattr(result,attr,value)
            self.assertFalse(model.matches(row,result))
        self.assertFalse(model.matches(row,None))

    def test_full741_selector_accepts_different_legal_start_observations_independently(self):
        row=model.expected_rows()[0];a,b=map(str.encode,model.output_options())
        selected={'rejection_oracle_required':False,'canonical_port_selected':False}
        self.assertEqual(selection.observation_status(selected,row,self.result(a),self.result(b),True),'PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE')
        self.assertEqual(selection.observation_status(selected,row,self.result(a),self.result(b),False),'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        self.assertEqual(selection.observation_status(selected,row,self.result(a),self.result(b+b),True),'FAIL_INDEPENDENT_SELECTED_CONSUMER')

    def test_complete_source_all_authorities_and_program_operations_cannot_drift(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            def changed(path):return saved(path)+b'changed' if path==provenance_input_path(model.ROOT, name) else saved(path)
            with self.subTest(name=name),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
        with mock.patch.object(model,'_INPUTS',{}),self.assertRaises(ValueError):model.expected_rows()
        text=(model.ROOT/model.SOURCE).read_text(encoding='utf-8')
        for token,count in (('go ',3),('await task',2),('awaitResult()',1),('.cancel()',1),('.close()',2),('print(',14)):
            # Ignore old expected-output comments,which contain no operations.
            self.assertEqual(text.count(token),count)
        self.assertTrue(text.rstrip().endswith('test_main()'))
        self.assertIn('fn blocked(ch: Channel<i64>)',text);self.assertIn('fn compute(x: i64)',text)

    def test_new_manifest_cannot_change_original_args_stdin_companions_or_finite_values(self):
        bound=json.loads((Path(__file__).parent/'task_status_driver_cases.json').read_text(encoding='utf-8'));driver.validate_cases(bound)
        for key,value in (('source','other.xr'),('arguments',['x']),('stdin_hex','31'),('companions',[{'path':'x'}]),
                          ('stdout_utf8_options',model.output_options()[:1]),('stdout_utf8','wrong\n')):
            changed=copy.deepcopy(bound);changed['cases'][0][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):driver.validate_cases(changed)

    def test_task_factory_delegates_full_row_without_granting_named_or_runtime_qualification(self):
        import complete_task_regression_model as tasks
        row=model.expected_rows()[0];rows=[x for x in tasks.expected_rows() if x['id']==model.IDENTITY]
        self.assertEqual(rows,[row]);self.assertFalse(row['separate_original_named_collection_obligation']['qualified_by_root_run'])
        self.assertFalse(row['required_semantic_traces'][0]['runtime_channel_cancel_resources_lifetime_FI_safety_provider_mixed_qualified'])


if __name__=='__main__':unittest.main()
