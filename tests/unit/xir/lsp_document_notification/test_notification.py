from pathlib import Path
import concurrent.futures,hashlib,json,os,re,subprocess,sys,tempfile,time
image=Path(sys.argv[1]).resolve();stdlib=Path(sys.argv[2]).resolve();mode=sys.argv[3]
env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib)}
def invoke(root,args):
    result=subprocess.run([str(image),(root/'root.xr').as_uri(),*args],env=env,capture_output=True,timeout=20)
    assert not result.stdout.startswith(b'Content-Length:'),result.stdout
    return result
with tempfile.TemporaryDirectory(prefix='xray-notification-文档-',dir=os.getcwd()) as folder:
    root=Path(folder);(root/'xray.toml').write_text('[project]\nname="notification"\nversion="1.0.0"\nmain="root.xr"\n',encoding='utf-8')
    (root/'root.xr').write_text('export fn main()->i64 { return 1 }\n',encoding='utf-8')
    if mode.startswith('--before-'):
        result=invoke(root,[mode]);kind='NUL_URI_closed_prefix' if mode=='--before-close' else 'duplicate_URI_changed_first'
        expected=f'NOTIFICATION_BEFORE {kind}=1 compiler0/0 JSON0/0 app0/0\n'.encode()
        assert result.returncode==1 and not result.stdout and result.stderr.replace(b'\r\n',b'\n')==expected,(result.returncode,result.stdout,result.stderr)
        print('Exact predecessor counterexample observed: '+expected.decode().strip())
    else:
        assert mode in ['--normal','--resources'];result=invoke(root,['--normal'])
        expected=("error: expected '}'\n  --> "+(root/'root.xr').as_posix()+":2:1\n   |\n 2 | \n   | ^\n\nerror: aborting due to 1 previous error\n").encode()
        assert result.returncode==0 and result.stderr.replace(b'\r\n',b'\n')==expected,(result.returncode,result.stdout,result.stderr)
        assert b'Notification cases=55 mutations=3 rejected_no_Source_work=47' in result.stdout,result.stdout
        print(result.stdout.decode().strip());print(result.stderr.decode().replace('\r\n','\n'),end='');measured=[]
        for kind in [1,2]:
            probe=invoke(root,['--fault',str(kind),str(2**64-1)])
            assert probe.returncode==0 and not probe.stderr,(probe.returncode,probe.stdout,probe.stderr)
            match=re.fullmatch(rb'Notification fault kind=(\d+) source=(\d+) app=(\d+) json=(\d+) injected=0 compiler0/0 JSON0/0 app0/0\r?\n',probe.stdout)
            assert match and int(match[1])==kind and int(match[2])>0 and int(match[4])==0,probe.stdout
            measured.append(dict(kind=kind,source=int(match[2]),app=int(match[3]),json=int(match[4])))
        focused=0
        for row in measured:
            for point in [0,row['source']//2,row['source']-1]:
                check=invoke(root,['--fault',str(row['kind']),str(point)])
                assert check.returncode==0 and not check.stderr and b'injected=1 compiler0/0 JSON0/0 app0/0' in check.stdout,(row,point,check.returncode,check.stdout,check.stderr)
                focused+=1
        completed=0
        if mode=='--resources':
            begin=time.monotonic()
            def fault(item):
                kind,point=item
                result=invoke(root,['--fault',str(kind),str(point)])
                assert result.returncode==0 and not result.stderr and b'injected=1 compiler0/0 JSON0/0 app0/0' in result.stdout,(item,result.returncode,result.stdout,result.stderr)
                return 1
            inputs=[(row['kind'],point) for row in measured for point in range(row['source'])]
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                for result in pool.map(fault,inputs):
                    completed+=result
                    assert time.monotonic()-begin<290
            assert completed==sum(row['source'] for row in measured)
        print(json.dumps(dict(mode=mode,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),measured=measured,
            full_Source_FI=mode=='--resources',focused_Source_faults=focused,completed=completed,JSON_decoder_allocations=0,actual_Source_transactions=True,
            actual_diagnostics_queue=True,publish_and_index_spies=True,public_LSP_process=False)))
