from provenance_test_support import provenance_input_path
from pathlib import Path
from types import SimpleNamespace
from collections import Counter
import copy,itertools,json,unittest
from unittest import mock
import complete_parallel_api_model as model
import complete_target_selection as selection
import run_parallel_api_driver as driver


class CompleteParallelApi(unittest.TestCase):
    def result(self,text):return SimpleNamespace(returncode=0,timed_out=False,stderr=b'',stdout=text.encode())

    def test_free_api_outputs_keep_every_map_and_overwrite_observation(self):
        trace=model.api_trace(3)
        self.assertEqual(trace['values'][:8],[10,4,0,9,3,11,13,20])
        self.assertEqual(trace['mapInto_before'],[99]);self.assertEqual(trace['mapInto_after'],[11,12,13])
        self.assertEqual(trace['free_callback_item_counts'],[5,4,3,5])
        self.assertEqual(trace['values'][8:],[3,409,True,True])
        self.assertEqual(trace['ordered_plan_map_values'],[100,102,104])
        self.assertEqual(trace['successful_plan_dispatch_ends'],2)

    def test_requested_workers_are_maximum_and_initializer_counts_stay_correlated(self):
        for lanes,init_sum,allowed in ((1,0,set(range(406,407))),(2,1,set(range(406,411))),(3,3,set(range(406,415)))):
            traces=[model.api_trace(lanes,assignment) for assignment in itertools.product(range(lanes),repeat=4)]
            self.assertEqual({trace['init_sum'] for trace in traces},{init_sum})
            self.assertEqual({trace['plan_sum'] for trace in traces},allowed)
            self.assertEqual(traces[0]['initialized_logical_lanes'],list(range(lanes)))
        for requested,actual in ((2,0),(2,3),(3,4),(3,True),(-1,1)):
            with self.assertRaises(ValueError):model.LogicalPlan(requested,actual,lambda lane:lane)

    def test_all2316_complete_reduce_and_map_assignments_preserve_both_boolean_observations(self):
        seen=set();count=0
        for lanes in (1,2,3):
            for reduce_lanes in itertools.product(range(lanes),repeat=4):
                for map_lanes in itertools.product(range(lanes),repeat=3):
                    trace=model.api_trace(lanes,reduce_lanes,map_lanes);count+=1
                    self.assertEqual(trace['values'][-2:],[True,True]);seen.add(model.render(trace['values']))
        self.assertEqual(count,2316)
        self.assertEqual(seen,set(model.output_options('parallel_api_reference')))
        self.assertEqual(len(seen),15)

    def test_close_rejects_dispatch_before_callback_or_second_end_and_is_idempotent(self):
        seen=set()
        for lanes in (1,2):
            for assignment in itertools.product(range(lanes),repeat=3):
                trace=model.close_trace(lanes,assignment);seen.add(model.render(trace['values']))
                self.assertEqual(trace['values'][1:],['closed','done'])
                self.assertEqual(trace['postclose_callback_values'],[])
                self.assertEqual((trace['successful_dispatch_ends'],trace['logical_state_clear_count']),(1,1))
        self.assertEqual(seen,{str(value)+'\nclosed\ndone\n' for value in range(6,10)})
        self.assertEqual(model.close_trace(2)['values'],[7,'closed','done'])
        plan=model.LogicalPlan(2,1,lambda lane:lane);plan.begin()
        with self.assertRaises(ValueError):plan.close()
        plan.end();plan.close()
        with self.assertRaises(model.ClosedPlan):plan.for_each((0,),lambda item:self.fail('Closed callback ran'))
        self.assertEqual(plan.ends,1)

    def test_impossible_cartesian_init_and_reduce_pairs_cannot_pass(self):
        row=next(row for row in model.expected_rows() if row['source'].endswith('parallel_api_reference.xr'))
        for init_sum,total in ((0,407),(0,414),(1,411),(1,414),(2,409),(3,405),(3,415)):
            values=model.api_trace(3)['values'];values[8:10]=[init_sum,total]
            self.assertFalse(model.matches(row,self.result(model.render(values))))

    def test_all_complete_output_slots_exit_stderr_and_extra_lines_remain_required(self):
        for row in model.expected_rows():
            for text in row['expected']['stdout_utf8_options']:
                self.assertTrue(model.matches(row,self.result(text)))
                lines=text.splitlines()
                for index in range(len(lines)):
                    changed=list(lines);changed[index]+='wrong'
                    self.assertFalse(model.matches(row,self.result('\n'.join(changed)+'\n')))
                self.assertFalse(model.matches(row,self.result(text+'extra\n')))
                self.assertFalse(model.matches(row,self.result('\n'.join(lines[:-1])+'\n')))
            for attr,value in (('returncode',1),('timed_out',True),('stderr',b'error')):
                result=self.result(row['expected']['stdout_utf8']);setattr(result,attr,value)
                self.assertFalse(model.matches(row,result))
            self.assertFalse(model.matches(row,None))

    def test_complete_originals_frozen_contract_excerpt_and_all_authorities_cannot_drift(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            def changed(path):return saved(path)+b'changed' if path==provenance_input_path(model.ROOT, name) else saved(path)
            with self.subTest(name=name),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
        with mock.patch.object(model,'_INPUTS',{}),self.assertRaises(ValueError):model.expected_rows()
        text=(model.ROOT/model.PROGRAMS['parallel_api_reference']).read_text(encoding='utf-8')
        for token in ('[99]','Options(0)','Options(1)','Options(2)','Options(3)','initSum.add(lane','plan.close()'):
            self.assertIn(token,text)
        close=(model.ROOT/model.PROGRAMS['parallel_plan_close_lifecycle']).read_text(encoding='utf-8')
        self.assertEqual(close.count('plan.close()'),2);self.assertEqual(close.count('print('),6)

    def test_manifest_and_owner_reject_foreign_or_shrunk_expanded_and_uncoupled_domains(self):
        bound=json.loads((Path(__file__).parent/'parallel_api_driver_cases.json').read_text(encoding='utf-8'));driver.validate_cases(bound)
        for key,value in (('source','other.xr'),('arguments',['x']),('stdin_hex','31'),('companions',[{'path':'x'}]),('stdout_utf8','wrong\n')):
            changed=copy.deepcopy(bound);changed['cases'][0][key]=value
            with self.assertRaises(ValueError):driver.validate_cases(changed)
        for row in model.expected_rows():
            for key,value in (('id','complete_task_status_1116'),('source','other.xr'),('original_source','other.xr'),('observation_count',0)):
                changed=copy.deepcopy(row);changed[key]=value
                with self.assertRaises(ValueError):model.expected_outputs(changed)
            options=row['expected']['stdout_utf8_options']
            for values in (options[:-1],options+[options[0]],options+['extra\n'],list(reversed(options))):
                changed=copy.deepcopy(row);changed['expected']['stdout_utf8_options']=values
                with self.assertRaises(ValueError):model.expected_outputs(changed)

    def test_full741_selector_checks_each_backend_legal_domain_without_requiring_equal_lanes(self):
        selected={'rejection_oracle_required':False,'canonical_port_selected':False}
        for row in model.expected_rows():
            options=row['expected']['stdout_utf8_options']
            self.assertEqual(selection.observation_status(selected,row,self.result(options[0]),self.result(options[-1]),True),'PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE')
            self.assertEqual(selection.observation_status(selected,row,self.result(options[0]),self.result(options[-1]),False),'FAIL_INDEPENDENT_SELECTED_CONSUMER')

    def test_coordination_delegates_both_whole_programs_without_granting_runtime_or_identity_proofs(self):
        import complete_coordination_model as coordination
        rows=[row for row in coordination.expected_rows() if row['id'] in model.IDENTITIES]
        self.assertEqual(rows,model.expected_rows());self.assertEqual(sum(row['observation_count'] for row in rows),15)
        self.assertFalse(model.close_trace(1)['physical_release_or_actual_parallelism_qualified'])
        trace=model.api_trace(3)
        self.assertTrue(trace['printed_init_sum_does_not_prove_each_runtime_initializer_identity_or_count'])
        self.assertFalse(trace['actual_lane_start_parallelism_resources_lifetime_FI_safety_provider_mixed_qualified'])


if __name__=='__main__':unittest.main()
