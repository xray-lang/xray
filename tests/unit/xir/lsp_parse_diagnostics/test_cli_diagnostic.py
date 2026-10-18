from pathlib import Path
import hashlib,subprocess,sys,tempfile
exe,stdlib=map(lambda p:Path(p).resolve(),sys.argv[1:3]);image=hashlib.sha256(exe.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory(prefix='parser-cli-',dir=exe.parent) as name:
    root=Path(name).resolve();assert root.is_relative_to(exe.parent)
    for mode,text in [('parse-reject','var value = (\n'),('graph-reject','import { value } from "./absent"\n')]:
        entry=root/(mode+'.xr');entry.write_text(text,encoding='utf-8')
        result=subprocess.run([str(exe),str(entry),str(stdlib),mode],capture_output=True,timeout=20)
        sys.stdout.buffer.write(result.stdout);sys.stderr.buffer.write(result.stderr)
        assert result.returncode==0,(mode,result.returncode)
        assert entry.read_text(encoding='utf-8')==text
assert hashlib.sha256(exe.read_bytes()).hexdigest()==image
print('Original CLI parser and graph rejection assertions passed with owned failure type')
