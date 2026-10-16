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
        self.assertEqual(m.observations(),expected)
        self.assertEqual(sum(map(len,expected.values())),82)
        self.assertEqual(len(m.expected_rows()),5)

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


if __name__=='__main__':unittest.main()
