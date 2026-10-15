"""Independent complete calendar, POSIX path and typed log-level traces."""
from pathlib import Path
from datetime import datetime,timedelta,timezone
import calendar,hashlib,posixpath

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/012-11-modules.md': '3c32493b1e22d29c459362a9f465a6f5316856b5b375c3d1c1e06025756eecd8',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'spec/source/sections/018-17-compilation-pipeline.md': '462e39b670cb0a0c495ff677f0e39212fe49bcdc33234b9f5e89e27288085694',
 'stdlib/datetime/datetime.xr': 'cd05dfebf4afb1d5a1bf2326aa84afd2dbe17825d90ca6f9a340751db596197d',
 'stdlib/io/io.xr': 'cbb670fe3a25e42a3fa96a3bf3460f9696f459c0fac059b82ba8bc99c4d17096',
 'stdlib/log/log.xr': 'd6f32e59f72cbaf35df46ce3438f576756b7fb0a2820e1c98bbc03bebdfea835',
 'stdlib/os/os.xr': '32733efee3a175b97087a6750d7c37447b58ede1aa0359d04877b81462331098',
 'stdlib/path/path.xr': 'c6d1721f27e93a385053105cfbf33d88dae8e038c79e29085b11dd70e08225a1',
 'stdlib/text/text.xr': '55d2aaa9e524bc1924f6b426b763f9d45e52f926f204ded84c7e8a64289b9aef',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'tests/diff/cases/semantics/stdlib/datetime_core_direct.xr': '6e08d89f3daf3e8105f98616db0219dfd4f476768ff6d782c5c93d31c5175c4e',
 'tests/diff/cases/semantics/stdlib/log_level_enum_direct.xr': '19734d46f7f1c2e1e9f794c50524345b52afb36424ab70661a4a56a6f5ac1374',
 'tests/diff/cases/semantics/stdlib/path_string_ops.xr': 'daecb9f8ca5a32e311b89c91991e718dcbb6334758d2b61d105a42a991ba4bbc'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/012-11-modules.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/016-15-standard-library.md',
 'spec/source/sections/018-17-compilation-pipeline.md',
 'stdlib/datetime/datetime.xr',
 'stdlib/io/io.xr',
 'stdlib/log/log.xr',
 'stdlib/os/os.xr',
 'stdlib/path/path.xr',
 'stdlib/text/text.xr',
 'stdlib/time/time.xr',
 'stdlib/types/array.xr',
 'stdlib/types/string.xr',
 'tests/diff/cases/semantics/stdlib/datetime_core_direct.xr',
 'tests/diff/cases/semantics/stdlib/log_level_enum_direct.xr',
 'tests/diff/cases/semantics/stdlib/path_string_ops.xr')
_PROGRAMS = {'datetime_core_direct': 'tests/diff/cases/semantics/stdlib/datetime_core_direct.xr',
 'log_level_enum_direct': 'tests/diff/cases/semantics/stdlib/log_level_enum_direct.xr',
 'path_string_ops': 'tests/diff/cases/semantics/stdlib/path_string_ops.xr'}
_NAMES=frozenset({'datetime_core_direct','path_string_ops','log_level_enum_direct'})
EPOCH=datetime(1970,1,1,tzinfo=timezone.utc)

def milliseconds(value):
    delta=value-EPOCH
    return (delta.days*86400+delta.seconds)*1000+delta.microseconds//1000

def split_milliseconds(value):return divmod(value,1000)

def iso(value):return value.isoformat(timespec='milliseconds').replace('+00:00','Z')

def add_months(value,amount):
    year,month0=divmod(value.year*12+value.month-1+amount,12)
    return value.replace(year=year,month=month0+1,day=min(value.day,calendar.monthrange(year,month0+1)[1]))

def basename(value):return posixpath.basename(value.rstrip('/'))
def dirname(value):return posixpath.dirname(value.rstrip('/')) or ('/' if value.startswith('/') else '.')
def extname(value):return posixpath.splitext(basename(value))[1]
def normalize(value):return posixpath.normpath(value)

def relative(start,end):
    a,b=normalize(start),normalize(end)
    if a.startswith('/')!=b.startswith('/'):raise ValueError('This reviewed model does not mix absolute and relative anchors')
    first=[s for s in a.split('/') if s and s!='.'];second=[s for s in b.split('/') if s and s!='.'];common=0
    for left,right in zip(first,second):
        if left!=right:break
        common+=1
    return '/'.join(['..']*(len(first)-common)+second[common:]) or '.'

def join(*parts):return posixpath.join(*parts) if parts else ''
def resolve(*parts):
    if not any(p.startswith('/') for p in parts):raise ValueError('No current directory is consulted by this model')
    return normalize(join(*parts))

def parse_path(value):
    base=basename(value);ext=extname(value)
    return {'root':'/' if value.startswith('/') else '', 'dir':dirname(value),'base':base,'name':base[:-len(ext)] if ext else base,'ext':ext}

def format_path(info):return join(info['dir'],info['base'] or info['name']+info['ext'])

