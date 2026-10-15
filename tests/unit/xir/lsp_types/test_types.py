from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3]);required=sys.argv[3:]==['--inheritance-required'];assert sys.argv[3:] in ([],['--inheritance-required'])
with tempfile.TemporaryDirectory(prefix='type-hierarchy-',dir=os.getcwd()) as folder:
    root=Path(folder);(root/'xray.toml').write_text('[project]\nname="type-hierarchy"\nmain="root.xr"\n',encoding='utf-8')
    (root/'lib.xr').write_text('export interface Base {\n measure()->i64\n}\n',encoding='utf-8')
    (root/'closed.xr').write_text('import {Base as Other} from "./lib"\ninterface External extends Other {}\n',encoding='utf-8')
    rows=[]
    for mode in ([6] if required else [0,1,2,3,4,5,7]):
        result=subprocess.run([str(exe.resolve()),(root/'root.xr').as_uri(),(root/'lib.xr').as_uri(),(root/'closed.xr').as_uri(),str(mode)],env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib.resolve())},timeout=290)
        rows.append(dict(mode=mode,exit=result.returncode))
    (exe.parent/('type-hierarchy-inheritance-required.json' if required else 'type-hierarchy-normal.json')).write_text(json.dumps(dict(image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),rows=rows,required_all_pass=all(r['exit']==0 for r in rows)),indent=2)+'\n',encoding='utf-8')
    assert all(r['exit']==0 for r in rows),rows
