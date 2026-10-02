from pathlib import Path
import argparse, ctypes, json, os, subprocess
p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--driver',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--publication',type=Path,required=True)
p.add_argument('--faults',type=Path,required=True);p.add_argument('--fault-driver',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,XRAY_STDLIB_PATH=str(a.root/'stdlib'),NO_COLOR='1');records=[]
def run(name,program,args,expected,environment=None):
 r=subprocess.run([str(program),*map(str,args)],cwd=a.output,env=environment or env,capture_output=True,timeout=90)
 (a.output/(name+'.stdout')).write_bytes(r.stdout);(a.output/(name+'.stderr')).write_bytes(r.stderr)
 records.append(dict(name=name,returncode=r.returncode,expected=expected));(a.output/'results.json').write_text(json.dumps(records,indent=2)+'\n')
 assert r.returncode==expected,(name,r.returncode,r.stdout,r.stderr)
 return r
source=a.output/'main.xr';source.write_text('print("source-owned")\n',encoding='utf-8');output=a.output/'program.c'
run('default',a.driver,['--c-only',source],0);assert (a.output/'app.c').is_file()
run('explicit',a.driver,['--native','--c-only',source,'-o',output],0);original=output.read_bytes()
run('overwrite',a.driver,['--c-only',source,'-o',output],0);assert output.read_bytes()==original
bad=a.output/'bad.xr';bad.write_text('var value = missing_symbol\n',encoding='utf-8')
run('source-rejection',a.driver,['--c-only',bad,'-o',output],1);assert output.read_bytes()==original
run('native-unavailable',a.driver,[source],1)
run('option-not-ignored',a.driver,['--c-only','-O2',source],1)
readonly_path=a.output/'readonly.stdout';readonly_path.write_bytes(b'untouched')
with readonly_path.open('rb') as readonly:
 r=subprocess.run([str(a.driver),'--c-only',str(source),'-o',str(output)],cwd=a.output,env=env,stdout=readonly,stderr=subprocess.PIPE,timeout=90)
(a.output/'stdout-failed.stderr').write_bytes(r.stderr)
assert r.returncode==1 and b'published=1' in r.stderr and output.read_bytes()==original,(r.returncode,r.stderr)
records.append(dict(name='stdout-failed-published',returncode=r.returncode,expected=1))
for suffix in ('.',' '):
 run('trimmed-'+str(ord(suffix)),a.driver,['--c-only',source,'-o',str(output)+suffix],4)
 assert output.read_bytes()==original
folder=a.output/'directory.c';folder.mkdir(exist_ok=True)
run('directory-output',a.driver,['--c-only',source,'-o',folder],1)
run('missing-parent',a.driver,['--c-only',source,'-o',a.output/'missing'/'out.c'],1)
long_dir=a.output/'long-output'
for i in range(20):long_dir=long_dir/('x'*52+str(i))
long_dir.mkdir(parents=True,exist_ok=True)
long_output=long_dir/'输出 空格.c';assert len(str(long_output))>1024
run('long-unicode-output',a.driver,['--c-only',source,'-o',long_output],0);assert long_output.read_bytes()==original
run('relative-dot-output',a.driver,['--c-only',source,'-o','./long-output/../dot.c'],0)
assert (a.output/'dot.c').read_bytes()==original
kernel=ctypes.WinDLL('kernel32',use_last_error=True)
kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
kernel.CreateFileW.restype=ctypes.c_void_p;kernel.CloseHandle.argtypes=[ctypes.c_void_p]
h=kernel.CreateFileW(str(output),0x80000000,1,None,3,0,None);assert h!=ctypes.c_void_p(-1).value
try:run('replace-share-denied',a.driver,['--c-only',source,'-o',output],1)
finally:assert kernel.CloseHandle(h)
assert output.read_bytes()==original
for mode,status,published in [('transient-close',1,1),('published-close',4,1),('unpublished-close',4,0)]:
 output.write_bytes(b'OLD')
 r=run(mode,a.fault_driver,['--c-only',source,'-o',output],status,dict(env,XR_PUBLICATION_TEST_FAILURE=mode))
 assert f'published={published}'.encode() in r.stderr,r.stderr
 assert output.read_bytes()==(original if published else b'OLD')
 if mode=='unpublished-close':
  assert b'cleanup_pending=1' in r.stderr
  leftovers=list(a.output.glob('.xray-*.tmp'));assert len(leftovers)==1,leftovers
  # The test harness removes the explicitly retained terminal artifact after
  # process exit. This is not a claim that the owner completed cleanup.
  for path in leftovers:path.unlink()
 else:assert not list(a.output.glob('.xray-*.tmp'))
run('publication',a.publication,[a.output/'published.bin'],0)
run('publication-faults',a.faults,[a.output/'faults.bin'],0)
assert (a.output/'published.bin').read_bytes()==b'complete\0bytes'
assert (a.output/'faults.bin').read_bytes()==b'complete\0bytes'
assert not list(a.output.rglob('.xray-*.tmp'))
print(f'{len(records)} source command and owned publication cases PASS')
