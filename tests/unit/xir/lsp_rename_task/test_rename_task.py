from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,tempfile,time
exe,stdlib=map(lambda p:Path(p).resolve(),sys.argv[1:3]);mode=sys.argv[3]
assert mode in ("--normal","--resources","--counterfactual")
env={**os.environ,"XRAY_STDLIB_PATH":str(stdlib)};maximum=str((1<<64)-1)
image=hashlib.sha256(exe.read_bytes()).hexdigest();deadline=time.monotonic()+290
metric=re.compile(rb"RENAME_METRICS method=(\d+) scenario=(\d+) source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) producer_dead=1 physical0/0 JSON0/0 app0/0")
def unpack(data):
    values=[]
    while data:
        head,separator,rest=data.partition(b"\r\n\r\n")
        assert separator and re.fullmatch(rb"Content-Length: [0-9]+",head),head
        size=int(head.split(b": ")[1]);assert 0<size<=len(rest)
        values.append(json.loads(rest[:size]));data=rest[size:]
    return values
def span(line,start,end):
    return dict(start=dict(line=line,character=start),end=dict(line=line,character=end))
with tempfile.TemporaryDirectory(prefix="rename-task-😀-#-",dir=exe.parent) as name:
    root=Path(name);uri,library,closed=[(root/(n+".xr")).as_uri() for n in ("root","lib","closed")]
    (root/"xray.toml").write_text('[project]\nname="rename-task"\nmain="root.xr"\n',encoding="utf-8")
    (root/"closed.xr").write_text('import { target } from "./lib"\nfn use()->i64{return target(2)}\n',encoding="utf-8")
    (root/"ignored.xr").write_text("var *** = ;\n",encoding="utf-8")
    originals={p:p.read_bytes() for p in root.iterdir() if p.is_file()}
    command=[str(exe),uri,"--rename-task",library,closed,str(root)]
    def invoke(method,*arguments):
        remaining=deadline-time.monotonic();assert remaining>0
        r=subprocess.run([*command,str(method),*map(str,arguments)],capture_output=True,input=b"",env=env,
            timeout=min(290 if mode=="--resources" else 20,remaining))
        if r.returncode:
            sys.stdout.write(r.stdout.decode("utf-8","replace"));sys.stderr.write(r.stderr.decode("utf-8","replace"))
            raise AssertionError((method,arguments,r.returncode))
        assert b"AddressSanitizer" not in r.stderr and b"runtime error:" not in r.stderr
        return r
    def check(value,method,scenario):
        if method:
            name="target" if scenario==13 else "lib" if scenario==14 else "alias"
            expected=span(0,10,16) if scenario==13 else span(1,21,24 if scenario==14 else 26)
            assert value==dict(range=expected,placeholder=name),(method,scenario,value)
            return
        changes=value["documentChanges"];assert len(changes)==(3 if scenario==13 else 1)
        found={row["textDocument"]["uri"]:row for row in changes};assert len(found)==len(changes)
        new="changed😀" if scenario==15 else "changed"
        if scenario==13:
            expected={uri:(7,[dict(range=span(0,8,14),newText="changed")]),
                library:(9,[dict(range=span(0,10,16),newText="changed")]),
                closed:(None,[dict(range=span(0,9,15),newText="changed as target")])}
        else:
            declaration=span(0,14,14) if scenario==14 else span(0,27,32) if scenario==15 else span(0,18,23)
            expected={uri:(7,[dict(range=span(1,21,24 if scenario==14 else 26),newText=new),
                dict(range=declaration,newText=" as changed" if scenario==14 else new)])}
        assert set(found)==set(expected)
        for path,(version,edits) in expected.items():
            assert found[path]==dict(textDocument=dict(uri=path,version=version),edits=edits),(scenario,found[path],version,edits)
    try:
        if mode=="--counterfactual":
            for method in (0,1):
                r=invoke(method,"--before");assert not r.stdout
                assert f"RENAME_BEFORE method={method} exact_BAD_STRUCTURE=1 no_compiler_admission=1".encode() in r.stderr
            print(json.dumps(dict(counterfactual=2,exact_predecessor_rejection=True,image_sha256=image)))
            raise SystemExit(0)
        rows=[]
        if mode=="--normal":
            for method in (0,1):
                for scenario in [*range(13),13,14,15,17,18,*([16] if method==0 else [])]:
                    r=invoke(method,scenario)
                    if scenario in (9,11,12):
                        check(json.loads(r.stdout),method,scenario)
                        assert (b"captured_owner_dead=1" if scenario in (9,11) else b"server_poisoned=1 one_shot=1") in r.stderr
                        if scenario==11:assert b"cached_AST_ledger_preserved=1" in r.stderr
                    elif scenario==10:
                        assert not r.stdout and b"RENAME_ADMISSION" in r.stderr
                    elif scenario in (4,5,6):assert not r.stdout
                    else:
                        values=unpack(r.stdout);assert len(values)==1 and values[0]["id"]=="source\0λ",values
                        if scenario in (1,2,8):assert values[0]["error"]["code"]==-32800,values
                        elif scenario==3:assert values[0]["error"]["code"]==-32803,values
                        elif scenario in (16,17):assert values[0]["error"]["code"]==-32603,values
                        else:check(values[0]["result"],method,scenario)
                    if scenario==2:
                        match=re.search(fr"INTERIOR mode={23+method} trigger=4096 calls=(\d+)".encode(),r.stderr)
                        assert match and 4096<=int(match[1])<=4160,r.stderr
                    rows.append(dict(method=method,scenario=scenario))
                    print(json.dumps(dict(method=method,scenario=scenario,stderr=r.stderr.decode("utf-8"),stdout=r.stdout.decode("utf-8")),ensure_ascii=False))
        baselines=[]
        for method in (0,1):
            for scenario in (0,13,14,15):
                r=invoke(method,"--resource",maximum,maximum,scenario);m=metric.search(r.stderr);assert m,r.stderr
                values=unpack(r.stdout);assert len(values)==1 and "result" in values[0];check(values[0]["result"],method,scenario)
                row=dict(method=method,scenario=scenario,source=int(m[3]),json=int(m[4]),allocated=int(m[7]),peak=int(m[8]),work=int(m[9]))
                assert row["source"]>0 and row["json"]>0 and not int(m[5]) and not int(m[6])
                assert row["allocated"]<=64*1024*1024 and row["peak"]<=8*1024*1024 and row["work"]<=128000000
                baselines.append(row)
                if mode=="--normal":
                    for domain in ("source","json"):
                        probe=invoke(method,"--resource","0" if domain=="source" else maximum,
                            "0" if domain=="json" else maximum,scenario)
                        hit=metric.search(probe.stderr);assert hit and int(hit[5 if domain=="source" else 6])==1
                        for value in unpack(probe.stdout):assert value["error"]["code"]==-32603 and "result" not in value
        checked={"source":0,"json":0};axes=0
        if mode=="--resources":
            def axes_case(row):
                r=invoke(row["method"],"--axes",row["allocated"],row["peak"],row["work"],row["scenario"])
                assert b"checked=9 physical0/0 JSON0/0 app0/0" in r.stderr
                values=unpack(r.stdout);assert sum("result" in v for v in values)==6
                for value in values:
                    if "result" in value:check(value["result"],row["method"],row["scenario"])
                    else:assert value["error"]["code"]==-32603
                return 9
            with ThreadPoolExecutor(max_workers=8) as pool:axes=sum(pool.map(axes_case,baselines))
            plans=[(row,domain,start,min(64,row[domain]-start)) for row in baselines for domain in ("source","json") for start in range(0,row[domain],64)]
            def chunk(plan):
                row,domain,start,count=plan
                r=invoke(row["method"],"--chunk",domain,start,count,row[domain],row["scenario"])
                assert f"RENAME_CHUNK domain={domain} start={start} count={count} total={row[domain]} checked={count}".encode() in r.stderr
                for value in unpack(r.stdout):assert value["id"]=="source\0λ" and value["error"]["code"]==-32603 and "result" not in value
                return row["method"],row["scenario"],domain,start,count
            with ThreadPoolExecutor(max_workers=8) as pool:
                done=list(pool.map(chunk,plans))
            assert len(set(done))==len(plans)
            for method,scenario,domain,start,count in done:checked[domain]+=count
            assert all(checked[d]==sum(row[d] for row in baselines) for d in checked)
            assert axes==72
        print(json.dumps(dict(normal_cases=len(rows),baselines=baselines,full_FI=mode=="--resources",
            checked=checked,axes=axes,original_limits=True,workers=8,deadline_seconds=290,image_sha256=image)))
    finally:
        assert hashlib.sha256(exe.read_bytes()).hexdigest()==image
        assert all(p.read_bytes()==content for p,content in originals.items())
        assert not (root/"root.xr").exists() and not (root/"lib.xr").exists()
