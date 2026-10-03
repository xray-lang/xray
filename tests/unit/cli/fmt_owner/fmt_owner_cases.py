"""Real command parser/formatter/publication integration; byte oracles are independent."""
import argparse, ctypes, json, os, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--driver',required=True);p.add_argument('--output',required=True);p.add_argument('--fault-driver');a=p.parse_args()
base=pathlib.Path(a.output).resolve();base.mkdir(parents=True,exist_ok=True)
for attempt in range(10000):
    root=base/f'run-{attempt:04d}'
    try:root.mkdir();break
    except FileExistsError:continue
else:raise RuntimeError('fixture run slots exhausted')
(base/'latest.json').write_text(json.dumps({'directory':str(root)}),encoding='utf-8')
print('evidence directory:',root,flush=True)
records=[]
def run(name,args,expected=0,cwd=None,env=None):
    command=[str(pathlib.Path(a.driver).resolve()),*map(str,args)]
    result=subprocess.run(command,cwd=cwd or root,capture_output=True,env=env)
    (root/(name+'.stdout')).write_bytes(result.stdout);(root/(name+'.stderr')).write_bytes(result.stderr)
    records.append(dict(name=name,command=command,exit=result.returncode,expected=expected))
    (root/'results.json').write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
    assert result.returncode==expected,(name,result.returncode,result.stderr.decode('utf-8','replace'))
    return result
def fixture(path,data):
    path=root/path;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);return path
old=b'const x=1\n';formatted=b'const x = 1\n'
f=fixture(pathlib.Path('one.xr'),old)
r=run('check',[f,'--check'],1);assert f.read_bytes()==old and b'Needs formatting:' in r.stdout
r=run('write',[f,'--verbose']);assert f.read_bytes()==formatted and b'Formatted:' in r.stdout
r=run('unchanged',[f,'--check','--verbose']);assert b'Unchanged:' in r.stdout
run('usage',[f,'--indent=17'],2)
run('line-usage',[f,'--line-length=10'],2)
run('unknown',[f,'--made-up'],2)
run('missing',[root/'missing.xr'],1)
empty=fixture(pathlib.Path('empty.xr'),b'');run('empty',[empty]);assert empty.read_bytes()==b''
raw=fixture(pathlib.Path('raw.xr'),b'const x=1\0\n');run('raw-nul',[raw],1);assert raw.read_bytes()==b'const x=1\0\n'
escaped=fixture(pathlib.Path('escaped.xr'),b'const x="a\\0b"\n');run('escaped-nul',[escaped]);run('escaped-idempotent',[escaped,'--check'])
u=fixture(pathlib.Path('中文')/'空 格.xr',old);run('unicode',[u]);assert u.read_bytes()==formatted
other=fixture(pathlib.Path('explicit.txt'),old);run('explicit-extension',[other]);assert other.read_bytes()==formatted
batch=root/'batch';batch.mkdir(exist_ok=True)
for skip in ('.hidden','node_modules','build','build-asan','build-release'):
    fixture(pathlib.Path('batch')/skip/'skipped.xr',old)
fixture(pathlib.Path('batch')/'ignored.txt',old)
child=fixture(pathlib.Path('batch')/'nested'/'real.xr',old)
run('implicit-cwd',[],cwd=batch);assert child.read_bytes()==formatted
for skip in ('.hidden','node_modules','build','build-asan','build-release'):assert (batch/skip/'skipped.xr').read_bytes()==old
assert (batch/'ignored.txt').read_bytes()==old
bad=fixture(pathlib.Path('batch')/'nested'/'bad.xr',b'const broken =\n');run('recursive-syntax',[batch],1);assert bad.read_bytes()==b'const broken =\n'
long=pathlib.Path('long')
for i in range(12):long/=f'segment{i}_'+('a'*80)
f=fixture(long/'source.xr',old);assert len(str(f))>1024;run('long-path',[f]);assert f.read_bytes()==formatted
options=['--tabs','--indent=2','--line-length=80','--align-branch-arrows','--no-align-branch-arrows','--align-enum','--align-fields','--align-comments','--wrap','--no-trailing-comma']
for i,option in enumerate(options):
    f=fixture(pathlib.Path('options')/f'{i}.xr',b'fn value(){return 1}\n');run(f'option-{i}',[f,option]);run(f'option-check-{i}',[f,option,'--check'])
if os.name=='nt':
    kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.CreateFileW.restype=ctypes.c_void_p
    kernel.CreateFileW.argtypes=[ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p]
    kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    f=fixture(pathlib.Path('locked.xr'),old);h=kernel.CreateFileW(str(f),0x80000000,0,None,3,0,None);assert h!=ctypes.c_void_p(-1).value
    try:run('sharing',[f],1)
    finally:assert kernel.CloseHandle(h)
    assert f.read_bytes()==old
    junction=root/'junction';target=root/'junction-target';target.mkdir(exist_ok=True)
    if not junction.exists():subprocess.run(['cmd','/c','mklink','/J',str(junction),str(target)],check=True,capture_output=True)
    run('junction',[junction],4)
print(f'fmt functional cases={len(records)} PASS; each publication checked by independent bytes')
