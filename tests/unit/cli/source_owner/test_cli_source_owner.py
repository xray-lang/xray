from pathlib import Path
import subprocess,sys,tempfile

exe,stdlib=map(Path,sys.argv[1:3])
def run(path,mode,expected=None):
    args=[str(exe),str(path.resolve()),str(stdlib.resolve()),mode]
    if expected is not None: args.append(str(expected.resolve()))
    p=subprocess.run(args,capture_output=True,timeout=180)
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
    nested=script/('子'*65);nested.mkdir()
    child=nested/'child.xr'
    child.write_text('const bad = missing_symbol\nexport fn answer() -> i64 { return 42 }\n',encoding='utf8')
    imported=script/'入口.xr'
    imported.write_text('import { answer } from "./'+nested.name+'/child"\nprint(answer())\n',encoding='utf8')
    assert len(str(child.resolve()).encode('utf8'))>192
    run(imported,'failure-path',child)
    (script/'parse.xr').write_text('var value = (\n',encoding='utf8')
    run(script/'parse.xr','parse-reject')
    (script/'graph.xr').write_text('import { value } from "./absent"\n',encoding='utf8')
    run(script/'graph.xr','graph-reject')
    run(script/'absent.xr','not-found')
    (project/'xray.toml').write_text('[project]\nname=42\n',encoding='utf8')
    run(project/'main.xr','invalid')
    (project/'xray.toml').write_text('[project]\nname="cli-owner"\n',encoding='utf8')
    run(project/'main.xr','limit')
    library=root/'catalog';library.mkdir()
    (library/'main.xr').write_text('import { answer } from "./library"\nprint("cli-owner", answer())\n',encoding='utf8')
    (library/'library.xr').write_text('export fn answer() -> i64 { return 42 }\n',encoding='utf8')
    run(library/'main.xr','catalog')
    for name, body in (
        ('private-var', 'var serial:i64=40\nexport fn answer()->i64 { return serial+2 }\n'),
        ('destructure-var', 'var (serial,spare)=(40,2)\nexport fn answer()->i64 { return serial+spare }\n'),
        ('unit-var', 'var serial:()=()\nexport fn answer()->i64 { return 42 }\n')):
        rejected=root/name;rejected.mkdir()
        (rejected/'main.xr').write_text('import { answer } from "./library"\nprint(answer())\n',encoding='utf8')
        (rejected/'library.xr').write_text(body,encoding='utf8')
        run(rejected/'main.xr','state-reject')
    checked=root/'checked-var';checked.mkdir()
    (checked/'main.xr').write_text('import { answer } from "./library"\nprint(answer())\n',encoding='utf8')
    (checked/'library.xr').write_text('var serial:i64=40\nexport fn answer()->i64 { return serial+2 }\n',encoding='utf8')
    run(checked/'main.xr','catalog-state-reject')
    constant=root/'constant-atomic';constant.mkdir()
    (constant/'main.xr').write_text('import { answer } from "./library"\nprint("cli-owner",answer())\n',encoding='utf8')
    (constant/'library.xr').write_text('const serial=Atomic(40)\nexport fn answer()->i64 { return serial.fetchAdd(2)+2 }\n',encoding='utf8')
    run(constant/'main.xr','catalog')
print('real script/project/lockfile/catalog CLI Source owner PASS')
