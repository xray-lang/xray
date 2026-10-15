"""Pin complete original callback, native-layout and memory traces independently."""
from pathlib import Path
import json
import unittest
from unittest import mock
import complete_ffi_memory_model as model


class FfiMemoryTraces(unittest.TestCase):
    def test_every_complete_program_trace(self):
        expected = {
            '_extern_layout_bytecode_lib': [], '_native_size_layout_lib': [],
            'cfn_bsearch': [False], 'cfn_optable': ['sum=30', 'f=1 v=10'],
            'cfn_tail_thread': ['result=42'],
            'cfn_visitor_return_once': ['seen=42', 'result=42', 'visits=1'],
            'extern_decl_large_layout_control_flow': [72, 491, 10],
            'extern_decl_registry_matrix': [8, 103, 42, True, False],
            'extern_direct_noescape_pointer': [0],
            'extern_layout_bytecode_roundtrip': [7, 37, -37],
            'extern_layout_introspection': [8, 8, 6, 16, 8, 0, 8, 10, 24, 8, 0, 4, 8, 16,
                                          4, 4, 0, 0, 32, 8, 0, 24],
            'extern_libm_dylib': ['1.0', '9.0'],
            'extern_pointer_float_prototypes': [False, 66, True],
            'extern_scalar_descriptor_matrix': [3, True, 1234567890123, 23, True, True],
            'extern_used_dylib_unused_symbol_isolation': [True],
            'mem_load_store_native_size': [37, -37, 91, -91, True, True],
            'mem_view_extern_layout': [7, 4100, 91, 91],
            'mem_view_extern_layout_native_size': [37, -37, 91, -91],
            'mem_view_imported_layout_native_size': [37, -37, 91, -91],
            'mem_view_packed_nested_native_size': [7, 37, -37, 91, -91],
            'mem_with_slice_mut_exception_loan_end': ['caught', 9],
            'mem_with_slice_mut_extern_layout': [-73, 73, -73],
            'ptr_memory': [10, 20, 30, 40, 30, False, False],
            'rawptr_copy_nonoverlap_owner': [1, 13437923, 13438180, 13439465, 13439722,
                                           13439979, 13441778, 13442035, 13445633],
            'rawptr_static_null': [True, True, True, 0, 0, 0, 'ok'],
            'unsafe_return_block': [42]}
        self.assertEqual(model.observations(), expected)
        self.assertEqual(sum(map(len, expected.values())), 105)
        rows = model.expected_rows()
        self.assertEqual(len(rows), 26)
        self.assertTrue(all(row['required_target'] == 'windows-x86_64' for row in rows))
        self.assertTrue(all(row['source'] == row['original_source'] for row in rows))
        for name in ['extern_libm', 'extern_no_arg_void_and_unused']:
            self.assertNotIn(name, expected)  # Provider-specific sqrt/rand still need their own evidence.

    def test_fixed_x64_layout_and_nested_packing(self):
        packed, fields = model.aggregate([(1, 1), (8, 8), (8, 8)], packed=True)
        self.assertEqual((packed, fields), ((17, 1), [0, 1, 9]))
        self.assertEqual(model.aggregate([(1, 1), packed]), ((18, 1), [0, 1]))
        self.assertEqual(model.aggregate([(1, 1), (8, 8)]), ((16, 8), [0, 8]))
        self.assertEqual(model.aggregate([(4, 4), (8, 8)], union=True), ((8, 8), [0, 0]))
        self.assertEqual(model.layout_observations()[1], 72)

    def test_unaligned_signed_bytes_and_both_endian_orders(self):
        memory = model.Memory(32)
        memory.store(1, 8, 0x0102030405060708)
        memory.store(10, 8, -37, signed=True)
        memory.store(19, 8, 0x0102030405060708, endian='big')
        self.assertEqual(memory.bytes[1:9], bytes.fromhex('0807060504030201'))
        self.assertEqual(memory.bytes[10:18], bytes.fromhex('dbffffffffffffff'))
        self.assertEqual(memory.bytes[19:27], bytes.fromhex('0102030405060708'))
        self.assertEqual(memory.load(10, 8, signed=True), -37)
        for operation in [lambda: memory.store(31, 8, 1), lambda: memory.load(-1, 8)]:
            with self.assertRaises(ValueError): operation()

    def test_copy_counts_keep_all_destination_sentinels(self):
        source = bytes(range(11, 51))
        for count in [1, 2, 7, 8, 9, 16, 17, 31, 40]:
            with self.subTest(count=count):
                destination = bytearray([205] * 40)
                model.copy_nonoverlapping(destination, source, count)
                self.assertEqual(destination, source[:count] + bytes([205] * (40-count)))
        for count in [0, -1]: model.copy_nonoverlapping(None, None, count)
        with self.assertRaises(ValueError): model.copy_nonoverlapping(None, None, 1)

    def test_exception_ends_loan_and_reentrant_use_is_rejected(self):
        loan = model.Loan(model.Memory(1))
        def interrupted(view):
            view.store(0, 1, 7)
            raise model.LoanStop()
        with self.assertRaises(model.LoanStop): loan.use(interrupted)
        self.assertFalse(loan.active)
        self.assertEqual(loan.use(lambda view: view.load(0, 1)), 7)
        with self.assertRaises(ValueError): loan.use(lambda view: loan.use(lambda nested: None))
        self.assertFalse(loan.active)

    def test_callback_search_distinguishes_found_from_null(self):
        comparison = lambda a, b: (a > b) - (a < b)
        self.assertEqual(model.bsearch_index(7, [1, 3, 7, 9], comparison), 2)
        self.assertIsNone(model.bsearch_index(8, [1, 3, 7, 9], comparison))

    def test_changed_source_manifest_or_authority_cannot_reuse_the_trace(self):
        saved = Path.read_bytes
        for name in model._INPUTS:
            with self.subTest(input=name):
                def changed(path): return saved(path) + b'\nchanged\n' if path == model.ROOT/name else saved(path)
                with mock.patch.object(Path, 'read_bytes', changed), self.assertRaises(ValueError): model.expected_rows()

    def test_exact_complete_program_set_and_source_identity_are_required(self):
        swapped = dict(model._PROGRAMS); names = sorted(swapped)
        swapped[names[0]], swapped[names[1]] = swapped[names[1]], swapped[names[0]]
        for attribute, value in [('_PROGRAMS', swapped), ('_PROGRAMS', {}), ('_INPUTS', {})]:
            with self.subTest(attribute=attribute), mock.patch.object(model, attribute, value), self.assertRaises(ValueError):
                model.expected_rows()

    def test_all_complete_originals_have_public_product_routes_and_companions(self):
        manifest = json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())
        cases = {row['id']: row for row in manifest['cases']}
        for expected in model.expected_rows():
            with self.subTest(program=expected['id']):
                row = cases[expected['id']]
                self.assertEqual(row['source'], expected['original_source'])
                self.assertEqual(row['stdout_utf8'], expected['expected']['stdout_utf8'])
                self.assertEqual((row['entry'], row['source_context']), ('run', 'original_absolute'))
                self.assertIn(model.BASE+'xray.toml', {item['path'] for item in row['companions']})
        self.assertIn(model.BASE+'_native_size_layout_lib.xr',
                      {item['path'] for item in cases['complete_ffi_memory_mem_view_imported_layout_native_size']['companions']})


if __name__ == '__main__': unittest.main()
