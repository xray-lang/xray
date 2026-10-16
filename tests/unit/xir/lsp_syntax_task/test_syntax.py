from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3]);mode=sys.argv[3]
assert mode in ('--normal','--resources','--counterfactual')
env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib.resolve())};maximum=str((1<<64)-1)
stats_re=re.compile(rb'SYNTAX source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) created=1 producer_dead=1 physical0/0 JSON0/0 app0/0')
def unpack(data):
    values=[]
    while data:
        head,sep,rest=data.partition(b'\r\n\r\n');assert sep and re.fullmatch(rb'Content-Length: [0-9]+',head)
        count=int(head.split(b': ')[1]);assert 0<count<=len(rest);values.append(json.loads(rest[:count]));data=rest[count:]
    return values
def span(a,b):return {'start':{'line':a[0],'character':a[1]},'end':{'line':b[0],'character':b[1]}}
def selection_expected(variant):
    if variant==10:return [{'range':span((0,0),(0,0))}]
    shift=int(variant==11);document={'range':span((0,0),(2+shift,0))}
    first={'range':span((shift,0),(shift,7)),'parent':document}
    second={'range':span((1+shift,2),(1+shift,7)),'parent':{'range':span((1+shift,0),(1+shift,7)),'parent':document}}
    last={'range':span((2+shift,0),(2+shift,0)),'parent':document}
    return [first,second,last]
def symbols_expected(variant):
    if variant==10:return []
    shift=int(variant==11)
    names=['before','Box','value','answer','Tone','Red','Blue'];kinds=[12,5,8,6,10,22,22]
    rows=[0,3,4,5,7,8,9];cols=[12,7,7,1,5,1,1];ends=[18,10,12,7,9,4,5]
    fullrows=[2,6,4,5,10,8,9];fullcols=[1,1,12,36,1,4,5]
    nodes=[dict(name=name,kind=kinds[i],range=span((rows[i]+shift,cols[i]),(fullrows[i]+shift,fullcols[i])),selectionRange=span((rows[i]+shift,cols[i]),(rows[i]+shift,ends[i]))) for i,name in enumerate(names)]
    nodes[1]['children']=nodes[2:4];nodes[4]['children']=nodes[5:7]
    return [nodes[0],nodes[1],nodes[4]]
def check(value,method,variant=0):
    expected=symbols_expected(variant) if method else selection_expected(variant)
    assert value==expected,(method,variant,value,expected)
