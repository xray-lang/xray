from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,tempfile,time
exe,stdlib=map(lambda p:Path(p).resolve(),sys.argv[1:3]);mode=sys.argv[3]
assert mode in ('--normal','--resources','--before')
deadline=time.monotonic()+(290 if mode=='--resources' else 25)
env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib)};image=hashlib.sha256(exe.read_bytes()).hexdigest()
maximum=str((1<<64)-1)
metric=re.compile(rb'PARSE_DIAGNOSTIC mode=(\d+) source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) producer_dead=1 physical0/0 JSON0/0')
bad='/* 😀 */ var *** = ;\n';crlf='/* 😀 */\r\n/* 😀 */ var *** = ;\r\n';bare_cr='/* 😀 */\r/* 😀 */ var *** = ;\n'
with tempfile.TemporaryDirectory(prefix='parser-diagnostic-😀-#-',dir=exe.parent) as name:
    temporary=Path(name).resolve();assert temporary.is_relative_to(exe.parent)
    commands={};originals={}
    for variant in range(8):
        root=temporary/str(variant);root.mkdir()
        (root/'xray.toml').write_text('[project]\nname="parse-diagnostics"\nmain="root.xr"\n',encoding='utf-8')
        library=root/'lib.xr';text=crlf if variant==2 else bare_cr if variant==3 else 'export fn value()->i64{return 99}\n' if variant in (0,4) else bad
        library.write_bytes(text.encode())
        commands[variant]=[str(exe),(root/'root.xr').as_uri(),library.as_uri(),(root/'root.xr').as_posix(),library.as_posix(),root.as_posix(),str(variant)]
        originals.update({p:p.read_bytes() for p in root.iterdir()})
    def invoke(variant,*arguments):
        remaining=deadline-time.monotonic();assert remaining>0
        r=subprocess.run([*commands[variant],*map(str,arguments)],capture_output=True,env=env,timeout=remaining)
        if r.returncode:
            sys.stdout.write(r.stdout.decode('utf-8','replace'));sys.stderr.write(r.stderr.decode('utf-8','replace'))
            raise AssertionError((variant,arguments,r.returncode))
        assert b'AddressSanitizer' not in r.stderr and b'runtime error:' not in r.stderr
        return r
    def check_output(r,variant):
        if variant>=6:assert not r.stdout;return
        value=json.loads(r.stdout);assert value['uri']==commands[variant][1 if variant==5 else 2]
        assert value['version']==(9 if variant in (0,4) else 7 if variant==5 else None)
        row=1 if variant in (2,3) else 0
        d=value['diagnostics'][0]
        assert d['range']==dict(start=dict(line=row,character=13),end=dict(line=row,character=14 if variant==5 else 13))
        assert d['message']=='expected variable name' and d['source']=='xray' and d['severity']==1
        if variant!=5:assert len(value['diagnostics'])==1
    try:
        variants=[6,7] if mode=='--before' else list(range(8));baselines=[]
        for variant in variants:
            r=invoke(variant);check_output(r,variant)
            m=metric.search(r.stderr);assert m,r.stderr
            baselines.append(dict(mode=variant,source=int(m[2]),json=int(m[3]),allocated=int(m[6]),peak=int(m[7]),work=int(m[8])))
            assert not int(m[4]) and not int(m[5])
        probes=0;checked={'source':0,'json':0};axes=0
        if mode=='--normal':
            r=invoke(0,'--self');assert not r.stdout
            assert b'PARSE_POSITION cases=16 work_cuts=3 no_allocation=1' in r.stderr
            assert b'PARSE_OCCUPIED fields=5 before_allocation=1 output_preserved=1' in r.stderr
            assert b'PARSE_FAILURE_LIFETIME producer_and_caller_ledger_dead=1 input_poisoned=1' in r.stderr
            r=invoke(2,'--change-disk');check_output(r,2)
            for row in baselines:
                for domain in ('source','json'):
                    if not row[domain]:continue
                    r=invoke(row['mode'],'0' if domain=='source' else maximum,'0' if domain=='json' else maximum)
                    m=metric.search(r.stderr);assert m and int(m[4 if domain=='source' else 5])==1 and not r.stdout;probes+=1
        elif mode=='--resources':
            def axis_case(row):
                base=[row['allocated'],row['peak'],row['work']];total=0
                for axis in range(3):
                    for delta in (-1,0,1):
                        values=base.copy();assert values[axis]>0;values[axis]+=delta
                        r=invoke(row['mode'],'--limits',int(delta<0),*values,'exact')
                        if delta<0:assert not r.stdout
                        else:check_output(r,row['mode'])
                        total+=1
                return total
            with ThreadPoolExecutor(max_workers=8) as pool:axes=sum(pool.map(axis_case,baselines))
            plans=[(row,domain,start,min(64,row[domain]-start)) for row in baselines for domain in ('source','json') for start in range(0,row[domain],64)]
            def chunk(plan):
                row,domain,start,count=plan
                r=invoke(row['mode'],'--chunk',domain,start,count,row[domain]);assert not r.stdout
                assert f"PARSE_CHUNK mode={row['mode']} domain={domain} begin={start} checked={count} total={row[domain]}".encode() in r.stderr
                return domain,count
            with ThreadPoolExecutor(max_workers=8) as pool:
                for domain,count in pool.map(chunk,plans):checked[domain]+=count
            assert all(checked[d]==sum(row[d] for row in baselines) for d in checked) and axes==72
        print(json.dumps(dict(mode=mode,baselines=baselines,full_FI=mode=='--resources',checked=checked,axes=axes,
            first_prefix_probes=probes,disk_change_after_capture=mode=='--normal',before_exact_generic=mode=='--before',
            images_unchanged=True,image_sha256=image,limits=[67108864,8388608,128000000],workers=8)))
    finally:
        assert hashlib.sha256(exe.read_bytes()).hexdigest()==image
        assert all(p.read_bytes()==b for p,b in originals.items())
        assert all(not (temporary/str(v)/'root.xr').exists() for v in range(8))
