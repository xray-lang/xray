from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3]);mode=sys.argv[3]
assert mode in ('--normal','--resources')
env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib.resolve())};maximum=str((1<<64)-1)
stats_re=re.compile(rb'NAV_WORKER mode=(\d+) source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) producer_dead=1 physical0/0 JSON0/0 app0/0')
def unpack(data):
    values=[]
    while data:
        head,separator,rest=data.partition(b'\r\n\r\n');assert separator and re.fullmatch(rb'Content-Length: [0-9]+',head)
        size=int(head.split(b': ')[1]);assert 0<size<=len(rest);values.append(json.loads(rest[:size]));data=rest[size:]
    return values
def check_result(value,method,library):
    assert value['jsonrpc']=='2.0' and value['id']=='source\0λ' and 'error' not in value,value
    if method==0:
        assert value['result']['uri']==library and value['result']['range']=={'start':{'line':0,'character':19},'end':{'line':0,'character':24}},value
    elif method<3:
        assert len(value['result'])==1,value
        if method==1:assert value['result'][0]['uri'].endswith('/root.xr'),value
        else:assert value['result'][0]['kind']==2,value
    else:assert 'value' in value['result']['contents']['value'],value
with tempfile.TemporaryDirectory(prefix='navigation-worker-😀-#-',dir=os.getcwd()) as folder:
    root=Path(folder);(root/'xray.toml').write_text('[project]\nname="navigation-worker"\nmain="root.xr"\n',encoding='utf-8')
    uri,library=[(root/f'{name}.xr').as_uri() for name in ('root','lib')]
    def invoke(method,*args):
        return subprocess.run([str(exe.resolve()),uri,'--nav-admission',library,str(method),*map(str,args)],
            input=b'',stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,timeout=290 if mode=='--resources' else 20)
    measured=[];rows=[]
    for method in range(4):
        result=invoke(method,'--worker',maximum,maximum);assert result.returncode==0,(method,result.returncode,result.stderr[-3500:])
        match=stats_re.search(result.stderr);assert match and int(match[1])==method and all(int(match[n])>0 for n in (2,3,7,8,9)),result.stderr[-3500:]
        ledger=re.search(rb'NAV_WORKER_LEDGER mode=(\d+) created=1 capture_work=(\d+) final_work=(\d+) same_original=1',result.stderr)
        assert ledger and int(ledger[1])==method and int(ledger[3])==int(match[9]) and int(ledger[3])>int(ledger[2]),result.stderr[-3500:]
        assert all(int(match[n])==0 for n in (4,5,6)),result.stderr[-3500:]
        messages=unpack(result.stdout);assert len(messages)==1,messages;check_result(messages[0],method,library)
        measured.append(dict(method=method,source=int(match[2]),json=int(match[3]),allocated=int(match[7]),peak=int(match[8]),work=int(match[9])))
    if mode=='--normal':
        for m in measured:
            result=invoke(m['method'],'--worker',maximum,m['json']-1)
            assert result.returncode==0 and result.stdout==b'',(m,result.returncode,result.stdout,result.stderr[-3000:])
            match=stats_re.search(result.stderr);assert match and int(match[5])==1 and int(match[6])==1,result.stderr[-3000:]
            rows.append(dict(method=m['method'],serialization_prefix=m['json']-1,producer_dead=True,three_domains_zero=True))
    else:
        chunks=[(m['method'],domain,start,min(64,m[domain]-start),m[domain]) for m in measured for domain in ('source','json') for start in range(0,m[domain],64)]
        def run_chunk(chunk):
            method,domain,start,count,total=chunk;result=invoke(method,'--worker-chunk',domain,start,count,total)
            assert result.returncode==0,(chunk,result.returncode,result.stderr[-3500:])
            marker=f'NAV_WORKER_CHUNK mode={method} domain={domain} start={start} count={count} total={total} checked={count} producer_dead=1 physical0/0 JSON0/0 app0/0'.encode()
            assert marker in result.stderr,(chunk,result.stderr[-3500:])
            for value in unpack(result.stdout):assert value['id']=='source\0λ' and value['error']['code']==-32603 and 'result' not in value,value
            return dict(method=method,domain=domain,start=start,count=count)
        with ThreadPoolExecutor(max_workers=8) as pool:rows=[future.result() for future in as_completed([pool.submit(run_chunk,chunk) for chunk in chunks])]
        for m in measured:
            result=invoke(m['method'],'--worker-axes',m['allocated'],m['peak'],m['work'])
            assert result.returncode==0 and f"NAV_WORKER_AXES mode={m['method']} checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0".encode() in result.stderr,(m,result.returncode,result.stderr[-3500:])
            messages=unpack(result.stdout);assert sum('result' in value for value in messages)==6,messages
            for value in messages:
                if 'result' in value:check_result(value,m['method'],library)
                else:assert value['error']['code']==-32603,value
            rows.append(dict(method=m['method'],resource_axes=9))
    print(json.dumps(dict(image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),mode=mode,measured=measured,
        full_FI=mode=='--resources',source_total=sum(m['source'] for m in measured),json_total=sum(m['json'] for m in measured),rows=rows),ensure_ascii=False))
