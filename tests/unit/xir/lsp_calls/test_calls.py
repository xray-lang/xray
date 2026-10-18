from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="call-hierarchy-",dir=os.getcwd()) as folder:
    root=Path(folder)
    (root/"xray.toml").write_text('[project]\nname="call-hierarchy"\nversion="1.0.0"\nmain="root.xr"\n',encoding="utf-8")
    (root/"lib.xr").write_text("export fn target(v:i64)->i64 {\n return v\n}\n",encoding="utf-8")
    (root/"closed.xr").write_text('import {target as other} from "./lib"\nimport "./root"\nfn external(v:i64)->i64{return other(v)}\n',encoding="utf-8")
    failures=[];results=[]
    for mode in range(9):
        result=subprocess.run([str(exe.resolve()),(root/"root.xr").as_uri(),(root/"lib.xr").as_uri(),(root/"closed.xr").as_uri(),str(mode)],env={**os.environ,"XRAY_STDLIB_PATH":str(stdlib.resolve())},timeout=290)
        if result.returncode:failures.append((mode,result.returncode))
        results.append({"mode":mode,"exit":result.returncode})
    (exe.parent/"call-normal-coverage.json").write_text(json.dumps({"image_sha256":hashlib.sha256(exe.read_bytes()).hexdigest(),"rows":results,"required_all_pass":not failures},indent=2)+"\n",encoding="utf-8")
    assert not failures,failures
