"""Real native child recipes omit inherited compiler flags and include variables."""
from pathlib import Path
import argparse,json,os,subprocess,tempfile,threading,time,sys
sys.path.insert(0,str(Path(__file__).resolve().parents[4]/"scripts"))
from xray_process import run_child
import psutil
p=argparse.ArgumentParser();p.add_argument('--cold',action='store_true');p.add_argument('--binary',type=Path,required=True);p.add_argument('--root',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=True);out=Path(tempfile.mkdtemp(prefix='run-',dir=a.out));env=dict(os.environ,XRAY_STDLIB_PATH=str(a.root/'stdlib'),CL='/invalid-cl-marker',_CL_='/invalid-cl-marker',LINK='/invalid-link-marker',_LINK_='/invalid-link-marker',INCLUDE='X:/not-an-include',LIB='X:/not-a-library')
if a.cold:
 for key in list(env):
  if key.upper()=='VCTOOLSINSTALLDIR':env.pop(key)
exe=out/'program.exe';observed={};stop=threading.Event()
def canonical(value):return os.path.normcase(os.path.normpath(value))
def command(label,argv,cwd):
 with (out/(label+'.stdout')).open('xb') as stdout,(out/(label+'.stderr')).open('xb') as stderr:
  code=run_child(argv,env=env,cwd=cwd,stdout=stdout,stderr=stderr,timeout=30)
 return subprocess.CompletedProcess(argv,code,(out/(label+'.stdout')).read_bytes(),(out/(label+'.stderr')).read_bytes())
def observe():
 while not stop.wait(.01):
  try:children=psutil.Process(os.getpid()).children(recursive=True)
  except psutil.Error:continue
  for child in children:
   try:
    name=child.name().lower()
    if name not in ['cl.exe','link.exe','vswhere.exe','cmd.exe']:continue
    if child.pid in observed:continue
    row=dict(pid=child.pid,name=name)
    if name in ['cl.exe','link.exe']:
     values={k.upper():v for k,v in child.environ().items()};argv=child.cmdline();cwd=Path(child.cwd())
     row['environmentKeys']=sorted(k for k in values if k in {'SYSTEMROOT','TEMP','TMP'});row['environmentCount']=len(values);row['onlyExplicitThree']=set(values)=={'SYSTEMROOT','TEMP','TMP'} and len(values)==3
     row['argv']=argv;row['cwd']=str(cwd)
     if name=='cl.exe':
      destination=next((x[3:] for x in argv if x.startswith('/Fo')),None)
      fixed=['/nologo','/std:c11','/utf-8','/experimental:c11atomics','/MD','/O2','/W4','/WX','/DNDEBUG','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN','/D_CRT_SECURE_NO_WARNINGS','/X']
      row['strictRecipe']=argv[1:14]==fixed and len(argv) in [26,27] and argv[21]=='/c' and argv[24]=='/sourceDependencies' and all(x.startswith('/I') for x in argv[14:21])
     else:
      destination=next((x[5:] for x in argv if x.startswith('/out:')),None)
      row['strictRecipe']=argv[1:5]==['/nologo','/incremental:no','/NODEFAULTLIB','/verbose:lib'] and len(argv)==19
     expected=Path(destination).parent if destination else Path('X:/invalid')
     system=next(v for k,v in env.items() if k.upper()=='SYSTEMROOT')
     row['exactValues']=row['onlyExplicitThree'] and canonical(values['SYSTEMROOT'])==canonical(system) and canonical(values['TEMP'])==canonical(str(expected)) and canonical(values['TMP'])==canonical(str(expected))
     row['exactWorkspace']=expected.name=='output' and canonical(str(cwd))==canonical(str(expected.parent/'input'))
    observed[child.pid]=row
   except psutil.Error:pass
thread=threading.Thread(target=observe);thread.start()
try:
 build=command('build',[str(a.binary),'build',str(a.root/'demos/01-basics/hello.xr'),'-o',str(exe)],a.root)
finally:stop.set();thread.join()
(out/'observed.json').write_text(json.dumps(list(observed.values()),indent=2)+'\n',encoding='utf-8')
assert build.returncode==0,(build.returncode,build.stderr)
compilers=[x for x in observed.values() if x['name'] in ['cl.exe','link.exe']]
assert compilers and any(x['name']=='cl.exe' for x in compilers) and all(x['onlyExplicitThree'] and x['exactValues'] and x['exactWorkspace'] and x['strictRecipe'] for x in compilers),observed
assert any(x['name']=='cmd.exe' for x in observed.values())==a.cold,observed
run=command('run',[str(exe)],out)
cases=json.loads((a.root/'tests/smoke/cases.json').read_text())['cases'];case=next(x for x in cases if x['mode']=='build-run' and x['path']=='demos/01-basics/hello.xr')
assert run.returncode==0 and run.stdout==(a.root/case['stdout']).read_bytes(),(run.returncode,run.stdout,run.stderr)
print(('cold' if a.cold else 'ready')+' original complete hello native; poisoned inherited markers omitted from exact3-key child recipe PASS')
