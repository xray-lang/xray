from concurrent.futures import ThreadPoolExecutor,as_completed
from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile,time
exe,stdlib=map(Path,sys.argv[1:3]);exe=exe.resolve();full=sys.argv[3:]==['--full'];assert sys.argv[3:] in ([],['--full'])
started=time.monotonic();deadline=started+290;image=hashlib.sha256(exe.read_bytes()).hexdigest();maximum=str(2**64-1)
limits=[str(64*1024*1024),str(8*1024*1024),'128000000']
with tempfile.TemporaryDirectory(prefix='type-resources #😀-',dir=os.getcwd()) as folder:
    root=Path(folder);manifest=root/'xray.toml';library=root/'lib.xr';entry=root/'root.xr'
    manifest.write_text('[project]\nname="type-facts"\nmain="root.xr"\n',encoding='utf-8')
    library.write_text('export interface Shape {size()->i64}\n',encoding='utf-8')
    before={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (manifest,library)};assert not entry.exists()
    environment={**os.environ,'XRAY_STDLIB_PATH':str(stdlib.resolve())}
    def child(mode,*args):
        remaining=deadline-time.monotonic();assert remaining>0,'original290-second Source type request deadline expired'
        result=subprocess.run([str(exe),str(entry),str(mode),*map(str,args)],env=environment,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=min(290,remaining),creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if result.returncode:
            sys.stdout.write(result.stdout);sys.stderr.write(result.stderr);raise RuntimeError((mode,args,result.returncode))
        return json.loads(result.stdout.splitlines()[-1])
    def point(mode,fault=maximum):
        row=child(mode,'--resource',fault,*limits)
        assert row['mode']==mode and row['status']==(0 if str(fault)==maximum else 7) and row['ledgers']==1
        assert row['fault_hit']==(str(fault)!=maximum) and row['physical_live']==row['physical_bytes']==0
        return row
    baselines=[point(mode) for mode in range(4)];axes=[]
    for baseline in baselines:
        row=child(baseline['mode'],'--axes',*[baseline[k] for k in ('allocated','peak','work')])
        assert row==dict(mode=baseline['mode'],axes=3,checked=9,physical_live=0,physical_bytes=0);axes.append(row)
    def chunk(job):
        mode,start,count,total=job;row=child(mode,'--chunk',start,count,total)
        assert row==dict(mode=mode,start=start,count=count,total=total,checked=count);return row
    jobs=[]
    for baseline in baselines:
        assert baseline['source']>0 and baseline['allocated']<=64*1024*1024 and baseline['peak']<=8*1024*1024 and baseline['work']<=128000000
        if full:jobs += [(baseline['mode'],start,min(64,baseline['source']-start),baseline['source']) for start in range(0,baseline['source'],64)]
        else:jobs += [(baseline['mode'],p) for p in sorted({0,baseline['source']//2,baseline['source']-1})]
    checked=[0]*4
    with ThreadPoolExecutor(max_workers=8 if full else 1) as pool:
        futures={pool.submit(chunk,job) if full else pool.submit(point,*job):job for job in jobs}
        for future in as_completed(futures):
            job=futures[future];row=future.result();checked[job[0]]+=row['checked'] if full else 1
    assert checked==[b['source'] if full else len({0,b['source']//2,b['source']-1}) for b in baselines]
    controls=[] if full else [chunk((b['mode'],0,min(2,b['source']),b['source'])) for b in baselines]
    assert not entry.exists() and before=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (manifest,library)}
    assert hashlib.sha256(exe.read_bytes()).hexdigest()==image
    print(json.dumps(dict(full=full,scope='Four real Source type programs, original64Mi/8Mi/128000000 ledger, complete factory/corpus/query/type observation pipeline; class inheritance required failure is separate; no LSP JSON/decoder/process qualification',baselines=baselines,axes=axes,checked=checked,chunk_controls=controls,workers=8 if full else 1,global_deadline=290,image_sha256=image,elapsed=time.monotonic()-started)))
