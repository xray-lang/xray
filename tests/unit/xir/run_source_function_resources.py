from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import subprocess
import sys

exe=Path(sys.argv[1]).resolve()
def run(*args):
    result=subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True,timeout=290)
    if result.returncode:
        print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
        raise RuntimeError(f'actual Source capacity child failed: {args}, exit={result.returncode}')
    return result.stdout

baseline=json.loads(run('baseline').splitlines()[-1])
assert baseline['sites']>0 and baseline['functions']==15
assert baseline['physical_live']==baseline['physical_bytes']==0
run('axes')
if len(sys.argv)==3 and sys.argv[2]=='--normal':
    print(json.dumps({'baseline':baseline,'all_axes':True,'full':False}))
else:
    assert len(sys.argv)==2
    chunks=[(start,min(64,baseline['sites']-start)) for start in range(0,baseline['sites'],64)]
    def sweep(chunk):
        output=run('sweep',*chunk)
        assert f'total={baseline["sites"]}' in output and 'physical0/0' in output
        return chunk[1]
    with ThreadPoolExecutor(max_workers=8) as pool:
        checked=sum(pool.map(sweep,chunks))
    assert checked==baseline['sites']
    print(json.dumps({'baseline':baseline,'all_axes':True,'checked':checked,'workers':8,'full':True}))
