"""Verify actual CLI source-byte diagnostics with independent location literals."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile

p=argparse.ArgumentParser()
p.add_argument('--root',type=Path,required=True)
p.add_argument('--cli',type=Path,required=True)
p.add_argument('--record',type=Path,required=True)
a=p.parse_args()
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
binary_before=digest(a.cli)
environment=dict(os.environ,XRAY_STDLIB=str(a.root/'stdlib'))
identity=json.loads(subprocess.check_output([a.cli,'--version','--json'],cwd=a.root))
head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=a.root,text=True).strip()
assert identity['commit']==head and not identity['dirty']
fixture=a.root/'tests/fixtures/public_source_nul'
inputs={name:(fixture/name).read_bytes() for name in ['entry.xr','坏 库.xr']}
assert b'\0' in inputs['坏 库.xr']
rows=[]
with tempfile.TemporaryDirectory(prefix='source-input-中文 ',dir=a.record.parent) as temporary:
    directory=Path(temporary)
    for name,data in inputs.items():(directory/name).write_bytes(data)
    bad=directory/'坏 库.xr'
    entry=directory/'entry.xr'
    expected=f'{bad.as_posix()}:2:1: error: source text contains an embedded NUL\n'.encode()
    cases=[('run-root',['run',str(bad)],None),('run-import',['run',str(entry)],None),
           ('check-root',['check',str(bad)],None),('check-import',['check',str(entry)],None),
           ('syntax-root',['check','--syntax-only',str(bad)],None),
           ('repl-load-root',['repl','--module-id','input-cli'],
            f'.load {bad}\nprint(42)\n.exit\n'.encode()),
           ('repl-load-import',['repl','--module-id','input-cli'],
            f'.load {entry}\nprint(42)\n.exit\n'.encode())]
    for name,args,data in cases:
        result=subprocess.run([str(a.cli),*args],cwd=a.root,input=data,env=environment,capture_output=True,timeout=20)
        # Canonical Windows paths can use either separator; diagnostic bytes otherwise stay exact.
        stderr=result.stderr.replace(b'\\',b'/')
        interactive=name.startswith('repl-')
        good=(result.returncode==(0 if interactive else 1) and stderr==expected and
              (result.stdout.count(b'42\n')==1 if interactive else result.stdout==b''))
        rows.append({'name':name,'arguments':args,'stdin_hex':data.hex() if data is not None else None,
                     'exit':result.returncode,'stdout_hex':result.stdout.hex(),'stderr_hex':result.stderr.hex(),
                     'expected_stderr_hex':expected.hex(),'status':'PASS' if good else 'FAIL'})
        print(name,'PASS' if good else 'FAIL')
assert all((fixture/name).read_bytes()==data for name,data in inputs.items())
assert digest(a.cli)==binary_before
record={'source_commit':head,'identity':identity,'binary_sha256':binary_before,'cases':rows,
        'inputs_sha256':{name:hashlib.sha256(data).hexdigest() for name,data in inputs.items()},
        'inputs_and_binary_unchanged':True,'passed':sum(x['status']=='PASS' for x in rows),'failed':sum(x['status']=='FAIL' for x in rows)}
a.record.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf8',newline='\n')
raise SystemExit(1 if record['failed'] else 0)
