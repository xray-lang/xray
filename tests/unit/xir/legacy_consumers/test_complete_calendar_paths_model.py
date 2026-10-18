from datetime import datetime,timezone
import unittest
import complete_calendar_paths_model as model

class CalendarPaths(unittest.TestCase):
    def test_whole_program_traces(self):
        expected={'datetime_core_direct':[2024,2,29,23,45,6,0,4,60,1709250306,'2024-02-29T23:45:06.000Z','2024/02/29 23:45:06 000','2024-02-29 23:45:06','1970-01-01T00:00:00.000Z',1970,-1,999,'1969-12-31T23:59:59.999Z',-2,999,2024,1,15,10,30,45,123,'2024-01-15T10:30:45.123Z',2024,1,15,3,1,2,29,-86400,-24,True,True,True,True,True,True,29,2024,6,1,0,0,0],
         'log_level_enum_direct':['LogLevel.Debug','LogLevel.Info','LogLevel.Warn','LogLevel.Error','LogLevel.Fatal','LogFormat.Text','LogFormat.JSON',True,False,False,False,True,True,True,'LogLevel.Fatal',2],
         'path_string_ops':['/',True,'/usr/local','foo','/','.','/a','bin','bar.txt','b','solo','.txt','.gz','','','/usr/bin/xray','/usr/local/bin','baz','/foo','.','.','../lib','.','../foobar','bin','..','../../x/y','../qux/zap','foo/bar','foo/bar/baz','/usr/local/bin','foo/bar','/bar/baz','/x',True,'x','/foo/bar','/bar/baz','/var','/','/','/home/user','file.txt','file','.txt','foo.txt','foo','.txt','/home/user/file.txt','/home/user/file.txt','./archive.tar','/srv/data','report.csv','.csv','/srv/data/report.csv','report.csv']}
        for name,values in expected.items():
            with self.subTest(program=name):self.assertEqual(model.observations()[name],values)

    def test_negative_epoch_floor_division(self):
        for value,expected in [(-1,(-1,999)),(-1001,(-2,999)),(-1000,(-1,0)),(1499,(1,499)),(1500,(1,500))]:
            with self.subTest(milliseconds=value):self.assertEqual(model.split_milliseconds(value),expected)

    def test_month_clamping_and_calendar_boundaries(self):
        for value,amount,expected in [(datetime(2024,1,31),1,(2024,2,29)),(datetime(2023,1,31),1,(2023,2,28)),(datetime(2024,3,31),-1,(2024,2,29))]:
            with self.subTest(value=value,amount=amount):
                result=model.add_months(value,amount);self.assertEqual((result.year,result.month,result.day),expected)

    def test_component_relative_join_and_dotfiles(self):
        for start,end,expected in [('/foo','/foobar','../foobar'),('/a/b/c','/a/x/y','../../x/y'),('foo/bar/../baz','foo/qux/./zap','../qux/zap'),('/same','/same','.')]:
            with self.subTest(start=start,end=end):self.assertEqual(model.relative(start,end),expected)
        self.assertEqual(model.join('prefix','/reset','tail'),'/reset/tail');self.assertEqual(model.extname('.gitignore'),'')
        self.assertEqual(model.format_path(model.parse_path('archive.tar')),'./archive.tar')
        with self.assertRaises(ValueError):model.resolve('relative','only')

    def test_nominal_levels_and_exact_utc_timestamp(self):
        self.assertEqual(model.log_observations()[:2],['LogLevel.Debug','LogLevel.Info'])
        self.assertEqual(model.milliseconds(datetime(2024,2,29,23,45,6,tzinfo=timezone.utc)),1709250306000)

    def test_complete_file_logger_trace_keeps_write_flush_read_and_remove(self):
        trace=model.log_file_trace();self.assertEqual(trace['outputs'],['LogLevel.'+level for level in ('Debug','Info','Warn','Error','Fatal')]+['Logger']+[True]*5)
        self.assertEqual(trace['file_before_remove'],'INFO server started port=8080 host=localhost\nWARN slow response module=http ms=42\n{"level":"ERROR","msg":"request failed","module":"http","route":"/api","code":500}\nDEBUG queued ok=true\n')
        self.assertEqual([event[0] for event in trace['events']],['writeFile']+['appendFile']*4+['readFile','remove'])
        self.assertEqual(trace['queued_before_flush'],['DEBUG queued ok=true']);self.assertEqual(trace['queue_after_flush'],[]);self.assertFalse(trace['file_exists_after_remove'])

    def test_logger_child_captures_configuration_and_output_switch_flushes_old_queue(self):
        state=model.LogFixtureState();state.write_file('old','');state.write_file('new','');state.set_output('old')
        child=state.child(state.root(),'module','http');state.format='JSON';state.level='Fatal'
        state.emit(child,'Warn','before','x',7);self.assertEqual(state.files['old'],'WARN before module=http x=7\n')
        state.level='Debug';state.enable_async(True);state.emit(state.root(),'Debug','queued','ok',True)
        self.assertNotIn('queued',state.files['old']);state.set_output('new')
        self.assertEqual(state.queue,[]);self.assertIn('"msg":"queued"',state.files['old']);self.assertEqual(state.files['new'],'')
        state.emit(state.root(),'Debug','next');state.enable_async(False);self.assertIn('"msg":"next"',state.files['new'])
        self.assertEqual(child[4],(('module','http'),));self.assertEqual(state.root()[4],())

    def test_logger_filtered_calls_and_complete_source_binding(self):
        from unittest.mock import patch
        state=model.LogFixtureState();state.write_file('out','');state.set_output('out');state.enable_async(True)
        state.emit(state.root(),'Debug','not enabled');self.assertEqual(state.queue,[])
        row=next(row for row in model.expected_rows() if row['source'].endswith('/log_pure_module_direct.xr'))
        self.assertEqual(row['observation_count'],11);self.assertFalse(row['required_semantic_trace']['file_exists_after_remove'])
        with patch.dict(model._INPUTS,{row['source']:'0'*64}):
            with self.assertRaisesRegex(ValueError,'source/authority changed'):model.expected_rows()

if __name__=='__main__':unittest.main()
