from pathlib import Path
import hashlib,json,re,subprocess,sys
exe=Path(sys.argv[1]).resolve();mode=sys.argv[2];assert mode in ('normal','full','before')
identity=hashlib.sha256(exe.read_bytes()).hexdigest();rows=[]
if mode=='before':
    for argument,kind in (('--probe-key','key-bytes'),('--probe-clone','clone-oom'),('--probe-append','append-oom')):
        result=subprocess.run([str(exe),argument],capture_output=True,text=True,encoding='utf-8',timeout=25)
        assert result.returncode==1 and result.stdout.count('BUG ')==1 and f'BUG {kind} ' in result.stdout,(kind,result.returncode,result.stdout,result.stderr)
        if kind=='key-bytes':assert 'expected=3 actual=1' in result.stdout,result.stdout
        if kind=='clone-oom':
            match=re.search(r'partial=1 leaked=(\d+)',result.stdout);assert match and int(match[1])>0 and 'raw_cleanup_only=true' in result.stdout,result.stdout
        if kind=='append-oom':assert 'count=1 null_key=1' in result.stdout,result.stdout
        rows.append(dict(kind=kind,exit=result.returncode,evidence=result.stdout.strip()))
else:
    result=subprocess.run([str(exe),*(['--normal'] if mode=='normal' else [])],capture_output=True,text=True,encoding='utf-8',timeout=25)
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
    rows=[json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    assert len(rows)==8 and [r['case'] for r in rows]==list(range(8)),rows
    assert all(r['sites']>0 and r['full']==(mode=='full') and r['producer_dead'] and not r['physical_live'] and not r['physical_bytes'] for r in rows),rows
    match=re.search(r'Clone cases8 FI_checked=(\d+) full=(\d+) parser_depth=256 physical0/0 PASS',result.stdout);assert match,result.stdout
    assert int(match[1])==(sum(r['sites'] for r in rows) if mode=='full' else 0) and int(match[2])==(mode=='full'),result.stdout
assert hashlib.sha256(exe.read_bytes()).hexdigest()==identity
print(json.dumps(dict(mode=mode,image_sha256=identity,cases=rows,full_clone_FI=mode=='full',old_controls_qualified=False)))
