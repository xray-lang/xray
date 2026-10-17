"""Negative and complete-domain controls for unchanged network originals."""
import copy,json
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
import unittest

import complete_network_model as m
import complete_target_selection as selection
import run_network_driver as driver


class CompleteNetworkControls(unittest.TestCase):
    def rows(self):
        return {row['id']:row for row in m.expected_rows()}

    def transfer(self):
        return self.rows()['complete_network_net_copy_bidirectional_transfer']

    def result(self,stdout,exit_code=0,stderr=b'',timeout=False):
        return SimpleNamespace(stdout=stdout,returncode=exit_code,stderr=stderr,timed_out=timeout)

    def test_all_complete_observations(self):
        rows=self.rows()
        self.assertEqual(len(rows),3)
        self.assertEqual(sum(row['observation_count'] for row in rows.values()),12)
        self.assertEqual(rows['complete_network_net_copy_bidirectional_error_enum']['expected']['stdout_utf8'],'Closed\ntrue\n')
        self.assertEqual(rows['complete_network_net_copy_bidirectional_cancel']['expected']['stdout_utf8'],'cancelled\nclosed\n')
        self.assertEqual(self.transfer()['expected']['stdout_utf8'],'4\n5\n5\n114\n4\n108\n4\n5\n')

    def test_all_twenty_read_prefix_pairs_are_complete_outputs(self):
        outputs=m.expected_outputs(self.transfer())
        self.assertEqual(len(outputs),20)
        self.assertEqual(len(set(outputs)),20)
        actual=set()
        for value in outputs:
            lines=value.decode().splitlines()
            self.assertEqual(len(lines),8)
            self.assertEqual([lines[i] for i in (0,1,3,5,6,7)],['4','5','114','108','4','5'])
            actual.add((int(lines[2]),int(lines[4])))
        self.assertEqual(actual,{(r,l) for r in range(1,6) for l in range(1,5)})

    def test_every_pump_partition_conserves_all_bytes_and_direction(self):
        left=m.partitions(4)
        right=m.partitions(5)
        self.assertEqual((len(left),len(right)),(8,16))
        for a in left:
            for b in right:
                first=m.relay(m.LEFT,a)
                second=m.relay(m.RIGHT,b)
                self.assertEqual((first['bytes'],second['bytes']),(tuple(b'left'),tuple(b'right')))
                self.assertEqual((first['count'],second['count']),(4,5))
                self.assertTrue(first['peer_eof'] and second['peer_eof'])

    def test_missing_half_close_cannot_settle_and_swapped_totals_fail(self):
        with self.assertRaises(ValueError):
            m.transfer_values(5,4,half_close=False)
        row=self.transfer()
        for r in range(1,6):
            for l in range(1,5):
                wrong=m.text(m.transfer_values(r,l,swap_counts=True)).encode()
                self.assertFalse(m.matches(row,self.result(wrong)))
        for chunks in ((1,1),(0,4),(5,)):
            with self.assertRaises(ValueError):
                m.relay(m.LEFT,chunks)

    def test_closed_guard_precedes_equal_invalid_descriptors(self):
        a,b=m.Endpoint('a'),m.Endpoint('b')
        self.assertEqual(m.closed_guard(a,b),'pump')
        a.close()
        b.close()
        self.assertEqual((a.descriptor(),b.descriptor()),(-1,-1))
        self.assertEqual(m.closed_guard(a,b),'Closed')
        self.assertEqual(m.closed_guard(a,b,identity_first=True),'Invalid')
        self.assertEqual(m.closed_guard(m.Endpoint('same'),m.Endpoint('same')),'Invalid')

    def test_cancel_result_and_process_settlement_are_separate_duties(self):
        row=self.rows()['complete_network_net_copy_bidirectional_cancel']
        trace=m.cancel_trace()
        self.assertEqual(trace['task_result'],'Cancelled')
        self.assertEqual(trace['parent_close_calls'],['left','right','listener'])
        self.assertTrue(trace['terminates'])
        self.assertFalse(trace['accepted_handle_runtime_cleanup_qualified'])
        self.assertFalse(m.matches(row,self.result(m.text(m.cancel_trace(publish_cancelled=False)['values']).encode())))
        self.assertFalse(m.cancel_trace(settles=False)['terminates'])
        self.assertFalse(m.matches(row,self.result(b'cancelled\nclosed\n',timeout=True)))

    def test_every_observation_slot_truncation_and_suffix_are_checked(self):
        row=self.transfer()
        for output in m.expected_outputs(row):
            lines=output.decode().splitlines()
            for i in range(8):
                mutated=lines[:]
                mutated[i]='999'
                self.assertFalse(m.matches(row,self.result(('\n'.join(mutated)+'\n').encode())))
            self.assertFalse(m.matches(row,self.result(output.rsplit(b'\n',2)[0]+b'\n')))
            self.assertFalse(m.matches(row,self.result(output+b'extra\n')))
        for r,l in ((0,1),(6,1),(1,0),(1,5),(True,1)):
            with self.assertRaises(ValueError):
                m.transfer_values(r,l)

    def test_finite_output_domain_cannot_shrink_widen_or_borrow_identity(self):
        original=self.transfer()
        changes=[('source','unrelated.xr'),('original_source','other.xr'),('id','unowned')]
        for key,value in changes:
            row=copy.deepcopy(original)
            row[key]=value
            with self.assertRaises(ValueError):
                m.expected_outputs(row)
        for options in ([],original['expected']['stdout_utf8_options'][-1:],
                        original['expected']['stdout_utf8_options']+['bogus\n']):
            row=copy.deepcopy(original)
            row['expected']['stdout_utf8_options']=options
            with self.assertRaises(ValueError):
                m.expected_outputs(row)
        row=copy.deepcopy(original)
        row['expected']['stdout_utf8']='unrelated\n'
        with self.assertRaises(ValueError):
            m.expected_outputs(row)

    def test_complete_original_route_accepts_different_legal_prefixes(self):
        row=self.transfer()
        outputs=m.expected_outputs(row)
        chosen={'rejection_oracle_required':False,'canonical_port_selected':False}
        first,last=self.result(outputs[0]),self.result(outputs[-1])
        self.assertNotEqual(first.stdout,last.stdout)
        self.assertEqual(selection.observation_status(chosen,row,first,last,True),'PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE')
        self.assertEqual(selection.observation_status(chosen,row,first,last,False),'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        for bad in (self.result(outputs[0],exit_code=1),self.result(outputs[0],stderr=b'error'),
                    self.result(outputs[0],timeout=True),self.result(b'wrong\n')):
            self.assertEqual(selection.observation_status(chosen,row,bad,last,True),'FAIL_INDEPENDENT_SELECTED_CONSUMER')

    def test_competing_oracles_cannot_hide_a_changed_complete_domain(self):
        row=self.transfer()
        selected={'case_id':row['original_source'],'selected_source':row['source'],
                  'selected_source_lf_sha256':m._INPUTS[row['source']],
                  'canonical_port_selected':False,'rejection_oracle_required':False}
        changed=copy.deepcopy(row)
        changed['expected']['stdout_utf8_options']=changed['expected']['stdout_utf8_options'][-1:]
        with self.assertRaises(ValueError):
            selection.expected_for(selected,[],[row,changed])
        self.assertEqual(selection.expected_for(selected,[],[row]),row)

    def test_original_sources_and_every_authority_are_pinned(self):
        original=Path.read_bytes
        for name in m._INPUTS:
            def changed(path):
                raw=original(path)
                return raw+b'\nchanged\n' if path==m.ROOT/name else raw
            with self.subTest(path=name),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):
                m.expected_rows()
        with mock.patch.dict(m._INPUTS,{m.PROGRAMS['net_copy_bidirectional_transfer']:'0'*64}),self.assertRaises(ValueError):
            m.expected_rows()

    def test_dedicated_manifest_rejects_lost_source_context_or_program(self):
        bound=json.loads((Path(__file__).parent/'network_driver_cases.json').read_text(encoding='utf-8'))
        driver.validate_cases(bound)
        self.assertEqual(len(bound['cases']),3)
        mutations={'source':'shortened.xr','source_lf_sha256':'0'*64,'oracle_id':'other',
                   'entry':'function','source_context':'materialized_subset',
                   'arguments':['changed'],'stdin_hex':'00','companions':[{'path':'unexpected.xr'}]}
        for key,value in mutations.items():
            changed=copy.deepcopy(bound)
            changed['cases'][0][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                driver.validate_cases(changed)
        for cases in (bound['cases'][:-1],bound['cases']+[bound['cases'][0]]):
            with self.assertRaises(ValueError):
                driver.validate_cases(dict(bound,cases=cases))


if __name__=='__main__':
    unittest.main()
