from pathlib import Path
import argparse, json, os, subprocess
p=argparse.ArgumentParser();p.add_argument('--root',type=Path);p.add_argument('--driver',type=Path);p.add_argument('--injected',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,XRAY_STDLIB_PATH=str(a.root/'stdlib'),NO_COLOR='1')
records=[];failures=[]
def write(name,text):
 path=a.output/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(text.encode() if isinstance(text,str) else text);return path

def run(name,args,code,contains=(),cwd=None,injected=False):
 r=subprocess.run([str(a.injected if injected else a.driver),*map(str,args)],cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=90)
 (a.output/(name+'.stdout')).write_bytes(r.stdout);(a.output/(name+'.stderr')).write_bytes(r.stderr)
 records.append({'name':name,'args':list(map(str,args)),'returncode':r.returncode,'expected':code})
 (a.output/'results.json').write_text(json.dumps(records,indent=2)+'\n')
 if r.returncode!=code:failures.append((name,r.returncode,code,r.stdout,r.stderr))
 for needle in contains:
  if needle.encode() not in r.stdout+r.stderr:failures.append((name,needle,r.stdout,r.stderr))
 return r
ok=write('ok.xr','const n = 42\n')
run('semantic',[ok],0)
run('syntax',['--syntax-only',ok],0)
missing=write('missing.xr','import {value} from "./absent"\n')
run('syntax-missing-import',['--syntax-only',missing],0)
run('semantic-missing-import',[missing],1,('status=not-found', 'module not found'))
bad=write('bad.xr','var value = missing_symbol\n')
run('syntax-unresolved',['--syntax-only',bad],0)
run('semantic-location',[bad],1,('bad.xr:1:13:',))
malformed=write('malformed.xr','var =\n')

write('modules/library.xr','export fn answer() -> i64 { return 42 }\n')
module=write('modules/main.xr','import {answer} from "./library"\nprint(answer())\n')
r=run('module-success',[module],0);assert not r.stdout
write('modules/library.xr','const bad = missing_symbol\nexport fn answer() -> i64 { return 42 }\n')
run('module-failure',[module],1,('library.xr:1:13:',))
run('module-syntax-only',['--syntax-only',module],0)
run('syntax-error',['--syntax-only',malformed],1)
run('nul',['--syntax-only',write('nul.xr',b'const n=1\x00const m=2')],1,('NUL',))
run('invalid-utf8',['--syntax-only',write('utf8.xr',b'const \xff=1')],1,('source must be valid UTF-8',))
run('multiple',['--verbose',bad,ok],1,('2 files checked','ok '+str(ok)))
quiet=run('quiet',['--quiet','--verbose',bad,ok],1);assert not quiet.stdout
run('strict-rejected',['--strict',ok],2,('unknown',))
run('short-strict-rejected',['-s',ok],2,('unknown',))
run('missing-path',[a.output/'not-here.xr'],1,('I/O status=2',))
write('tree/a.xr','const a = 1\n');write('tree/nested/b.xr','const b = 2\n');write('tree/skip.txt','invalid')
run('directory',[a.output/'tree'],0,('2 files checked',))
run('default-directory',[],0,('2 files checked',),cwd=a.output/'tree')
write('tree/nested/b.xr','var value = missing_symbol\n')
run('directory-semantic',[a.output/'tree'],1,('b.xr:1:13:',))
run('directory-syntax',['--syntax-only',a.output/'tree'],0,('2 files checked',))
unicode=write('中文/源.xr','const 中文 = "合法"\n')
run('unicode',[unicode],0)
longdir=a.output/'long'
for i in range(12):longdir=longdir/('segment'+str(i)+'_'+'x'*80)
longfile=write(str((longdir/'source.xr').relative_to(a.output)),'const n = 1\n')
assert len(str(longfile))>1024
run('long-path',['--syntax-only',longfile],0)
# A real sharing denial must remain I/O, not a guessed syntax error.
import ctypes
kernel=ctypes.WinDLL('kernel32',use_last_error=True)
kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p]
kernel.CreateFileW.restype=ctypes.c_void_p
kernel.CloseHandle.argtypes=[ctypes.c_void_p]
handle=kernel.CreateFileW(str(ok),0x80000000,0,None,3,0x80,None)
assert handle not in (None,ctypes.c_void_p(-1).value)
try:run('sharing-denied',['--syntax-only',ok],1,('I/O status=8',))
finally:assert kernel.CloseHandle(handle)

# A directory junction back to the tree must be rejected, never descended into.
link=a.output/'tree'/'loop'
r=subprocess.run(['cmd','/c','mklink','/J',str(link),str((a.output/'tree').resolve())],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
assert r.returncode==0,(r.stdout,r.stderr)
try:run('junction',['--syntax-only',a.output/'tree'],1,('I/O status=4',))
finally:os.rmdir(link)
run('syntax-faults',['--syntax-only',ok],0,('physical zero PASS',),injected=True)
run('directory-faults',['--syntax-only',a.output/'tree'],0,('physical zero PASS',),injected=True)
run('semantic-faults',[ok],0,('physical zero PASS',),injected=True)
assert not failures,failures
print(f'{len(records)} CHECK source/syntax/typed failures cases PASS')
