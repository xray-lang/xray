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

if __name__=='__main__':unittest.main()
