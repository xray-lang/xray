"""Complete original outputs and independent byte/POD boundary controls."""
import unittest
from unittest.mock import patch
import complete_byte_pod_model as model


class CompleteBytePod(unittest.TestCase):
    def test_every_original_observation(self):
        expected=[
          [3,'[66, 67, 68]',9,'[66, 67, 68, 66, 67, 68, 66, 67, 68]',4,'[88, 89, 88, 89]',430,True],
          [4,1,255,0,128,1,0,1,3,254,4,1,1,0,2,1,1],['before-range'],
          [1,1,7,8,9,7,1,1,4,0,-1,1,-1,1,4,3,0,0,65,66,65,66,65,66,65,66],
          [0,0,192,63,192,32,0,0,2,192,1.5,-2.5,-2.25],
          [52,18,18,52,1,4,4660,4660,16909060,2569],
          [254,255,255,253,-2,-3,-12345678,-16909060,-1,65534],
          [8,513,1,2,7,7,7,7],[0,-1,1,1,-1,-1],
          [0,17,42,0,2,1,1,2,3,'Slice.copyFrom(src) range out of bounds',430],
          [2,0,9,9,0,True,True,True,'zzz',0],
          [8,16843009,16843009,3,119,119,6,2313,2313,
            'Slice<u8>.reinterpret<T>() length is not divisible by target size',430,
            'Slice<u8>.reinterpret<T>() source address is not aligned for target type',404]]
        traces=model.observations()
        self.assertEqual(sum(map(len,traces.values())),133)
        for name,values in zip(model._NAMES,expected):
            with self.subTest(name=name):self.assertEqual(traces[name],values)

    def test_array_format_is_container_text(self):
        values=model.observations()[model._NAMES[0]]
        self.assertEqual(values[1],'[66, 67, 68]');self.assertNotEqual(values[1],'BCD')

    def test_complete_constructor_coercion_preserves_original_values(self):
        self.assertEqual([model.byte(v) for v in [257,-1,128,True,False,3.8,-2.2,260.0,'x']],[1,255,128,1,0,3,254,4,0])

    def test_append_self_alias_uses_full_snapshot(self):
        data=model.Bytes();data.append(b'abc');data.append(data.view(1,2).raw())
        self.assertEqual(data.data,b'abcbc')

    def test_growing_repeat_reads_newly_written_bytes(self):
        data=model.Bytes();data.append(b'ab');data.repeat(2,7)
        self.assertEqual(data.data,b'ababababa')

    def test_repeat_fault_does_not_mutate(self):
        for distance,count in [(0,1),(4,1),(1,-1)]:
            data=model.Bytes();data.append(b'abc')
            with self.assertRaises(model.Fault):data.repeat(distance,count)
            self.assertEqual(data.data,b'abc')

    def test_signed_and_unsigned_loads_differ(self):
        view=model.Bytes(2).view();view.store(0,2,-2)
        self.assertEqual(view.load(0,2),65534);self.assertEqual(view.load(0,2,signed=True),-2)

    def test_ieee_float_bytes_and_explicit_endian(self):
        v=model.Bytes(8).view();v.store(0,4,1.5,floating=True);v.store(4,4,-2.5,'big',floating=True)
        self.assertEqual(v.raw(),bytes.fromhex('0000c03fc0200000'))
        self.assertEqual(v.load(4,4,'big',floating=True),-2.5)

    def test_huge_offset_is_not_truncated_to_u32(self):
        v=model.Bytes(8,1).view()
        self.assertEqual(v.load(0,4),16843009)
        with self.assertRaises(model.Fault):v.load(4294967296,4)

    def test_overlapping_copy_has_memmove_snapshot_semantics(self):
        v=model.pod([1,2,3,4],2);v.window(1,4).copy_from(v.window(0,3))
        self.assertEqual([v.get(i) for i in range(4)],[1,1,2,3])

    def test_copy_range_failure_preserves_destination(self):
        v=model.pod([5],4);source=model.pod([7,8],4)
        with self.assertRaises(model.Fault):v.copy_from(source)
        self.assertEqual(v.get(0),5)

    def test_compare_is_unsigned_byte_order(self):
        a,b=model.pod([256],2),model.pod([1],2)
        self.assertEqual(a.compare(b),-1)
        self.assertEqual(a.window(0,0).compare(b),-1)

    def test_as_bytes_and_reinterpret_share_one_region(self):
        v=model.pod([0,0],4);raw=v.as_bytes();raw.fill(1)
        self.assertIs(raw.owner,v.owner);self.assertEqual(v.get(1),0x01010101)
        pairs=raw.reinterpret(2);pairs.set(1,0x7777)
        self.assertEqual(raw.raw()[2:4],b'ww')

    def test_size_and_alignment_are_distinct_faults(self):
        raw=model.Bytes(6).view()
        first=model.caught(lambda:raw.window(0,5).reinterpret(4))
        second=model.caught(lambda:raw.window(1,5).reinterpret(4))
        self.assertEqual([first[1],second[1]],[430,404])

    def test_shared_view_cannot_write(self):
        v=model.Bytes(4).view(mutable=False)
        with self.assertRaises(ValueError):v.fill(1)

    def test_whole_source_or_authority_tamper_is_rejected(self):
        changed=dict(model._INPUTS);changed[model._PROGRAMS[model._NAMES[0]]]='0'*64
        with patch.object(model,'_INPUTS',changed):
            with self.assertRaisesRegex(ValueError,'original or authority'):model.expected_rows()

    def test_registered_rows_keep_fault_and_no_unreachable_output(self):
        rows=model.expected_rows();self.assertEqual(len(rows),12)
        row=rows[2]
        self.assertEqual(row['expected']['exit_code'],1)
        self.assertEqual(row['expected']['stdout_utf8'],'before-range\n')
        self.assertNotIn('unreachable',row['expected']['stdout_utf8'])
        self.assertTrue(all(x['required_target']=='windows-x86_64' for x in rows))


if __name__=='__main__':unittest.main()
