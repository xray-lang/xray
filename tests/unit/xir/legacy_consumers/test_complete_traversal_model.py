"""Preserve complete traversal workloads and reject plausible wrong answers."""
from pathlib import Path
import json
import tempfile
import unittest
from unittest.mock import patch
import complete_traversal_model as m


class CompleteTraversalControls(unittest.TestCase):
    def test_seven_complete_originals_and124_observations(self):
        rows=m.expected_rows()
        self.assertEqual(7,len(rows))
        self.assertEqual([14,23,2,27,4,43,11],[x['observation_count'] for x in rows])
        self.assertEqual(124,sum(x['observation_count'] for x in rows))
        self.assertTrue(all(x['expected']['exit_code']==0 and x['expected']['stderr_utf8']=='' for x in rows))

    def test_inclusive_complete_outputs(self):
        self.assertEqual(['1..=3',3,True,False,1,3,[1,2,3],6,3,m.I64_MAX,1,'low','mid','other'],m.inclusive_trace()['outputs'])

    def test_full_i64_count_is_saturated_without_wrapping(self):
        r=m.Range(m.I64_MIN,m.I64_MAX,True)
        self.assertEqual(1<<64,r.raw_count())
        self.assertEqual(m.I64_MAX,r.count())
        self.assertEqual([m.I64_MAX],m.Range(m.I64_MAX,m.I64_MAX,True).to_array())
        self.assertEqual(6,m.inclusive_trace()['whole_original_loop_body_steps'])

    def test_direction_zero_step_and_empty(self):
        for r in (m.Range(5,5),m.Range(9,2),m.Range(0,9,step=0)):
            self.assertEqual([],r.to_array())
            self.assertFalse(r.contains(0))
        self.assertEqual([7],m.Range(7,7,True).to_array())

    def test_negative_step_minimum_and_membership(self):
        self.assertEqual([0,m.I64_MIN],m.Range(0,m.I64_MIN,True,m.I64_MIN).to_array())
        r=m.Range(5,-2,False,-2)
        self.assertEqual([5,3,1,-1],r.to_array())
        self.assertTrue(r.contains(-1));self.assertFalse(r.contains(-2));self.assertFalse(r.contains(2))

    def test_half_open_match_end_is_excluded(self):
        self.assertTrue(m.Range(1,3,True).contains(3))
        self.assertTrue(m.Range(4,6).contains(5))
        self.assertFalse(m.Range(4,6).contains(6))
        with self.assertRaises(m.Fault):m.Range(1,3).at(2)

    def test_oversized_original_keeps10000001_and_allocates_no_elements(self):
        with patch.object(m.Range,'at',side_effect=AssertionError('must reject before materializing')):
            trace=m.oversized_trace()
        self.assertEqual(['Range.toArray range too large',441],trace['outputs'])
        self.assertEqual(10000001,trace['original_length'])
        self.assertEqual(10000000,trace['materialization_limit'])
        self.assertEqual(0,trace['materialized_elements'])
        self.assertEqual(10000000,m.Range(0,10000000).count())

    def test_range_cursor_complete23_outputs(self):
        self.assertEqual([[2,3,4,5,6,7,8],[1,2,3,4],[],[],[-3,-2,-1,0,1],[-4,-3,-2],[7],
                          [3,4,5,6,7,8],[3,4,5,6,7,8],33,33,True,3,4,3,4,False,0,35,10,0,-5,43],m.range_cursor_trace()['outputs'])
        self.assertEqual(61,m.range_cursor_trace()['whole_original_loop_body_steps'])

    def test_new_range_cursor_is_independent_but_iterator_returns_self(self):
        r=m.Range(3,8,True);a,b=r.iterator(),r.iterator()
        self.assertIsNot(a,b)
        self.assertEqual((3,4,3),(a.next(),a.next(),b.next()))
        c=b.iterator();self.assertIs(c,b)
        self.assertEqual(4,c.next());self.assertEqual(5,b.next());self.assertEqual(5,a.next())

    def test_nth_consumes_prefix_and_preserves_exact_failure_kind(self):
        c=m.Cursor([10,20,30]);self.assertEqual(30,c.nth(2));self.assertFalse(c.has_next())
        with self.assertRaises(m.Fault) as failure:c.next()
        self.assertEqual((432,m.NEXT_MESSAGE),(failure.exception.code,str(failure.exception)))
        c=m.Cursor([10,20,30])
        with self.assertRaises(m.Fault) as failure:c.nth(3)
        self.assertEqual((432,m.NTH_MESSAGE),(failure.exception.code,str(failure.exception)))
        self.assertEqual(3,c.position)

    def test_negative_nth_rejects_before_consuming(self):
        c=m.Cursor([4,5])
        with self.assertRaises(m.Fault) as failure:c.nth(-1)
        self.assertEqual(430,failure.exception.code);self.assertEqual(0,c.position)

    def test_all_nine_original_exhaustion_faults_remain_caught(self):
        trace=m.exhausted_trace()
        self.assertEqual([432]*9,trace['fault_codes']);self.assertEqual(9,trace['caught_faults'])
        self.assertEqual(0,trace['unreachable_prints_executed']);self.assertTrue(trace['empty_stays_exhausted'])
        self.assertEqual(['arr 10','arr 20','arr 30','arr-has false','arr-next 432 '+m.NEXT_MESSAGE,
          'arr-entry 0=10','arr-entry 1=20','arr-entry 2=30','arr-entry-next 432','arr-nth 30','arr-nth 432 '+m.NTH_MESSAGE,
          'map k','map j','map-next 432','map-entry k=1','map-entry j=2','map-entry-next 432','set 7','set 8','set-next 432',
          'rune a','rune b','rune-next 432 '+m.NEXT_MESSAGE,'rune-nth b','rune-nth 432','empty-has false','empty-next 432'],trace['outputs'])

    def test_unicode_iterator_counts_runes_not_utf8_bytes(self):
        c=m.Cursor(list('🙂x'));self.assertEqual(['🙂','x'],m.drain(c))
        with self.assertRaises(m.Fault):c.next()
        self.assertFalse(c.has_next())

    def test_updates_keep_position(self):
        table=m.Ordered();table.put('z',1);table.put('a',2);table.put('z',9)
        self.assertEqual([('z',9),('a',2)],table.items())
        self.assertEqual(['insert','insert','update'],table.operations)

    def test_delete_reinsert_and_tombstone_append_preserve_order(self):
        table=m.Ordered()
        for key in ('a','b','c'):table.put(key,key)
        table.delete('b');table.put('b','B');self.assertEqual(['a','c','b'],table.keys())
        table.delete('a');table.delete('c');table.put('d','D');self.assertEqual(['b','d'],table.keys())
        self.assertFalse(table.delete('missing'))

    def test_complete43_ordered_projection_outputs(self):
        self.assertEqual(['-- map for k --','first','second','third','-- map for k,v --','first=10','second=2','third=3',
          '-- map keys --','first','second','third','-- map values --','10','2','3','-- map delete+reinsert --','first','third','second',
          '-- map tombstone append --','second=20','fourth=4','fifth=5','20','4','5','-- set for e --','x','y','z',
          '-- set delete+reinsert --','y','z','x','-- set tombstone append --','z','x','w','-- int set --','30','10','20'],m.order_trace()['outputs'])
        self.assertEqual(33,m.order_trace()['iteration_body_steps'])

    def test_full_derived_key_includes_enum_identity_number_and_label(self):
        base=m.Key(('module::Color',0),1,'a')
        self.assertTrue(m.key_equal(base,m.Key(('module::Color',0),1,'a')))
        for other in (m.Key(('other::Color',0),1,'a'),m.Key(('module::Color',1),1,'a'),
                      m.Key(('module::Color',0),2,'a'),m.Key(('module::Color',0),1,'z'),
                      m.Key(('module::Color',0),1,'a','other::Key')):
            self.assertFalse(m.key_equal(base,other))
        self.assertFalse(m.key_equal(True,1))

    def test_complete_derived11_outputs_and_countdown4(self):
        self.assertEqual([2,11,True,False,False,None,False,1,2,True,False],m.derived_trace()['outputs'])
        self.assertEqual([4,3,2,1],m.countdown_trace()['outputs'])
        self.assertTrue(m.countdown_trace()['legacy_interface_and_iterator_declarations_require_atomic_migration'])
        self.assertFalse(m.derived_trace()['actual_derive_or_hash_implementation_qualified'])

    def test_whole_source_tamper_is_rejected(self):
        name=next(iter(m._PROGRAMS.values()));original=m.ROOT
        with tempfile.TemporaryDirectory() as temporary:
            changed=Path(temporary)/'changed.xr';changed.write_bytes((original/name).read_bytes()+b'\nprint(0)\n')
            class SourceView:
                def __truediv__(self, relative):return changed if relative==name else original/relative
            with patch.object(m,'ROOT',SourceView()), self.assertRaisesRegex(ValueError,'source or authority changed'):
                m.expected_rows()

    def test_all_seven_models_are_registered_with_original_hashes(self):
        manifest=json.loads((m.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_text())['cases']
        selected={x['id']:x for x in manifest}
        for row in m.expected_rows():
            self.assertEqual(row['source'],selected[row['id']]['source'])
            self.assertEqual(m._INPUTS[row['source']],selected[row['id']]['source_lf_sha256'])
            self.assertEqual(row['expected']['stdout_utf8'],selected[row['id']]['stdout_utf8'])


if __name__=='__main__':unittest.main()
