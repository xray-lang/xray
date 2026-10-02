from pathlib import Path
import subprocess,sys,tempfile,hashlib
with tempfile.TemporaryDirectory(prefix='xray-project-owner-') as temporary:
    root=Path(temporary);(root/'sub').mkdir();(root/'point.h').write_text('struct Point { long long value; };\n')
    source=b'int owner_answer(void) { return 42; }\n';(root/'owner.c').write_bytes(source)
    (root/'xray.toml').write_text('''[project]
name="owner-test"
main="main.xr"
[dependencies]
local={path="sub"}
other="^1.2.0"
[target.x86_64-windows-msvc]
cc="cl.exe"
cc_flags=["/O2", "/W4"]
[native]
name="owner-native"
version="1.0.0"
license="MIT"
source="local"
audit_mode="exploratory"
vm="unsupported"
[[native.unit]]
name="unit"
kind="c"
sources=["owner.c"]
source_hashes=["SOURCE_HASH"]
include_dirs=["sub"]
defines=["OWNER=1"]
system_links=["kernel32"]
c_standard="c11"
optimization="release"
visibility="default"
warnings="strict"
cpu_feature="baseline"
output="owner.obj"
purpose="native fixture"
[[native.symbol]]
xray="math.answer"
native="owner_answer"
kind="function"
calling_convention="c"
unit="unit"
[native.target.x86_64-windows-msvc]
profile="release"
visibility="default"
cpu_feature="baseline"
system_links=["kernel32"]
vm="unsupported"
[[native.layout]]
xray_type="Point"
c_type="Point"
header="point.h"
assert={size=true,align=true,fields=true}
[[export.c]]
xray="answer"
symbol="answer"
visibility="default"
abi="native"
header=true
[[export.c]]
xray="drop"
symbol="drop"
visibility="hidden"
abi="native"
'''.replace('SOURCE_HASH',hashlib.sha256(source).hexdigest()),encoding='utf-8')
    (root/'xray.lock').write_text('[package.owner/dep]\nversion = "1.2.3"\nresolved = "https://example.invalid/dep"\nchecksum = "sha256:test"\ndependencies = []\n',encoding='utf-8')
    (root/'main.xr').write_text('var value = 42\n');(root/'sub'/'inner.xr').write_text('var value = 42\n')
    for name,text in {
        'bad':'[project\n',
        'package':"[package]\nname='owner/package'\nversion='1.2.3'\n",
        'badversion':"[package]\nname='owner/package'\nversion='^1.2.3'\n",
    }.items():
        folder=root/name;folder.mkdir();(folder/'xray.toml').write_text(text);(folder/'main.xr').write_text('var x = 1\n')
    with tempfile.TemporaryDirectory(prefix='xray-script-owner-') as script_temp:
        script=Path(script_temp)/'main.xr';script.write_text('var x = 1\n')
        subprocess.run([sys.argv[1],str(root),str(script)]+sys.argv[2:],check=True)

