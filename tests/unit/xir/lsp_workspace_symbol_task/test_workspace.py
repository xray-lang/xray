from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3]);mode=sys.argv[3]
assert mode in ('--normal','--resources','--counterfactual')
env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib.resolve())};maximum=str((1<<64)-1)
stats_re=re.compile(rb'WORKSPACE source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) created=1 producer_dead=1 physical0/0 JSON0/0 app0/0')
def unpack(data):
    values=[]
    while data:
        head,sep,rest=data.partition(b'\r\n\r\n');assert sep and re.fullmatch(rb'Content-Length: [0-9]+',head)
        count=int(head.split(b': ')[1]);assert 0<count<=len(rest);values.append(json.loads(rest[:count]));data=rest[count:]
    return values
def span(a,b):return {'start':{'line':a[0],'character':a[1]},'end':{'line':b[0],'character':b[1]}}
def expected(method,variant):
    assert method==0
    if variant in (9,10):return []
    names=['first','second','Thing','shared','hidden'];kinds=[12,12,5,12,14];lines=[0,1,2,0,1];columns=[19,3,7,10,6];ends=[24,9,12,16,12]
    values=[dict(name=n,kind=kinds[i],location=dict(uri=uri if i<3 else library,range=span((lines[i],columns[i]),(lines[i],ends[i])))) for i,n in enumerate(names)]
    if variant==11:values += [dict(name='a'*60+f'{i:03}',kind=12,location=dict(uri=uri+'-many.xr',range=span((i,3),(i,66)))) for i in range(95)]
    return values
def check(value,method,variant=0):
    assert value==expected(method,variant),(method,variant,value,expected(method,variant))
with tempfile.TemporaryDirectory(prefix='workspace-😀-#-',dir=os.getcwd()) as folder:
    root=Path(folder);uri,library=[(root/f'{name}.xr').as_uri() for name in ('root','lib')]
    def invoke(method,*args):
        r=subprocess.run([str(exe.resolve()),uri,'--workspace-symbol',library,str(method),*map(str,args)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,input=b'',env=env,timeout=290 if mode=='--resources' else 20)
        assert r.returncode==0,(method,args,r.returncode,r.stderr[-7000:])
        assert b'AddressSanitizer' not in r.stderr and b'runtime error:' not in r.stderr
        return r
    if mode=='--counterfactual':
        for method in (0,):
            r=invoke(method,13)
            check(json.loads(r.stdout),method)
            assert b'WORKSPACE_CACHE before=1 index_ledger_work_delta=' in r.stderr
            print(r.stderr.decode('utf-8'))
        print('Exact predecessor handler and store visitor accept duplicate queries and debit the index ledger; physical0/0')
        sys.exit(0)
    rows=[]
    if mode=='--normal':
        for method in (0,):
            for scenario in [*range(13),14,15,18]:
                r=invoke(method,scenario)
                if scenario in (12,15):
                    check(json.loads(r.stdout),method)
                    assert (b'WORKSPACE_CACHE before=0 index_ledger_work_delta=0' if scenario==12 else b'server_poisoned=1 one_shot=1') in r.stderr
                elif scenario==14:
                    assert not r.stdout and b'WORKSPACE_ADMISSION duplicate=1 invalid_shapes=9 original_index_ledger_unchanged=1' in r.stderr
                elif scenario in (4,5,6):assert not r.stdout
                else:
                    messages=unpack(r.stdout);assert len(messages)==1 and messages[0]['id']=='source\0λ',messages
                    if scenario in (1,2,8):assert messages[0]['error']['code']==-32800,messages
                    elif scenario==3:assert messages[0]['error']['code']==-32803,messages
                    elif scenario in (16,17):assert messages[0]['error']['code']==-32603,messages
                    else:check(messages[0]['result'],method,scenario)
                if scenario==2:
                    m=re.search(fr'INTERIOR mode=22 trigger=4096 calls=(\d+)'.encode(),r.stderr);assert m and 4096<=int(m[1])<=4160,r.stderr
                print(json.dumps(dict(method=method,case=scenario,stderr=r.stderr.decode('utf-8'),stdout=r.stdout.decode('utf-8')),ensure_ascii=False))
                rows.append(dict(method=method,scenario=scenario,passed=True))
    measured=[]
    for method in (0,):
        for variant in (0,9,10,11):
            r=invoke(method,'--resource',maximum,maximum,variant);m=stats_re.search(r.stderr)
            assert m and all(int(m[n])>0 for n in (1,2,6,7,8)) and all(int(m[n])==0 for n in (3,4,5)),r.stderr
            values=unpack(r.stdout);assert len(values)==1 and 'result' in values[0],values;check(values[0]['result'],method,variant)
            measured.append(dict(method=method,variant=variant,source=int(m[1]),json=int(m[2]),allocated=int(m[6]),peak=int(m[7]),work=int(m[8])))
    if mode=='--normal':
        for method in (0,):
            for source,js in ((0,maximum),(maximum,0)):
                r=invoke(method,'--resource',source,js,0);assert b'_hit=1' in r.stderr
                for value in unpack(r.stdout):assert value['error']['code']==-32603
    if mode=='--resources':
        chunks=[(m['method'],m['variant'],domain,start,min(64,m[domain]-start),m[domain]) for m in measured for domain in ('source','json') for start in range(0,m[domain],64)]
        def run_chunk(chunk):
            method,variant,domain,start,count,total=chunk;r=invoke(method,'--chunk',domain,start,count,total,variant)
            assert f'WORKSPACE_CHUNK domain={domain} start={start} count={count} total={total} checked={count} physical0/0 JSON0/0 app0/0'.encode() in r.stderr
            for value in unpack(r.stdout):assert value['error']['code']==-32603 and 'result' not in value,value
            return dict(method=method,variant=variant,domain=domain,start=start,count=count)
        with ThreadPoolExecutor(max_workers=8) as pool:rows.extend(f.result() for f in as_completed([pool.submit(run_chunk,c) for c in chunks]))
        for m in measured:
            r=invoke(m['method'],'--axes',m['allocated'],m['peak'],m['work'],m['variant'])
            assert b'WORKSPACE_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0' in r.stderr
            messages=unpack(r.stdout);assert sum('result' in v for v in messages)==6,messages
            for value in messages:
                if 'result' in value:check(value['result'],m['method'],m['variant'])
                else:assert value['error']['code']==-32603
    print(json.dumps(dict(mode=mode,image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),measured=measured,source_total=sum(m['source'] for m in measured),json_total=sum(m['json'] for m in measured),full_FI=mode=='--resources',rows=rows)))
