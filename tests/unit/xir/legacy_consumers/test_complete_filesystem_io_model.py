"""Complete filesystem outputs, independent effects and retained provider gaps."""
from provenance_test_support import provenance_input_path
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
        rows=[row for row in m.expected_rows() if row['id']!=m._DYLIB_ID and row['id'] not in m._SYSTEM_QUERY_IDS];self.assertEqual(len(rows),8);self.assertEqual(sum(x['observation_count'] for x in rows),141)
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
                def changed(path):return saved(path)+b'\nchanged\n' if path==provenance_input_path(m.ROOT, name) else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()

    def test_actual_products_keep_whole_original_sources_and_all_arguments(self):
        folder=Path(__file__).parent;cases={x['id']:x for x in json.loads((folder/'product_driver_cases.json').read_text())['cases']}
        for row in (row for row in m.expected_rows() if row['id']!=m._DYLIB_ID and row['id'] not in m._SYSTEM_QUERY_IDS):
            case=cases[row['id']];self.assertEqual(case['source'],row['source']);self.assertEqual(case['source'],case['preserved_original_identity']['source'])
            self.assertEqual(case['entry'],'run');self.assertEqual(case['source_context'],'original_absolute')
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8']);self.assertEqual(case['arguments'],[]);self.assertEqual(case['stdin_hex'],'')
            self.assertIn('descriptor release',case['independent_execution_model'])


    def test_complete_dylib_all_ten_observations_and_single_provider_close(self):
        trace=m.dylib_handle_trace()
        self.assertEqual(trace['observations'],[True]*10)
        self.assertEqual(trace['provider_calls'],[
            ('open','kernel32.dll'),('symbol',1,'GetCurrentProcessId'),
            ('symbol',1,'__xray_missing_symbol_for_dylib_test__'),('close',1),
            ('open','__xray_missing_library_for_dylib_test__')])
        self.assertEqual((trace['logical_handle'],trace['logical_open']),(0,False))
        self.assertFalse(trace['function_address_invoked'])
        self.assertFalse(trace['host_provider_orphysical_release_qualified'])

    def test_dylib_idempotent_close_is_true_even_after_first_failure(self):
        provider=m.DylibSymbols(close_ok=False);handle=m.DylibHandle(provider,1)
        self.assertFalse(handle.close());self.assertTrue(handle.close())
        self.assertIsNone(handle.symbol('GetCurrentProcessId'))
        self.assertEqual(provider.calls,[('close',1)])
        self.assertEqual((handle.handle,handle.is_open),(0,False))

    def test_dylib_missing_host_facts_cannot_silently_reuse_positive_outputs(self):
        for changes in ({'available':False},{'exported':False},{'missing_export':True},
                        {'missing_library':True},{'error_text':''},{'close_ok':False}):
            with self.subTest(changes=changes):
                self.assertNotEqual(m.dylib_handle_trace(m.DylibSymbols(**changes))['observations'],[True]*10)

    def test_dylib_changed_original_or_any_reviewed_authority_fails_closed(self):
        saved=Path.read_bytes
        for name in m._DYLIB_INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'changed' if path==provenance_input_path(m.ROOT, name) else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        with mock.patch.object(m,'_DYLIB_INPUTS',{}),self.assertRaises(ValueError):m.expected_rows()

    def test_dylib_whole_original_and_public_product_keep_every_call_and_output(self):
        original=(m.ROOT/m._DYLIB_SOURCE).read_text(encoding='utf-8')
        self.assertEqual(original.count('print('),10)
        self.assertEqual(original.count('import '),5)
        self.assertEqual(original.count('lib!.close()'),2)
        self.assertIn('var symPtr: Ptr<u8> = sym!',original)
        self.assertIn('print(mem.addr(symPtr) != 0)',original)
        self.assertEqual(original.count('sys.Dylib.open('),2)
        self.assertEqual(original.count('lib!.symbol('),3)
        row=next(row for row in m.expected_rows() if row['id']==m._DYLIB_ID)
        self.assertEqual(row['expected'],{'exit_code':0,'stdout_utf8':('true'+chr(10))*10,'stderr_utf8':''})
        self.assertEqual(row['source'],row['original_source']);self.assertEqual(row['observation_count'],10)
        cases=json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']
        case=next(case for case in cases if case['id']==row['id'])
        self.assertEqual((case['source'],case['entry'],case['source_context']),(m._DYLIB_SOURCE,'run','original_absolute'))
        self.assertEqual((case['arguments'],case['stdin_hex'],case['companions']),([],'',[]))
        self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8'])


    def test_whole_OS_query_has_all39_outputs_andboth_environment_snapshots(self):
        trace=m.system_query_trace()
        self.assertEqual(trace['observations'],[True]*39)
        self.assertEqual((trace['map_snapshot_nonempty'],trace['map_snapshot_empty']),('map-aot',''))
        self.assertEqual(trace['environment_before'],trace['environment_after'])
        self.assertTrue(trace['fallback_bindings_restored'])
        self.assertEqual(trace['owned_test_bindings_remaining'],[])
        self.assertEqual((trace['cwd_change_target'],trace['cwd_after']),('/working','/working'))
        self.assertEqual(trace['clock_sample_count'],1)
        self.assertFalse(trace['host_provider_IO_resource_lifetime_qualified'])

    def test_OS_original_restore_branches_keep_absent_empty_andnonempty_distinct(self):
        names=('TMPDIR','TMP','TEMP','HOME')
        for choices in itertools.product((None,'','original'),repeat=4):
            initial={name:value for name,value in zip(names,choices) if value is not None}
            initial['UNRELATED']='keep';initial['USERPROFILE']='/profile'
            trace=m.system_query_trace(initial)
            self.assertEqual(trace['observations'],[True]*39)
            self.assertEqual(trace['environment_after'],initial)
            self.assertTrue(trace['fallback_bindings_restored'])
            self.assertEqual(trace['owned_test_bindings_remaining'],[])
        self.assertEqual(m.system_query_trace()['saved_fallback_bindings'],dict.fromkeys(names))

    def test_OS_empty_binding_is_required_andCRT_removal_cannot_bea_passing_prefix(self):
        env=m.SystemEnvironment();env.set('key','')
        self.assertEqual(env.environ(),{'key':''});self.assertEqual(env.get('key'),'')
        env.unset('key');self.assertIsNone(env.get('key'))
        self.assertNotIn('key',env.environ())
        def plain_crt(initial):return m.SystemEnvironment(initial,'crt_remove')
        with self.assertRaises(KeyError):m.system_query_trace(environment_class=plain_crt)
        with self.assertRaises(ValueError):m.SystemEnvironment(empty_policy='ignore')

    def test_OS_host_predicate_failures_are_not_silently_normalized_to_true(self):
        for change in ({'platform':''},{'arch':''},{'sep':''},{'eol':''},{'pid':0},{'uid':-1},
                       {'gid':-1},{'cpu_count':0},{'ppid':0},{'signal_zero_ok':False},
                       {'total_memory':0},{'free_memory':0},{'uptime':-1},{'loadavg':[-1,0,0]},
                       {'clock':-1},{'cwd':''},{'chdir_ok':False},{'hostname':''},{'username':None},
                       {'system_home':None}):
            with self.subTest(change=change):self.assertNotEqual(m.system_query_trace(facts=change)['observations'],[True]*39)
        self.assertFalse(m.system_query_trace({'XRAY_TEST_NONEXISTENT_VAR_12345':'occupied'})['observations'][18])

    def test_two_offset_readings_keep_only_the_three_original_predicates(self):
        for first,second in itertools.product((-1441,-1440,-1,0,1,1440,1441),repeat=2):
            trace=m.timezone_offset_trace(first,second)
            self.assertEqual(trace['observations'],[first>=-1440,first<=1440,second>=-1440])
            self.assertEqual(trace['original_offset_call_count'],2)
            self.assertFalse(trace['requires_same_timestamp_or_offset'])
            self.assertFalse(trace['exact_offset_value_asserted'])
        self.assertEqual(m.timezone_offset_trace(-1440,100000)['observations'],[True]*3)
        self.assertEqual(m.timezone_offset_trace(1440,-1440)['observations'],[True]*3)

    def test_system_query_whole_sources_andnew_authorities_fail_closed(self):
        saved=Path.read_bytes
        for name in m._SYSTEM_QUERY_INPUTS:
            with self.subTest(input=name):
                def changed(path):return saved(path)+b'changed' if path==provenance_input_path(m.ROOT, name) else saved(path)
                with mock.patch.object(Path,'read_bytes',changed),self.assertRaises(ValueError):m.expected_rows()
        for attribute in ('_SYSTEM_PROGRAMS','_SYSTEM_QUERY_IDS','_SYSTEM_QUERY_INPUTS'):
            with mock.patch.object(m,attribute,{}),self.assertRaises(ValueError):m.expected_rows()

    def test_both_whole_system_originals_have_exact_public_products_andall_calls(self):
        cases={case['id']:case for case in json.loads((Path(__file__).parent/'product_driver_cases.json').read_text())['cases']}
        for name,prints,imports in (('os_query_system_direct',39,4),('datetime_offset_system_direct',3,1)):
            source=m._SYSTEM_PROGRAMS[name];text=(m.ROOT/source).read_text(encoding='utf-8')
            self.assertEqual(text.count('print('),prints);self.assertEqual(text.count('import '),imports)
            row=next(row for row in m.expected_rows() if row['source']==source);case=cases[row['id']]
            self.assertEqual(row['expected'],{'exit_code':0,'stdout_utf8':('true'+chr(10))*prints,'stderr_utf8':''})
            self.assertEqual((row['source'],row['original_source'],row['observation_count']),(source,source,prints))
            self.assertEqual((case['source'],case['entry'],case['source_context']),(source,'run','original_absolute'))
            self.assertEqual((case['arguments'],case['stdin_hex'],case['companions']),([],'',[]))
            self.assertEqual(case['stdout_utf8'],row['expected']['stdout_utf8'])
        text=(m.ROOT/m._SYSTEM_PROGRAMS['os_query_system_direct']).read_text()
        self.assertEqual(text.count('os.setenv('),11);self.assertEqual(text.count('os.unsetenv('),7)
        self.assertEqual(text.count('os.environ()'),2);self.assertEqual(text.count('io.chdir('),1)
        text=(m.ROOT/m._SYSTEM_PROGRAMS['datetime_offset_system_direct']).read_text()
        self.assertEqual(text.count('datetime.offset()'),2)

if __name__=='__main__':unittest.main()
