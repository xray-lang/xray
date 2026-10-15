import copy,hashlib,unittest
from pathlib import Path
from unittest import mock
import complete_borrowed_views_model as model

class BorrowedViews(unittest.TestCase):
    def test_complete_original_observations(self):
        expected={'array_expression_view':[21,31,21,31],
          'array_negative_both':[[2,3],[2,3],[1,2,3],[]],
          'array_negative_end':[[1,2,3],[1,2,3],[],[]],
          'array_negative_start':[[3,4],[3,4],[1,2,3,4],[]],
          'string_byte_slice_view':[4,65,66,2,66,67,66],
          'string_byte_slice_receiver_nll':[5,3],
          'slice_window_shared_core':[3,10,30,99,2,99,30,0,0,6,50]}
        self.assertEqual(model.observations(),expected)
        self.assertEqual(sum(map(len,expected.values())),36)
    def test_shared_read_permissions_and_owned_snapshot(self):
        owner=model.Owner([[1],[2]]);a=owner.view();b=owner.view();self.assertEqual(owner.read(0),[1])
        with self.assertRaises(model.BorrowConflict):owner.write(0,[9])
        with self.assertRaises(model.BorrowConflict):a.write(0,[9])
        with self.assertRaises(model.BorrowConflict):a.child(mutable=True)
        saved=a.snapshot();a.close();b.close();owner.write(0,[9]);saved[1][0]=7
        self.assertEqual(saved,[[1],[7]]);self.assertEqual(owner.items,[[9],[2]])
    def test_exclusive_view_blocks_all_owner_paths(self):
        owner=model.Owner([1,2]);view=owner.view(mutable=True)
        for operation in [lambda:owner.read(0),lambda:owner.write(0,9),lambda:owner.rebind([]),lambda:owner.view()]:
            with self.subTest(operation=operation),self.assertRaises(model.BorrowConflict):operation()
        view.write(0,7);view.close();self.assertEqual(owner.read(0),7)
        with self.assertRaises(model.BorrowConflict):view.read(0)
    def test_child_reborrow_suspends_exclusive_parent(self):
        owner=model.Owner([1,2,3]);parent=owner.view(mutable=True);child=parent.child(1,3)
        for operation in [lambda:parent.read(0),lambda:parent.write(0,9),parent.close]:
            with self.subTest(operation=operation),self.assertRaises(model.BorrowConflict):operation()
        self.assertEqual(child.snapshot(),[2,3]);child.close();parent.write(1,8);parent.close();self.assertEqual(owner.read(1),8)
    def test_clamping_and_empty_windows(self):
        for start,end,expected in [(-999,999,(0,4)),(-1,-999,(3,3)),(999,None,(4,4)),(None,-999,(0,0)),(4,4,(4,4))]:
            with self.subTest(start=start,end=end):self.assertEqual(model.bounds(4,start,end),expected)
    def test_every_delivered_input_is_frozen(self):
        saved=Path.read_bytes
        for name in model._INPUTS:
            with self.subTest(source=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==model.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()
    def test_program_and_port_identity_cannot_be_swapped_or_omitted(self):
        programs=dict(model._PROGRAMS)
        a,b='string_byte_slice_view','string_byte_slice_receiver_nll';programs[a],programs[b]=programs[b],programs[a]
        for attribute,bad in [('_PROGRAMS',programs),('_PROGRAMS',{}),('_PORTS',{}),('_INPUTS',{})]:
            with self.subTest(attribute=attribute),mock.patch.object(model,attribute,bad),self.assertRaises(ValueError):model.expected_rows()
    def test_matching_digest_does_not_authorize_a_changed_complete_port(self):
        name=model._PORTS['slice_window_shared_core'];saved=Path.read_bytes;raw=saved(model.ROOT/name)+b'\nprint("extra")\n'
        inputs=dict(model._INPUTS);inputs[name]=hashlib.sha256(raw.replace(b'\r\n',b'\n')).hexdigest()
        def changed(path):return raw if path==model.ROOT/name else saved(path)
        with mock.patch.object(model,'_INPUTS',inputs),mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):model.expected_rows()

if __name__=='__main__':unittest.main()
