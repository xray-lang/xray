"""Hash exact prepared C and independently encode Input2 from observed owner fields."""
from pathlib import Path
import hashlib,re,struct,subprocess,sys,tempfile
exe,stdlib,sdk,compiler=map(lambda x:Path(x).resolve(),sys.argv[1:])
version=subprocess.run([str(compiler)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT).stdout.decode('utf-8',errors='replace').strip()
assert version and re.search(r'\b\d+\.\d+\.\d+\b',version),version
with tempfile.TemporaryDirectory(prefix='xray-projection-owner-') as temporary:
    root=Path(temporary);source=root/'main.xr';source.write_text('print("owned-before-binding")\n',encoding='utf-8');code=root/'actual.c'
    result=subprocess.run([str(exe),str(root),str(source),str(stdlib),str(sdk),str(compiler),version,str(code)],capture_output=True,timeout=300)
    sys.stdout.buffer.write(result.stdout);sys.stderr.buffer.write(result.stderr);assert result.returncode==0,result.returncode
    lines=dict(line.split(' ',1) for line in result.stdout.decode('utf-8').splitlines() if line.split(' ',1)[0] in {'WORDS','SOURCE','CLOSED','LAYOUT','POLICY','GENERATED','TOOLCHAIN','INPUT'})
    assert hashlib.sha256(code.read_bytes()).hexdigest()==lines['GENERATED']
    words=tuple(map(int,lines['WORDS'].split()));assert words[:6]==(2,25,64,21,26,29)
    data=b'xray:xir-native-input:v2'+struct.pack('<10I',*words)
    data+=b''.join(bytes.fromhex(lines[k]) for k in ['SOURCE','CLOSED','LAYOUT','POLICY','GENERATED','TOOLCHAIN'])
    assert hashlib.sha256(data).hexdigest()==lines['INPUT']
print('independent actual C SHA and Input2 framing PASS; observed file facts do not grant complete Target authority')