def calendar_observations():
    dt=datetime(2024,2,29,23,45,6,tzinfo=timezone.utc);rows=[dt.year,dt.month,dt.day,dt.hour,dt.minute,dt.second,dt.microsecond//1000,(dt.weekday()+1)%7,dt.timetuple().tm_yday,milliseconds(dt)//1000,
     iso(dt),dt.strftime('%Y/%m/%d %H:%M:%S')+' 000',dt.strftime('%Y-%m-%d %H:%M:%S'),iso(EPOCH),EPOCH.year]
    before=EPOCH+timedelta(milliseconds=-1);rows.extend([split_milliseconds(-1)[0],split_milliseconds(-1)[1],iso(before),*split_milliseconds(-1001)])
    parsed=datetime.fromisoformat('2024-01-15T10:30:45.123+00:00');rows.extend([parsed.year,parsed.month,parsed.day,parsed.hour,parsed.minute,parsed.second,parsed.microsecond//1000,iso(parsed)])
    date_only=datetime.strptime('2024-01-15','%Y-%m-%d');rows.extend([date_only.year,date_only.month,date_only.day])
    day=dt+timedelta(days=1);month=add_months(datetime(2024,1,31,tzinfo=timezone.utc),1);rows.extend([day.month,day.day,month.month,month.day])
    dt2=dt+timedelta(days=1);delta_ms=milliseconds(dt)-milliseconds(dt2);a,b=1500,1499
    rows.extend([delta_ms//1000,delta_ms//3600000,dt<dt2,dt2>dt,dt==datetime(2024,2,29,23,45,6,tzinfo=timezone.utc),a>b,a==1500,calendar.isleap(dt.year),calendar.monthrange(dt.year,dt.month)[1]])
    # create(2024,6) observes civil defaults only; toUTC/toLocal preserve the instant.
    # No local offset, current instant or host clock is predicted or substituted.
    civil=datetime(2024,6,1);rows.extend([civil.year,civil.month,civil.day,0,0,0])
    return rows

def path_observations():
    rows=['/',len(':')==1]
    rows.extend(dirname(p) for p in ['/usr/local/bin','foo/bar.txt','/','noslash','/a/b/'])
    rows.extend(basename(p) for p in ['/usr/local/bin','foo/bar.txt','/a/b/','solo'])
    rows.extend(extname(p) for p in ['foo/bar.txt','archive.tar.gz','noext','.gitignore'])
    rows.extend(normalize(p) for p in ['/usr/local/../bin/./xray','/usr//local///bin','foo/bar/../../baz','/../foo','.',''])
    rows.extend(relative(a,b) for a,b in [('/usr/local/bin','/usr/local/lib'),('/usr/local','/usr/local'),('/foo','/foobar'),('/usr/local','/usr/local/bin'),('/usr/local/bin','/usr/local'),('/a/b/c','/a/x/y'),('foo/bar/../baz','foo/qux/./zap')])
    rows.extend(join(*parts) for parts in [('foo','bar'),('foo','bar','baz'),('/usr','local','bin'),('foo/','bar'),('foo','/bar','baz'),('very/long/prefix/that/resets','/x')])
    rows.extend([join()=='',join('','x')])
    rows.extend(resolve(*parts) for parts in [('/foo','bar'),('/foo','/bar','baz'),('/tmp','../var'),('/foo','..')])
    parsed=parse_path('/home/user/file.txt');rows.extend(parsed.values());trailing=parse_path('foo.txt/');rows.extend(trailing[k] for k in ['base','name','ext'])
    rows.extend([format_path(parsed),format_path(parse_path('/home/user/file.txt')),format_path(parse_path('archive.tar'))])
    value='/srv/data/report.csv';rows.extend([dirname(value),basename(value),extname(value),normalize(value),relative('/srv/data',value)])
    return rows

def log_observations():
    levels=['Debug','Info','Warn','Error','Fatal'];formats=['Text','JSON'];name=lambda enum,member:enum+'.'+member
    rows=[*(name('LogLevel',v) for v in levels),*(name('LogFormat',v) for v in formats),levels.index('Debug')==levels.index('Debug'),levels.index('Warn')==levels.index('Error')]
    threshold=levels.index('Warn');rows.extend(levels.index(v)>=threshold for v in ['Debug','Info','Warn','Fatal'])
    threshold=levels.index('Info');stored=['Fatal','Debug'];rows.extend([levels.index('Info')>=threshold,name('LogLevel',stored[0]),len(stored)])
    return rows

def observations():return {'datetime_core_direct':calendar_observations(),'path_string_ops':path_observations(),'log_level_enum_direct':log_observations()}

def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES):raise ValueError('Calendar/path exact identity set changed')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib'}:raise ValueError('Calendar/path input is not delivered')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Calendar/path source/authority changed: '+name)
    rows=[]
    for name,values in sorted(observations().items()):
        source=_PROGRAMS[name]
        if source!='tests/diff/cases/semantics/stdlib/'+name+'.xr' or source not in _INPUTS:raise ValueError('Calendar/path original source identity changed')
        rows.append({'id':'complete_calendar_paths_'+name,'source':source,'original_source':source,'required_target':None,
         'expected':{'exit_code':0,'stdout_utf8':''.join((str(v).lower() if type(v) is bool else str(v))+'\n' for v in values),'stderr_utf8':''},
         'model':'Complete original source; independent civil calendar and POSIX path components plus nominal enum levels. No clock/cwd/host timezone oracle,whole-library/provider/lifetime qualification.',
         'observation_count':len(values)})
    return rows
