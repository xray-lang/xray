"""Control independent full memory traces without running Xray or host C code."""
from pathlib import Path
import unittest
from unittest import mock
import complete_memory_primitives_model as m


class CompleteMemoryPrimitives(unittest.TestCase):
    def test_every_original_complete_trace(self):
        expected={
            'mem_bulk_shared_core':[64,7,7,7,7,0,True,0,5,6,'ok'],
            'mem_from_address_shared_core':[True,0,False,42,99,99,True,16909060,True,True,True,0,0,0,305419896,True,'ok'],
            'mem_layout_introspection':[1,1,2,2,4,4,8,8,4,4,8,8,8,8,6,2,12,4,0,2,4,8,7,1,0,1,5,4,2,20,4,0,12,16],
            'mem_load_store_scalar_shared_core':[-2,250,-1234,4660,-12345678,16909060,-5,72623859790382856,1.5,-2.25,True,True,270544960,270544960],
            'mem_volatile_shared_core':[305419896,120,255,43981,72623859790382856,'ok']}
        self.assertEqual({name:m.observations()[name] for name in expected},expected)
        self.assertEqual(sum(map(len,expected.values())),82)
        self.assertEqual(len([row for row in m.expected_rows() if row['id'] not in {m._UNALIGNED_ID,m._ALLOCATION_ID} and row['original_source'] not in m._LAYOUT_PROGRAMS.values()]),5)

    def test_uninitialized_and_dead_bytes_never_supply_observations(self):
        memory=m.Region(4)
        with self.assertRaises(ValueError):memory.read(0,1)
        memory.write(1,b'\x09');self.assertEqual(memory.read(1,1),b'\x09')
        with self.assertRaises(ValueError):memory.read(0,2)
        memory.freed=True
        with self.assertRaises(ValueError):memory.read(1,1)
        with self.assertRaises(ValueError):memory.write(1,b'\x0a')

    def test_explicit_integer_bytes_signedness_endianness_and_unaligned_offsets(self):
        memory=m.Region(12,zeroed=True)
        memory.store_int(1,2,-2,signed=True,endian='big')
        self.assertEqual(memory.read(0,4),bytes.fromhex('00fffe00'))
        self.assertEqual(memory.load_int(1,2,signed=True,endian='big'),-2)
        self.assertEqual(memory.load_int(1,2,endian='big'),65534)
        memory.store_int(3,4,0x01020304,endian='little')
        self.assertEqual(memory.read(3,4),bytes.fromhex('04030201'))
        before=memory.read(0,12)
        with self.assertRaises(ValueError):memory.store_int(11,2,1)
        self.assertEqual(memory.read(0,12),before)

    def test_float_bits_are_independent_ieee_known_answers(self):
        memory=m.Region(16,zeroed=True)
        memory.store_float(0,4,1.5,'little');memory.store_float(4,4,-2.5,'big');memory.store_float(8,8,-2.25,'big')
        self.assertEqual(memory.read(0,16).hex(),'0000c03fc0200000c002000000000000')
        self.assertEqual(memory.load_float(4,4,'big'),-2.5)

    def test_copy_policy_distinguishes_overlap_and_preserves_all_bytes(self):
        memory=m.Region(6);memory.write(0,bytes([1,2,3,4,5,6]));p=m.Pointer(m.Address(memory),mutable=True)
        with self.assertRaises(ValueError):m.bulk_copy(p.offset(1),p,5)
        self.assertEqual(memory.read(0,6),bytes([1,2,3,4,5,6]))
        m.bulk_copy(p.offset(1),p,5,overlap=True)
        self.assertEqual(memory.read(0,6),bytes([1,1,2,3,4,5]))
        m.bulk_copy(p,p.offset(1),5,overlap=True)
        self.assertEqual(memory.read(0,6),bytes([1,2,3,4,5,5]))

    def test_empty_bulk_operations_do_not_inspect_null_pointers(self):
        for count in [0,-1,-2**63]:
            m.bulk_copy(None,None,count);m.bulk_set(None,255,count)
            self.assertEqual(m.bulk_compare(None,None,count),0)
        for operation in [lambda:m.bulk_copy(None,None,1),lambda:m.bulk_set(None,0,1),lambda:m.bulk_compare(None,None,1)]:
            with self.assertRaises(ValueError):operation()

    def test_symbolic_typed_addresses_keep_identity_and_access_permission(self):
        first=m.Region(4096,page_aligned=True);second=m.Region(4096,page_aligned=True)
        address=m.Address(first);writable=m.Pointer(address,'u32',True);readonly=m.Pointer(address,'u8')
        writable.write(bytes([1,2,3,4]));self.assertEqual(readonly.read(4),bytes([1,2,3,4]))
        self.assertEqual(m.Pointer(address,'f64').address,address)
        self.assertNotEqual(address,m.Address(second));self.assertEqual(address.add(15).alignment_remainder(4096),15)
        slots=m.PointerSlots();memory=m.Region(24,zeroed=True)
        slots.store(memory,0,writable);slots.store(memory,8,m.Pointer(m.Address(second,17)))
        self.assertEqual(len(memory.read(0,16)),16)
        self.assertEqual(slots.load(memory,0,mutable=True).address,address)
        self.assertEqual(slots.load(memory,8).address,m.Address(second,17))
        self.assertTrue(slots.load(memory,16).is_null())
        memory.store_int(16,8,1)
        with self.assertRaises(ValueError):slots.load(memory,16)
        with self.assertRaises(ValueError):readonly.write(b'\x00')
        with self.assertRaises(ValueError):m.Pointer(m.Address(None)).read(1)

    def test_volatile_width_and_invalid_width_policy_do_not_claim_ordering(self):
        memory=m.Region(8,zeroed=True);p=m.Pointer(m.Address(memory),mutable=True)
        m.volatile_store(p,-1,8);self.assertEqual(m.volatile_load(p,8),-1)
        self.assertEqual(m.volatile_load(p,1),255)
        before=memory.read(0,8);m.volatile_store(None,7,3)
        self.assertEqual(m.volatile_load(None,3),0);self.assertEqual(memory.read(0,8),before)

    def test_every_source_and_authority_is_pinned(self):
        saved=Path.read_bytes
        for name in m._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()

    def test_complete_program_map_and_target_facts_cannot_be_swapped_or_erased(self):
        swapped=dict(m._PROGRAMS);names=sorted(swapped);swapped[names[0]],swapped[names[1]]=swapped[names[1]],swapped[names[0]]
        for attribute,value in [('_PROGRAMS',swapped),('_PROGRAMS',{}),('_INPUTS',{}),('NATIVE_ENDIAN','big'),('POINTER_BYTES',4),('CACHE_LINE_BYTES',128)]:
            with self.subTest(attribute=attribute),mock.patch.object(m,attribute,value),self.assertRaises(ValueError):m.expected_rows()



    def test_five_complete_layout_and_numeric_traces(self):
        expected={'repr_c_struct':[12,20,False,4,2,3],
         'repr_packed_aligned_struct_aggregate_param':[42,42,42,5,1,1,16,16,16,16,1],
         'union_fixed_array_overlay':[4,3,2,1,16909060],'union_fixed_layout_overlay':[4294967295,2147483647],
         'fixed_width_type_tests':['7 is i32=true is i8=true is u8=true','7 as i32=7','7 as i8?=7',
          '3e9 is i32=false is i64=true','3e9 as i32?=null','-5 is u8=false is i16=true',
          '1.5 is f32=true is f64=true','1.5 as f32?=1.5','0.1 is f32=false is f64=true','0.1 as f32?=null','"s" is i32=false is f32=false']}
        self.assertEqual(sum(map(len,expected.values())),35);self.assertEqual(m.fixed_layout_observations(),expected)
        self.assertEqual(len([row for row in m.expected_rows() if row['id'] not in {m._UNALIGNED_ID,m._ALLOCATION_ID}]),10);self.assertEqual(sum(map(len,m.observations().values())),117)

    def test_packing_and_requested_alignment_do_not_change_member_order(self):
        fields=[(1,1),(4,4)]
        self.assertEqual(m.aligned_aggregate(fields),((8,4),[0,4]))
        self.assertEqual(m.aligned_aggregate(fields,packed=True),((5,1),[0,1]))
        self.assertEqual(m.aligned_aggregate(fields,packed=True,alignment=16),((16,16),[0,1]))
        self.assertEqual(m.aligned_aggregate([(4,4)],alignment=16),((16,16),[0]))
        for alignment in [-1,3,True]:
            with self.subTest(alignment=alignment),self.assertRaises(ValueError):m.aligned_aggregate(fields,alignment=alignment)

    def test_record_values_keep_nominal_identity_and_nested_copy(self):
        point=m.RecordValue('CPoint',{'x':1,'y':2});box=m.RecordValue('CBox',{'p':point,'tag':3});copy=box.copy()
        copy.fields['p'].fields['x']=99
        self.assertEqual(box.fields['p'].fields['x'],1);self.assertEqual(point.fields['x'],1)
        self.assertNotEqual(box,copy);self.assertEqual(point,point);self.assertNotEqual(point,m.RecordValue('CPoint',{'x':1,'y':2}))
        self.assertNotEqual(point,m.RecordValue('OtherPoint',{'x':1,'y':2}))

    def test_every_original_aggregate_function_call_is_preserved(self):
        values,calls=m.aggregate_parameter_trace()
        self.assertEqual(calls,['make_packed','make_aligned','make_packed_aligned','sum_packed','read_aligned','sum_packed_aligned'])
        self.assertEqual(values[:3],[42,42,42]);self.assertEqual(len(values),11)

    def test_union_signedness_and_inactive_access_are_explicit(self):
        value=m.UnionValue({'i':(4,True,False),'u':(4,False,False)},'i',-1)
        self.assertEqual(value.memory.read(0,4),bytes([255]*4))
        with self.assertRaises(ValueError):value.read('u')
        self.assertEqual(value.read('u',unsafe=True),2**32-1)
        value.store('u',0x80000000);self.assertEqual(value.read('i',unsafe=True),-(2**31))
        with self.assertRaises(ValueError):value.store('i',True)

    def test_union_fixed_array_member_read_is_a_logical_copy(self):
        value=m.UnionValue({'u':(4,False,False),'b':(4,False,True)},'u',0x01020304)
        copied=value.read('b',unsafe=True);self.assertEqual(copied,[4,3,2,1]);copied[0]=255
        self.assertEqual(value.read('u'),0x01020304);self.assertEqual(value.read('b',unsafe=True),[4,3,2,1])
        value.store('b',copied);self.assertEqual(value.read('u',unsafe=True),0x010203ff)

    def test_erased_integer_width_checks_use_exact_range_and_family(self):
        for signed in [False,True]:
            for bits in [8,16,32,64]:
                target=('i' if signed else 'u')+str(bits);lo=-(1<<(bits-1)) if signed else 0;hi=(1<<(bits-int(signed)))-1
                for value in [lo,hi]:self.assertTrue(m.exactly_representable(value,target))
                for value in [lo-1,hi+1,True,1.0,'1',None]:self.assertFalse(m.exactly_representable(value,target))
        self.assertFalse(m.exactly_representable(3000000000,'i32'));self.assertTrue(m.exactly_representable(3000000000,'i64'))

    def test_erased_floating_width_checks_do_not_silently_round(self):
        for value in [1.5,-0.0,2.0**100]:self.assertTrue(m.exactly_representable(value,'f32'))
        for value in [0.1,1e100]:self.assertFalse(m.exactly_representable(value,'f32'));self.assertTrue(m.exactly_representable(value,'f64'))
        self.assertFalse(m.exactly_representable(1,'f32'));self.assertFalse(m.exactly_representable('s','f32'))
        self.assertIsNone(m.checked_optional_cast(0.1,'f32'));self.assertEqual(m.checked_optional_cast(1.5,'f32'),1.5)
        self.assertIsNone(m.checked_optional_cast(3000000000,'i32'));self.assertEqual(m.checked_optional_cast(7,'i8'),7)
        with self.assertRaises(ValueError):m.exactly_representable(7,'unknown')
        with self.assertRaises(ValueError):m.exactly_representable(float('nan'),'f64')

    def test_new_bindings_retain_whole_sources_and_target_premises(self):
        rows=[row for row in m.expected_rows() if row['original_source'] in m._LAYOUT_PROGRAMS.values()]
        self.assertEqual(len(rows),5);self.assertEqual(sum(row['observation_count'] for row in rows),35)
        for row in rows:
            self.assertEqual(row['source'],row['original_source']);self.assertEqual(row['required_target'],'windows-x86_64');self.assertEqual(row['expected']['exit_code'],0)


    def test_complete_unaligned_original_has_all_scalar_andbacking_byte_observations(self):
        trace=m.unaligned_memory_trace()
        self.assertEqual(trace['observations'],[4660,16909060,72623859790382856,52,18,4,3,2,1,8,1])
        self.assertEqual(trace['final_array_bytes'],[0,52,18,0,4,3,2,1,8,7,6,5,4,3,2,1])
        self.assertEqual(trace['events'][:6],[('store',1,2,4660,'LE'),('store',4,4,16909060,'LE'),
            ('store',8,8,72623859790382856,'LE'),('load',1,2,'LE'),('load',4,4,'LE'),('load',8,8,'LE')])
        self.assertEqual(trace['events'][6:],[('array_read',x) for x in (1,2,4,5,6,7,8,15)])
        self.assertEqual((trace['original_store_calls'],trace['original_load_calls'],trace['original_byte_observations']),(3,3,8))
        self.assertFalse(trace['physical_borrow_lifetime_FI_safety_orprovider_qualified'])

    def test_explicit_LE_is_host_independent_andbyte_suffix_detects_wrong_encoding(self):
        with mock.patch.object(m,'NATIVE_ENDIAN','big'):
            self.assertEqual(m.unaligned_memory_trace()['observations'],m.unaligned_expected_row()['required_semantic_trace']['observations'])
        class WrongEndian(m.Region):
            def store_int(self,offset,width,value,**kwargs):
                return super().store_int(offset,width,value,endian='big')
            def load_int(self,offset,width,**kwargs):
                return super().load_int(offset,width,endian='big')
        wrong=m.unaligned_memory_trace(WrongEndian)['observations']
        self.assertEqual(wrong[:3],[4660,16909060,72623859790382856])
        self.assertNotEqual(wrong[3:],m.unaligned_memory_trace()['observations'][3:])

    def test_complete_unaligned_source_andauthorities_are_independently_pinned(self):
        saved=Path.read_bytes
        for name in m._UNALIGNED_INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'changed' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_UNALIGNED_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()

    def test_unaligned_product_retains_whole_original_andall_eleven_observations(self):
        import json
        row=m.unaligned_expected_row()
        case=next(x for x in json.loads((m.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_bytes())['cases'] if x['id']==row['id'])
        self.assertEqual((case['source'],case['entry'],case['source_context']),(m._UNALIGNED_SOURCE,'run','original_absolute'))
        self.assertEqual((case['arguments'],case['stdin_hex'],case['companions']),([],'',[]))
        self.assertEqual((row['source'],row['original_source'],row['required_target']),(m._UNALIGNED_SOURCE,m._UNALIGNED_SOURCE,None))
        self.assertEqual((case['expected_exit_code'],case['stderr_utf8'],case['stdout_utf8']),(0,'',row['expected']['stdout_utf8']))
        self.assertEqual(len(case['stdout_utf8'].splitlines()),11)


    def test_complete_managed_buffer_original_all_nineteen_observations(self):
        trace=m.buffer_allocation_trace()
        self.assertEqual(trace['observations'],[16,42,99,16,42,43,43,128,5,5,8,0,0,11,True,64,43,99,'ok'])
        self.assertEqual((trace['original_print_expressions'],trace['original_alloc_calls'],trace['original_resize_calls']),(19,3,1))
        self.assertEqual((trace['original_borrow_pointer_calls'],trace['original_with_slice_mut_calls'],trace['original_asBytes_calls'],trace['original_bulk_set_calls']),(5,1,1,1))
        self.assertEqual([row for row in trace['events'] if row[0] in ('alloc','alloc_aligned','alloc_zeroed','resize')],[('alloc',16),('alloc_aligned',128,64),('alloc_zeroed',8),('resize','p',64,True)])
        self.assertFalse(trace['actual_borrow_lifetime_FI_resource_failure_reclamation_provider_safety_qualified'])

    def test_buffer_views_share_bytes_andlogical_scoped_view_ends(self):
        buffer=m.BufferAllocation(4,zeroed=True);pointer=buffer.borrow_pointer();readonly=buffer.readonly_bytes()
        self.assertEqual(readonly.read(0),0)
        with self.assertRaises(ValueError):readonly.write(0,9)
        readonly.close();escaped=[]
        def update(view):view.write(0,9);escaped.append(view)
        m.buffer_mutable_slice(pointer,4,buffer,update)
        self.assertEqual(pointer.read(1),bytes([9]))
        with self.assertRaises(ValueError):escaped[0].read(0)
        class DetachedWindow(m.BufferWindow):
            def __init__(self,region,count,writable=False,loan=None):
                copied=m.Region(len(region.bytes));copied.bytes[:]=region.bytes;copied.initialized[:]=region.initialized
                super().__init__(copied,count,writable,loan)
        wrong=m.buffer_allocation_trace(DetachedWindow)['observations']
        self.assertEqual(wrong[5],43);self.assertEqual(wrong[6],42);self.assertNotEqual(wrong,m.buffer_allocation_trace()['observations'])

    def test_successful_resize_keeps_initialized_prefix_without_invented_zero_fill_oraddresses(self):
        buffer=m.BufferAllocation(16);pointer=buffer.borrow_pointer()
        pointer.write(bytes([43]));pointer.offset(15).write(bytes([99]))
        self.assertTrue(buffer.resize_success(64));self.assertEqual(len(buffer),64)
        fresh=buffer.borrow_pointer();self.assertEqual((fresh.read(1),fresh.offset(15).read(1)),(bytes([43]),bytes([99])))
        self.assertEqual(buffer.region.initialized[:16],[True]+[False]*14+[True])
        self.assertFalse(any(buffer.region.initialized[16:]))
        with self.assertRaises(ValueError):fresh.offset(16).read(1)
        trace=m.buffer_allocation_trace();self.assertEqual(trace['q_requested_alignment'],64);self.assertTrue(trace['q_all_128_bytes_are5'])
        self.assertEqual(trace['z_final_bytes'],[0,0,0,11,0,0,0,0])
        self.assertFalse(trace['physical_address_orallocation_count_inferred'])

    def test_complete_buffer_source_authorities_andproduct_keep_all_calls(self):
        import json
        saved=Path.read_bytes
        for name in m._ALLOCATION_INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'changed' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_ALLOCATION_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()
        row=m.allocation_expected_row();case=next(x for x in json.loads((m.ROOT/'tests/unit/xir/legacy_consumers/product_driver_cases.json').read_bytes())['cases'] if x['id']==row['id'])
        self.assertEqual((case['source'],case['entry'],case['source_context']),(m._ALLOCATION_SOURCE,'run','original_absolute'))
        self.assertEqual((case['arguments'],case['stdin_hex'],case['companions']),([],'',[]))
        self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8']);self.assertEqual(len(case['stdout_utf8'].splitlines()),19)
        self.assertEqual((case['expected_exit_code'],case['stderr_utf8']),(0,''))

if __name__=='__main__':unittest.main()
