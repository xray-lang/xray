from concurrent.futures import ThreadPoolExecutor,as_completed
from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile,time

exe,stdlib=map(Path,sys.argv[1:3]);exe=exe.resolve();full=sys.argv[3:]==["--full"];assert sys.argv[3:] in ([],["--full"])
started=time.monotonic();deadline=started+290;image=hashlib.sha256(exe.read_bytes()).hexdigest();maximum=str(2**64-1)
with tempfile.TemporaryDirectory(prefix="call-resources-",dir=os.getcwd()) as folder:
    root=Path(folder);(root/"xray.toml").write_text('[project]\nname="call-resources"\nversion="1.0.0"\nmain="root.xr"\n',encoding="utf-8")
    (root/"lib.xr").write_text("export fn target(v:i64)->i64 {\n return v\n}\n",encoding="utf-8")
    (root/"closed.xr").write_text('import {target as other} from "./lib"\nimport "./root"\nfn external(v:i64)->i64{return other(v)}\n',encoding="utf-8")
    command=[str(exe),(root/"root.xr").as_uri(),(root/"lib.xr").as_uri(),(root/"closed.xr").as_uri()]
    environment={**os.environ,"XRAY_STDLIB_PATH":str(stdlib.resolve())}
    def run(method,source=maximum,js=maximum):
        remaining=deadline-time.monotonic();assert remaining>0,"290-second complete request deadline expired"
        result=subprocess.run([*command,"--resource",str(method),str(source),str(js)],env=environment,capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=min(290,remaining),creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0))
        if result.returncode:
            sys.stdout.write(result.stdout);sys.stderr.write(result.stderr);raise RuntimeError((method,source,js,result.returncode))
        row=json.loads(result.stdout.splitlines()[-1]);assert row["method"]==method and row["ledgers"]==1
        assert all(not row[k] for k in ("physical_live","physical_bytes","json_live","json_bytes"))
        return row
    baselines=[run(i) for i in range(3)];jobs=[]
    def chunk(job):
        method,domain,start,count,total=job;remaining=deadline-time.monotonic();assert remaining>0
        result=subprocess.run([*command,"--chunk",str(method),domain,str(start),str(count),str(total)],env=environment,capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=min(290,remaining),creationflags=getattr(subprocess,"CREATE_NO_WINDOW",0))
        if result.returncode:
            sys.stdout.write(result.stdout);sys.stderr.write(result.stderr);raise RuntimeError((job,result.returncode))
        row=json.loads(result.stdout.splitlines()[-1]);assert row==dict(method=method,domain=domain,start=start,count=count,total=total,checked=count);return row
    for baseline in baselines:
        assert baseline["status"]==0 and baseline["source"]>0 and baseline["json"]>0
        for domain in ("source","json"):
            if full:jobs.extend((baseline["method"],domain,start,min(64,baseline[domain]-start),baseline[domain]) for start in range(0,baseline[domain],64))
            else:jobs.extend((baseline["method"],domain,p) for p in sorted({0,baseline[domain]//2,baseline[domain]-1}))
    checked={domain:[0,0,0] for domain in ("source","json")}
    with ThreadPoolExecutor(max_workers=8 if full else 1) as pool:
        futures={pool.submit(chunk,job) if full else pool.submit(run,job[0],job[2] if job[1]=="source" else maximum,job[2] if job[1]=="json" else maximum):job for job in jobs}
        for future in as_completed(futures):
            job=futures[future];method,domain=job[:2];row=future.result()
            if full:checked[domain][method]+=row["checked"]
            else:assert row["status"]==7 and row[domain+"_hit"];checked[domain][method]+=1
    for domain in checked:
        assert checked[domain]==[b[domain] if full else len({0,b[domain]//2,b[domain]-1}) for b in baselines]
    chunk_probe=[]
    if not full:
        for baseline in baselines:
            for domain in ("source","json"):
                chunk_probe.append(chunk((baseline["method"],domain,0,min(2,baseline[domain]),baseline[domain])))
    assert hashlib.sha256(exe.read_bytes()).hexdigest()==image
    print(json.dumps({"full":full,"scope":"Three actual handler pipelines over one valid complete corpus; required non-Sendable module-callable positive is a separate unresolved gate; no input decoder/process qualification","baselines":baselines,"checked":checked,"chunk_probe":chunk_probe,"workers":8 if full else 1,"global_deadline":290,"image_sha256":image,"elapsed":time.monotonic()-started}))