with tempfile.TemporaryDirectory(prefix='syntax-😀-#-',dir=os.getcwd()) as folder:
    root=Path(folder);uri,library=[(root/f'{name}.xr').as_uri() for name in ('root','lib')]
    def invoke(method,*args):
        r=subprocess.run([str(exe.resolve()),uri,'--syntax',library,str(method),*map(str,args)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,input=b'',env=env,timeout=290 if mode=='--resources' else 20)
        assert r.returncode==0,(method,args,r.returncode,r.stderr[-7000:])
        assert b'AddressSanitizer' not in r.stderr and b'runtime error:' not in r.stderr
        return r
    if mode=='--counterfactual':
        for method in (0,1):
            r=invoke(method,13)
            if method:
                check(json.loads(r.stdout),1)
                assert b'SYNTAX_BEFORE method=1 real_SourceOpen=1' in r.stderr and b'cached_ledger_debited=1' in r.stderr
            else:
                check(json.loads(r.stdout),0)
                assert b'SYNTAX_BEFORE method=0 duplicate_coordinate_accepted=1 original_CLI_limits=1 physical0/0 JSON0/0 app0/0' in r.stderr
            print(r.stderr.decode('utf-8'))
        print('Exact predecessor selection accepts duplicate coordinates under CLI limits; document symbols debit the original live SourceOpen ledger; physical0/0')
        sys.exit(0)
    rows=[]
    if mode=='--normal':
        for method in (0,1):
            for scenario in [*range(13),14,15,*([16,17] if method==0 else [])]:
                r=invoke(method,scenario)
                if scenario in (12,15):
                    check(json.loads(r.stdout),method)
                    assert (b'cache_AST_and_original_ledger_preserved=1' if scenario==12 else b'server_poisoned=1 one_shot=1') in r.stderr
                elif scenario==14:
                    assert not r.stdout and f'SYNTAX_ADMISSION method={method} duplicate={2 if method else 5} invalid_coordinates={0 if method else 8}'.encode() in r.stderr
                elif scenario in (4,5,6):assert not r.stdout
                else:
                    messages=unpack(r.stdout);assert len(messages)==1 and messages[0]['id']=='source\0λ',messages
                    if scenario in (1,2,8):assert messages[0]['error']['code']==-32800,messages
                    elif scenario==3:assert messages[0]['error']['code']==-32803,messages
                    elif scenario in (16,17):assert messages[0]['error']['code']==-32603,messages
                    else:check(messages[0]['result'],method,scenario)
                if scenario==2:
                    m=re.search(fr'INTERIOR mode={18+method} trigger=4096 calls=(\d+)'.encode(),r.stderr);assert m and 4096<=int(m[1])<=4160,r.stderr
                print(json.dumps(dict(method=method,case=scenario,stderr=r.stderr.decode('utf-8'),stdout=r.stdout.decode('utf-8')),ensure_ascii=False))
                rows.append(dict(method=method,scenario=scenario,passed=True))
    measured=[]
    for method in (0,1):
        for variant in (0,9,10,11):
            r=invoke(method,'--resource',maximum,maximum,variant);m=stats_re.search(r.stderr)
            assert m and all(int(m[n])>0 for n in (1,2,6,7,8)) and all(int(m[n])==0 for n in (3,4,5)),r.stderr
            values=unpack(r.stdout);assert len(values)==1 and 'result' in values[0],values;check(values[0]['result'],method,variant)
            measured.append(dict(method=method,variant=variant,source=int(m[1]),json=int(m[2]),allocated=int(m[6]),peak=int(m[7]),work=int(m[8])))
    if mode=='--normal':
        for method in (0,1):
            for source,js in ((0,maximum),(maximum,0)):
                r=invoke(method,'--resource',source,js,0);assert b'_hit=1' in r.stderr
                for value in unpack(r.stdout):assert value['error']['code']==-32603
    if mode=='--resources':
        chunks=[(m['method'],m['variant'],domain,start,min(64,m[domain]-start),m[domain]) for m in measured for domain in ('source','json') for start in range(0,m[domain],64)]
        def run_chunk(chunk):
            method,variant,domain,start,count,total=chunk;r=invoke(method,'--chunk',domain,start,count,total,variant)
            assert f'SYNTAX_CHUNK domain={domain} start={start} count={count} total={total} checked={count} physical0/0 JSON0/0 app0/0'.encode() in r.stderr
            for value in unpack(r.stdout):assert value['error']['code']==-32603 and 'result' not in value,value
            return dict(method=method,variant=variant,domain=domain,start=start,count=count)
        with ThreadPoolExecutor(max_workers=8) as pool:rows.extend(f.result() for f in as_completed([pool.submit(run_chunk,c) for c in chunks]))
        for m in measured:
            r=invoke(m['method'],'--axes',m['allocated'],m['peak'],m['work'],m['variant'])
            assert b'SYNTAX_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0' in r.stderr
            messages=unpack(r.stdout);assert sum('result' in v for v in messages)==6,messages
            for value in messages:
                if 'result' in value:check(value['result'],m['method'],m['variant'])
                else:assert value['error']['code']==-32603
    print(json.dumps(dict(mode=mode,image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),measured=measured,source_total=sum(m['source'] for m in measured),json_total=sum(m['json'] for m in measured),full_FI=mode=='--resources',rows=rows)))
