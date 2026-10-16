"""Complete filesystem outputs, independent effects and retained provider gaps."""
from pathlib import Path
import itertools,json,unittest
from unittest import mock
import complete_filesystem_io_model as m


class CompleteFilesystemIO(unittest.TestCase):
    def test_all_eight_complete_programs_and141_original_observations(self):
        traces=m.traces();expected={
            'io_binary_file_boundary_direct':[True,True,256,0,1,127,128,255,True,True],
            'io_chmod_shared_core':[True]*8,'io_path_result_shared_core':[True]*14,
            'io_read_dir_shared_core':[True]*13+[0,True],
            'io_remove_all_shared_core':[True]*10,
            'io_touch_shared_core':[True,True,0,True,True,True,True,True,True,True],
            'io_write_shared_core':[True,0,True,True,True,True,True,4,65,0,10,255,True],
            'io_system_direct':[True]*10+[5,True,True,2,'hello','world',True,1,'alpha',True,3,'a',True,'b',True,3,65,0,255]+[True]*32}
        self.assertEqual(sum(map(len,expected.values())),141)
        self.assertEqual({k:v['observations'] for k,v in traces.items()},expected)
        rows=m.expected_rows();self.assertEqual(len(rows),8);self.assertEqual(sum(x['observation_count'] for x in rows),141)
        for row in rows:
            name=Path(row['source']).stem
            self.assertEqual(row['expected']['stdout_utf8'],''.join(m.render({'expected':x})+'\n' for x in expected[name]))

    def test_all256_binary_bytes_and_independent_polynomial_known_answer(self):
        trace=m.traces()['io_binary_file_boundary_direct'];payload=bytes.fromhex(trace['complete_payload_hex'])
        self.assertEqual(payload,bytes(range(256)));self.assertEqual(m.polynomial_hash(payload),290104970)
        self.assertEqual((trace['required_initialization_iterations'],trace['required_hash_loop_iterations']),(256,256))
        for index in range(256):
            changed=bytearray(payload);changed[index]^=1
            self.assertNotEqual(m.polynomial_hash(changed),290104970)
        self.assertEqual(m.polynomial_hash(b''),0)

    def test_empty_append_binary_nul_high_bytes_and_exact_content_effects(self):
        trace=m.traces()['io_write_shared_core'];writes=[x for x in trace['side_effects'] if x['operation'] in {'write','append'}]
        self.assertEqual([x['result_hex'] for x in writes],['','616c706861','616c706861','616c7068610a6f6d656761','41000aff'])
        fs=m.Tree();root=fs.temp_dir();data=bytearray([65,0,10,255]);self.assertTrue(fs.write(root+'/x',data));data[0]=90
        self.assertEqual(fs.read(root+'/x'),b'A\x00\n\xff')

    def test_read_lines_preserves_empty_crlf_and_trailing_line_policy(self):
        self.assertEqual(m.lines(b'hello\nworld'),['hello','world'])
        self.assertEqual(m.lines(b'alpha\n'),['alpha']);self.assertEqual(m.lines(b'a\r\n\r\nb\r\n'),['a','','b'])
        self.assertEqual(m.lines(b''),[]);self.assertEqual(m.lines(b'\n'),['']);self.assertEqual(m.lines(b'a\r'),['a\r'])
        self.assertEqual(m.lines(b'a\vb\fc'),['a\vb\fc'])
        self.assertEqual(m.lines(b'\xff\n'),['�'])

    def test_all128_directory_reopens_and135_pair_obligations_remain(self):
        trace=m.traces()['io_read_dir_shared_core']
        self.assertEqual(trace['required_original_reopen_calls'],128);self.assertEqual(trace['reopen_results'],[True]*128)
        self.assertEqual(trace['directory_iterator_pair_obligations'],135)
        self.assertEqual(trace['entry_names'],['a','top.txt'])
        self.assertEqual(trace['recursive_names'],['a','a/b','a/b/deep.txt','top.txt'])
        # These are required operations, not measured native descriptor closes.
        self.assertEqual(m.traces()['io_system_direct']['directory_iterator_pair_obligations'],7)

    def test_original_directory_membership_observations_do_not_assume_host_order(self):
        for entries in itertools.permutations(['a','top.txt']):
            self.assertEqual(['top.txt' in entries,'a' in entries,'.' not in entries,'..' not in entries],[True]*4)
        for entries in itertools.permutations(['a','a/b','a/b/deep.txt','top.txt']):
            self.assertTrue(all(x in entries for x in ['top.txt','a','a/b','a/b/deep.txt']))

    def test_all_owned_paths_cleaned_and_original_cwd_restored(self):
        for name,trace in m.traces().items():
            with self.subTest(program=name):
                self.assertEqual(trace['live_owned_paths'],[]);self.assertTrue(trace['cwd_restored'])
        for name in ['io_path_result_shared_core','io_system_direct']:
            events=[x for x in m.traces()[name]['side_effects'] if x['operation']=='chdir']
            self.assertEqual(len(events),2);self.assertEqual(events[-1]['path'],'/working')

    def test_both_symlink_capability_branches_keep_all_original_outputs(self):
        absent=m.traces(link_ok=False);present=m.traces(link_ok=True)
        for name in m._NAMES:self.assertEqual(absent[name]['observations'],present[name]['observations'])
        for name in ['io_path_result_shared_core','io_system_direct']:
            self.assertFalse(absent[name]['symlink_branch']);self.assertTrue(present[name]['symlink_branch'])
            self.assertIn('symlink_refused',[x['operation'] for x in absent[name]['side_effects']])
            self.assertIn('symlink',[x['operation'] for x in present[name]['side_effects']])
            self.assertEqual(present[name]['live_owned_paths'],[])

    def test_windows_plain_crt_readonly_cleanup_conflict_is_not_erased(self):
        required=m.traces()['io_chmod_shared_core'];plain=m.traces(delete_policy='plain_crt')['io_chmod_shared_core']
        self.assertEqual(required['observations'],[True]*8);self.assertEqual(plain['observations'],[True]*6+[False,False])
        self.assertTrue(plain['live_owned_paths'])
        row=next(x for x in m.expected_rows() if x['source'].endswith('io_chmod_shared_core.xr'))
        self.assertEqual(row['current_windows_leaf_projection']['plain_crt_observations'],plain['observations'])
        self.assertIn('not_proved',row['current_windows_leaf_projection']['qualification'])

    def test_touch_preserves_payload_and_original_clock_failure_predicate(self):
        for delta in [0,1,17]:self.assertTrue(m.traces(touch_delta=delta)['io_touch_shared_core']['observations'][8])
        rollback=m.traces(touch_delta=-1)['io_touch_shared_core']['observations']
        self.assertFalse(rollback[8]);self.assertTrue(rollback[7]);self.assertTrue(rollback[9])
        fs=m.Tree(touch_delta=1);root=fs.temp_dir();fs.write(root+'/x','payload');before=fs.stat(root+'/x');fs.touch(root+'/x')
        self.assertEqual(before['mtime'],17);self.assertEqual(fs.stat(root+'/x')['mtime'],18);self.assertEqual(fs.read(root+'/x'),b'payload')

    def test_windows_and_posix_mode_observations_keep_original_platform_branch(self):
        for platform in ['windows','posix']:
            self.assertEqual(m.traces(platform=platform)['io_chmod_shared_core']['observations'],[True]*8)
        with self.assertRaises(ValueError):m.Tree(platform='unreviewed')
        with self.assertRaises(ValueError):m.Tree(delete_policy='unreviewed')

    def test_recursive_remove_does_not_follow_link_outside_owned_tree(self):
        fs=m.Tree(link_ok=True);root=fs.temp_dir();fs.write('/working/foreign','keep');fs.symlink('/working/foreign',root+'/link')
        self.assertTrue(fs.remove(root,True));self.assertEqual(fs.read('/working/foreign'),b'keep')
        self.assertFalse(fs.remove(root,True));self.assertEqual(fs.live_owned(),[])
        root=fs.temp_dir();fs.symlink('/working/missing',root+'/dangling');self.assertTrue(fs.remove(root,True))

    def test_copy_rename_and_remove_preserve_exact_byte_ownership(self):
        fs=m.Tree();root=fs.temp_dir();fs.write(root+'/a',b'hello\nworld');self.assertTrue(fs.copy(root+'/a',root+'/b'))
        fs.write(root+'/a',b'changed');self.assertEqual(fs.read(root+'/b'),b'hello\nworld')
        self.assertTrue(fs.rename(root+'/b',root+'/c'));self.assertFalse(fs.exists(root+'/b'));self.assertEqual(fs.read(root+'/c'),b'hello\nworld')
        self.assertTrue(fs.remove(root,True));self.assertFalse(fs.exists(root))

    def test_missing_and_invalid_path_operations_fail_without_fabricated_data(self):
        fs=m.Tree();self.assertIsNone(fs.read('/missing'));self.assertIsNone(fs.stat('/missing'))
        self.assertIsNone(fs.realpath('/missing'));self.assertIsNone(fs.readlink('/missing'))
        self.assertIsNone(fs.read_dir('/missing'));self.assertIsNone(fs.recursive('/missing'));self.assertEqual(fs.directory_pairs,0)
        self.assertFalse(fs.write('/missing/x',b'x'));self.assertFalse(fs.write('/working',b'x'))
        fs.write('/working/file',b'x');self.assertFalse(fs.mkdirp('/working/file/child'))
        self.assertFalse(fs.copy('/missing','/working/dst'));self.assertFalse(fs.rename('/missing','/working/dst'))

    def test_every_complete_source_and_normative_input_is_pinned(self):
        saved=Path.read_bytes
        for name in m._INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'\nchanged\n' if path==m.ROOT/name else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()

    def test_actual_products_keep_whole_original_sources_and_all_arguments(self):
        folder=Path(__file__).parent;cases={x['id']:x for x in json.loads((folder/'product_driver_cases.json').read_text())['cases']}
        for row in m.expected_rows():
            case=cases[row['id']];self.assertEqual(case['source'],row['source']);self.assertEqual(case['source'],case['preserved_original_identity']['source'])
            self.assertEqual(case['entry'],'run');self.assertEqual(case['source_context'],'original_absolute')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8']);self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')
            self.assertIn('descriptor release',case['independent_execution_model'])


if __name__=='__main__':unittest.main()
