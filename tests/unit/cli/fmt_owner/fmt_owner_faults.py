"""Actual command with ledger/Win32 fault injection, keeping independent old/new bytes."""
import argparse,json,os,pathlib,re,subprocess
p=argparse.ArgumentParser();p.add_argument('--driver',required=True);p.add_argument('--output',required=True);a=p.parse_args()
base=pathlib.Path(a.output).resolve();base.mkdir(parents=True,exist_ok=True)
for attempt in range(10000):
    root=base/f'run-{attempt:04d}'
    try:root.mkdir();break
    except FileExistsError:continue
else:raise RuntimeError('fixture run slots exhausted')
(base/'latest.json').write_text(json.dumps({'directory':str(root)}),encoding='utf-8')
print('evidence directory:',root,flush=True);file=root/'target.xr';old=b'const x=1\n';new=b'const x = 1\n';records=[]
def run(name,controls=None,expected=None,bytes_after=old,paths=None):
    file.write_bytes(old);env=os.environ.copy();env.update(controls or {})
    r=subprocess.run([str(pathlib.Path(a.driver).resolve()),*map(str,paths or [file])],env=env,capture_output=True)
    (root/(name+'.out')).write_bytes(r.stdout);(root/(name+'.err')).write_bytes(r.stderr)
    record=dict(name=name,controls=controls,exit=r.returncode,old_or_new='old' if file.read_bytes()==old else 'new' if file.read_bytes()==new else 'OTHER')
    records.append(record);(root/'results.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
    assert r.returncode==(0 if expected is None else expected),(name,r.returncode,r.stderr)
    assert file.read_bytes()==bytes_after,(name,record)
    match=re.search(rb'FMT_STATS allocations=(\d+) io=(\d+) work=(\d+) allocated=(\d+) peak=(\d+) ledgers=(\d+) live=(\d+)',r.stderr)
    if match:
        stats=tuple(map(int,match.groups()));assert stats[-2:]==(1,0),(name,stats)
    else:assert expected==4 and b'terminal publication cleanup failure' in r.stderr
    return r,stats if match else None
r,baseline=run('baseline',{'XR_FMT_TEST_RECORD_WORK':'1'},bytes_after=new)
for i in range(baseline[0]):run(f'oom-{i}',{'XR_FMT_TEST_ALLOC':str(i)},4)
for error,exitcode in ((5,1),(8,4),(14,4)):
    for i in range(baseline[1]):run(f'io-{error}-{i}',{'XR_FMT_TEST_IO':str(i),'XR_FMT_TEST_OS_ERROR':str(error)},exitcode)
for key,index in (('WORK',2),('ALLOCATED',3),('LIVE',4)):
    run('exact-'+key,{'XR_FMT_TEST_'+key:str(baseline[index])},bytes_after=new)
    run('minus1-'+key,{'XR_FMT_TEST_'+key:str(baseline[index]-1)},1)
points=sorted(set(int(v) for v in re.search(rb'FMT_WORK([^\r\n]+)',r.stderr).group(1).split()))
for i,point in enumerate(points):run(f'work-{i}',{'XR_FMT_TEST_WORK':str(point-1)},1)
r,_=run('output-failure',{'XR_FMT_TEST_MODE':'output-failure'},1,new);assert b'output failure published=1' in r.stderr
run('transient-cleanup',{'XR_FMT_TEST_MODE':'transient-close'},1,new)
run('published-terminal',{'XR_FMT_TEST_MODE':'published-close'},4,new)
run('unpublished-terminal',{'XR_FMT_TEST_MODE':'unpublished-close'},4,old)
# The killed child retained its owner: preserve temporary artifacts and name them.
residuals=[str(p.name) for p in root.iterdir() if p.name.startswith('.tmp-') or p.name.startswith('.xray-')]
second=root/'second.xr';second.write_bytes(old)
run('cumulative-two-files',{'XR_FMT_TEST_WORK':str(baseline[2]+64)},1,new,[file,second]);assert second.read_bytes()==old
for mode in ('dir-open','dir-next'):
    run(mode,{'XR_FMT_TEST_MODE':mode},1,old,[root])
(root/'summary.json').write_text(json.dumps(dict(baseline=baseline,explicit_work_submissions=len(points),cases=len(records),terminal_disk_residuals=residuals),indent=2),encoding='utf-8')
print(f'PASS {len(records)} cases; malloc={baseline[0]} Win32={baseline[1]}x3 work={len(points)} exact/minus1/cumulative/terminal; live=0 on all returning calls')
