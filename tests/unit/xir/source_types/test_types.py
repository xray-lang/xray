from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3]);required=sys.argv[3:]==['--inheritance-required'];assert sys.argv[3:] in ([],['--inheritance-required'])
with tempfile.TemporaryDirectory(prefix='type-facts-',dir=os.getcwd()) as folder:
    root=Path(folder);(root/'xray.toml').write_text('[project]\nname="type-facts"\nmain="root.xr"\n',encoding='utf-8')
    (root/'lib.xr').write_text('export interface Shape {size()->i64}\n',encoding='utf-8')
    rows=[]
    for mode in ([4] if required else range(4)):
        result=subprocess.run([str(exe.resolve()),str(root/'root.xr'),str(mode)],env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib.resolve())},timeout=29)
        rows.append({'mode':mode,'exit':result.returncode})
    (exe.parent/('type-inheritance-required.json' if required else 'type-facts-normal.json')).write_text(json.dumps({'image_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'rows':rows,'required_all_pass':all(r['exit']==0 for r in rows)},indent=2)+'\n',encoding='utf-8')
    assert all(r['exit']==0 for r in rows),rows
