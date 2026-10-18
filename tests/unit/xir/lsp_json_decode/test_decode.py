from pathlib import Path
import hashlib,json,re,subprocess,sys
exe=Path(sys.argv[1]).resolve();mode=sys.argv[2];assert mode in ('--normal','--resources','--before')
identity=hashlib.sha256(exe.read_bytes()).hexdigest()
if mode=='--before':
    controls=[('--array-before','BUG decode-array partial=1 count=0 leaked=1 raw_cleanup_only=1'),('--status-before','BUG decode-status OOM_and_invalid_NULL=1 physical0/0'),('--unicode-before','BUG decode-unicode raw_invalid_accepted=2 physical0/0')]
    for argument,expected in controls:
        r=subprocess.run([str(exe),argument],capture_output=True,text=True,encoding='utf-8',timeout=25)
        assert r.returncode==1 and r.stdout.strip()==expected and not r.stderr,(r.returncode,r.stdout,r.stderr)
        print(r.stdout,end='')
else:
    r=subprocess.run([str(exe),mode],capture_output=True,text=True,encoding='utf-8',timeout=25)
    print(r.stdout,end='');print(r.stderr,end='',file=sys.stderr);assert r.returncode==0,(r.returncode,r.stdout,r.stderr)
    rows=[json.loads(line) for line in r.stdout.splitlines() if line.startswith('{')]
    assert len(rows)==11 and [m['case'] for m in rows]==list(range(11))
    assert all(m['sites']>0 and m['checked']==(m['sites'] if mode=='--resources' else 1) and m['producer_dead'] and m['physical0_0'] for m in rows)
    assert 'Decode invalid=74 grammar23 control32 UTF8_value_and_key18 depth1 physical0/0' in r.stdout
    assert 'Decode bad_arguments=4 untouched_output=1 allocation0' in r.stdout
    assert f'full_FI={int(mode=="--resources")} physical0/0' in r.stdout
assert hashlib.sha256(exe.read_bytes()).hexdigest()==identity
print(json.dumps(dict(mode=mode,image_sha256=identity,full_FI=mode=='--resources',public_server=False)))
