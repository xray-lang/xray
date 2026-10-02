from pathlib import Path
import subprocess,sys,tempfile

exe,stdlib=map(Path,sys.argv[1:3])
def run(path,mode):
    p=subprocess.run([str(exe),str(path.resolve()),str(stdlib.resolve()),mode],capture_output=True,timeout=180)
    sys.stdout.buffer.write(p.stdout);sys.stderr.buffer.write(p.stderr)
    assert p.returncode==0,(mode,p.returncode)
with tempfile.TemporaryDirectory(prefix='xray-cli-source-owner-') as temporary:
    root=Path(temporary)
    script=root/'script';script.mkdir()
    program='var counter = 40\ncounter++\nprint("cli-owner", counter)\n'
    (script/'main.xr').write_text(program,encoding='utf8')
    run(script/'main.xr','accept')
    project=root/'project';project.mkdir()
    (project/'main.xr').write_text(program,encoding='utf8')
    (project/'xray.toml').write_text('[project]\nname="cli-owner"\nversion="1.0.0"\nmain="main.xr"\n',encoding='utf8')
    (project/'xray.lock').write_text('# Format version: 1\n[package.unused]\nversion="1.2.3"\nresolved="unused.tar"\nchecksum="sha256:'+'0'*64+'"\n',encoding='utf8')
    run(project/'main.xr','scan')
    (script/'bad.xr').write_text('var value = missing_symbol\n',encoding='utf8')
    run(script/'bad.xr','reject')
    run(script/'absent.xr','not-found')
    (project/'xray.toml').write_text('[project]\nname=42\n',encoding='utf8')
    run(project/'main.xr','invalid')
    (project/'xray.toml').write_text('[project]\nname="cli-owner"\n',encoding='utf8')
    run(project/'main.xr','limit')
    library=root/'catalog';library.mkdir()
    (library/'main.xr').write_text('import { answer } from "./library"\nprint("cli-owner", answer())\n',encoding='utf8')
    (library/'library.xr').write_text('export fn answer() -> i64 { return 42 }\n',encoding='utf8')
    run(library/'main.xr','catalog')
print('real script/project/lockfile/catalog CLI Source owner PASS')
