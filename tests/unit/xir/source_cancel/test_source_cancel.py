from pathlib import Path
import os,subprocess,sys,tempfile,json,hashlib
exe,stdlib=map(Path,sys.argv[1:3])
before=sys.argv[3:]==['--before'];assert sys.argv[3:] in ([],['--before'])
with tempfile.TemporaryDirectory(prefix='source-cancel-',dir=os.getcwd()) as folder:
    root=Path(folder);(root/'xray.toml').write_text('[project]\nname="source-cancel"\nmain="root.xr"\n',encoding='utf-8')
    result=subprocess.run([str(exe.resolve()),(root/'root.xr').as_uri(),(root/'lib.xr').as_uri()],env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib.resolve())},timeout=290,capture_output=True,text=True,encoding='utf-8')
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    accepted=result.returncode==1 and result.stderr.count('Cancellation did not interrupt inside Source')==8 if before else result.returncode==0
    (exe.parent/(exe.stem+'-result.json')).write_text(json.dumps(dict(image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),exit=result.returncode,accepted=accepted,counterfactual=before,full_resources=False,source='actual owned request, four modes, interior work cancellation, no public server'),indent=2)+'\n',encoding='utf-8')
    assert accepted,(result.returncode,result.stderr)
