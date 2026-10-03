"""Real local discovery outside a developer prompt; no native compilation."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

exe, sdk, compiler, output = map(lambda p: Path(p).resolve(), sys.argv[1:5])
output.mkdir(parents=True, exist_ok=True)
output = Path(tempfile.mkdtemp(prefix='run-', dir=output))
environment = dict(os.environ)
for key in list(environment):
    if key.upper().startswith(('VSCMD_', 'VS', 'VC', 'WINDOWSSDK', 'UCRT', 'UNIVERSALCRT')) or key.upper() in ('INCLUDE','LIB','LIBPATH','CL','_CL_','LINK','_LINK_'):
        environment.pop(key)
# Keep only the system search path needed by Microsoft's discovery batch file.
system = Path(os.environ['SystemRoot'])
asan = [p for p in os.environ.get('PATH','').split(os.pathsep) if 'clang' in p.lower() and p.lower().endswith('windows')]
environment['PATH'] = os.pathsep.join([*asan, str(system/'System32'), str(system), str(system/'System32/Wbem')])
records = []
def run(name, binary, bundle, cc, workspace, expected, expected_sdk=sdk, env=environment):
    framed = hashlib.sha256()
    for key, value in sorted(env.items()):
        for text in (key, value):
            data = text.encode('utf-8'); framed.update(len(data).to_bytes(8,'little')); framed.update(data)
    environment_identity = {'count':len(env), 'framed_sha256':framed.hexdigest()}
    command = [str(binary), str(bundle), str(cc), str(workspace), str(expected)]
    result = subprocess.run(command, env=env, capture_output=True, timeout=120)
    (output/f'{name}.stdout').write_bytes(result.stdout)
    (output/f'{name}.stderr').write_bytes(result.stderr)
    records.append({'case':name, 'argv':command, 'returncode':result.returncode,
                    'exe_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
                    'sdk_manifest_sha256':hashlib.sha256((sdk/'sdk_manifest.json').read_bytes()).hexdigest(),
                    'discovery_environment':environment_identity})
    (output/'records.json').write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
    print(name, result.returncode, flush=True)
    if result.returncode:
        sys.stdout.buffer.write(result.stdout); sys.stderr.buffer.write(result.stderr)
        raise SystemExit(result.returncode)
    if expected == 0:
        values=dict(line.split('=',1) for line in result.stdout.decode('utf-8').splitlines() if line.startswith(('compiler=','linker=','sdk=','workspace=')))
        assert Path(values['compiler']).samefile(compiler), values
        assert Path(values['linker']).samefile(compiler.parent/'link.exe'), values
        assert Path(values['sdk']).samefile(expected_sdk), values
        if workspace != '-': assert Path(values['workspace']).samefile(workspace), values
with tempfile.TemporaryDirectory(prefix='xray-local-toolchain-') as temporary:
    parent = Path(temporary)
    fixture = parent / 'owned-中'; fixture.mkdir()
    cases = [('automatic', sdk, '-', '-', 0), ('explicit', sdk, compiler, fixture, 0),
             ('missing-sdk', parent/'missing-sdk', compiler, fixture, 2),
             ('missing-compiler', sdk, parent/'missing-cl.exe', fixture, 2),
             ('missing-parent', sdk, compiler, parent/'missing-parent', 2),
             ('wrong-compiler', sdk, compiler.parent/'link.exe', fixture, 1),
             ('empty-parent', sdk, compiler, '', 1),
             ('tail-space-parent', sdk, compiler, str(fixture)+' ', 1)]
    for name, bundle, cc, workspace, expected in cases:
        run(name, exe, bundle, cc, workspace, expected)
    long_parent=fixture
    for i in range(12):
        long_parent=long_parent/(f'segment{i}_'+'x'*80); long_parent.mkdir()
    assert len(str(long_parent))>1024
    run('long-parent',exe,sdk,compiler,long_parent,0)
    install=parent/'installation'; (install/'bin').mkdir(parents=True)
    staged=install/'bin'/exe.name; shutil.copy2(exe,staged)
    installed_sdk=install/'lib/xray/xir-runtime-sdk'; shutil.copytree(sdk,installed_sdk)
    run('installed-sdk-auto',staged,'-',compiler,fixture,0,installed_sdk)
    first=staged.parent/'xir-runtime-sdk'; first.write_bytes(b'wrong kind')
    run('first-sdk-wrong-kind',staged,'-',compiler,fixture,1)
    first.unlink(); first.mkdir(); (first/'sdk_manifest.json').write_bytes(b'{}')
    run('first-sdk-bad-manifest',staged,'-',compiler,fixture,1)
    (first/'sdk_manifest.json').unlink(); first.rmdir(); shutil.move(str(installed_sdk),first)
    run('build-sdk-auto',staged,'-',compiler,fixture,0,first)
    no_temp=dict(environment)
    for key in list(no_temp):
        if key.upper() in ('TEMP','TMP'): no_temp.pop(key)
    run('system-temp',exe,sdk,compiler,'-',0,env=no_temp)
    bad_temp=dict(environment); bad_temp['TEMP']=str(parent/'missing-temp'); bad_temp['TMP']=str(fixture)
    run('invalid-temp-no-fallback',exe,sdk,compiler,'-',2,env=bad_temp)
print('real VS discovery, SDK admission, explicit no-fallback and owner lifetime PASS')
