"""Require real capability fallback and preserve explicit/provider failure boundaries."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,sys,tempfile

exe,clang,msvc,parent=map(lambda p:Path(p).resolve(),sys.argv[1:5])
parent.mkdir(parents=True,exist_ok=True)
root=Path(tempfile.mkdtemp(prefix='fallback-原生-',dir=parent));records=[]
env=dict(os.environ,LOCALAPPDATA=str(root/'config'))
def run(name,args,environment=env,expected=0):
    argv=[str(exe),*map(str,args)]
    result=subprocess.run(argv,env=environment,capture_output=True,timeout=120)
    row=dict(case=name,argv=argv,exit=result.returncode,stdout=result.stdout.decode('utf-8',errors='replace'),
             stderr=result.stderr.decode('utf-8',errors='replace'))
    records.append(row);(parent/'records.json').write_text(json.dumps(records,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    assert result.returncode==expected,row
    return result
def probe(name,environment=env,options=(),ready=True,provider='msvc',fallback=False):
    result=run(name,['toolchain','probe','--target','native',*options,'--json'],environment,0 if ready else 3)
    assert not result.stderr,result.stderr
    value=json.loads(result.stdout);selection=value['selection']
    assert set(value)=={'schema','xray','request','selection','capabilities','diagnostics','probe'}
    assert value['schema']==1 and selection['ready'] is ready and selection['fallbackUsed'] is fallback
    assert selection['targetAbi']=='x86_64-windows-msvc' and value['probe']['cache']=='disabled'
    if ready:
        assert selection['provider']==provider and all(value['capabilities'][k]=='ok' for k in ['cCompile','sdkCompile','runtimeLink','nativeRun'])
        assert bool(value['diagnostics']) is fallback
    else:assert value['diagnostics'] and value['capabilities']['nativeRun']!='ok'
    return value

probe('ready-msvc-is-preferred',options=['--provider','auto'])
vc=Path(os.environ['VCToolsInstallDir']);kit=os.environ['WindowsSdkDir'];version=os.environ['WindowsSDKVersion']
assert msvc.is_file() and clang.is_file() and (vc/'include').is_dir()
original={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [msvc,msvc.parent/'link.exe']}
private=root/'vc';binary=private/'bin/Hostx64/x64';binary.mkdir(parents=True)
copied=[]
def copy(source,destination):
    destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,destination)
    copied.append(dict(source=str(source),path=str(destination),sha256=hashlib.sha256(destination.read_bytes()).hexdigest()))
copy(msvc,binary/'cl.exe');copy(msvc.parent/'link.exe',binary/'link.exe')
for dll in msvc.parent.glob('*.dll'):
    copy(dll,binary/dll.name)
for directory in msvc.parent.iterdir():
    if directory.is_dir() and directory.name.isdecimal():shutil.copytree(directory,binary/directory.name)
shutil.copytree(vc/'include',private/'include')
for name in ['msvcrt.lib','oldnames.lib','vcruntime.lib']:copy(vc/'lib/x64'/name,private/'lib/x64'/name)
vs=root/'vs';script=vs/'Common7/Tools/VsDevCmd.bat';script.parent.mkdir(parents=True)
script.write_text('@echo off\nset "VCToolsInstallDir=%~dp0..\\..\\..\\vc\\"\nset "WindowsSdkDir='+kit+'"\n'
                  'set "WindowsSDKVersion='+version+'"\nset "SystemRoot='+os.environ['SystemRoot']+'"\n',encoding='utf-8')
program_files=root/'discovery';locator=program_files/'Microsoft Visual Studio/Installer/vswhere.exe';locator.parent.mkdir(parents=True)
source=root/'locator.c';source.write_text('#include <stdio.h>\nint main(void){puts('+json.dumps(str(vs),ensure_ascii=False)+');return 0;}\n',encoding='utf-8')
compiled=subprocess.run([str(msvc),'/nologo','/utf-8','/MD',str(source),'/Fe'+str(locator),'/Fo'+str(root/'locator.obj')],
                        env=env,cwd=root,capture_output=True,timeout=120)
(parent/'locator-compile.stdout').write_bytes(compiled.stdout);(parent/'locator-compile.stderr').write_bytes(compiled.stderr)
assert compiled.returncode==0,(compiled.returncode,compiled.stdout,compiled.stderr)
broken=dict(env,**{'ProgramFiles(x86)':str(program_files)})
negative=probe('explicit-broken-msvc',broken,['--provider','msvc'],False)
assert negative['diagnostics'][0]['code']=='COMPILE_PROBE_FAILED',negative
assert negative['diagnostics'][0]['stage']=='native-transaction',negative
probe('unresolved-provider-inputs-do-not-fallback',broken,['--provider','auto'],False)
for name in ['cl.exe.config','link.exe.config']:copy(msvc.parent/name,binary/name)
header=private/'include/vcruntime.h';original_header=header.read_bytes()
header.write_bytes(original_header+b'\n#if defined(_MSC_VER) && !defined(__clang__)\n#error private capability fixture rejects MSVC C compilation\n#endif\n')
copied.append(dict(source=str(vc/'include/vcruntime.h'),path=str(header),
                   original_sha256=hashlib.sha256(original_header).hexdigest(),sha256=hashlib.sha256(header.read_bytes()).hexdigest()))
probe('explicit-real-msvc-compile-failure',broken,['--provider','msvc'],False)
selected=probe('automatic-after-real-child-failure',broken,['--provider','auto'],True,'clang',True)
assert Path(selected['selection']['compiler']).samefile(clang)
assert selected['diagnostics'][0]['code']=='COMPILE_PROBE_FAILED',selected
probe('explicit-path-prevents-fallback',broken,['--provider','auto','--cc',binary/'cl.exe'],False)
app=root/'app.xr';app.write_text('print(42)\n',encoding='utf-8');output=root/'native.exe'
run('automatic-build-shares-probe',['build',app,'--toolchain','auto','-o',output],broken)
observed=subprocess.run([str(output)],capture_output=True,timeout=30)
assert (observed.returncode,observed.stdout,observed.stderr)==(0,b'42\n',b''),observed
output.write_bytes(b'preserve previous publication')
run('explicit-build-preserves-output',['build',app,'--toolchain','msvc','-o',output],broken,1)
assert output.read_bytes()==b'preserve previous publication'
path=json.loads(run('private-config-path',['toolchain','config-path','--json'],broken).stdout)['path']
config=Path(path);config.parent.mkdir(parents=True,exist_ok=True)
config.write_text('schema = 1\n[policy]\nallow_fallback = false\nprefer_external_native = true\nauto_probe = true\n',encoding='utf-8')
before=config.read_bytes();probe('policy-disables-fallback',broken,['--provider','auto'],False)
run('policy-build-preserves-output',['build',app,'--toolchain','auto','-o',output],broken,1)
assert output.read_bytes()==b'preserve previous publication' and config.read_bytes()==before
assert original=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [msvc,msvc.parent/'link.exe']}
(parent/'fixture.json').write_text(json.dumps(dict(root=str(root),copied=copied,original_tools=original,
    cause='Incomplete provider inputs are retained as a non-fallback preflight negative. Complete genuine binaries/configs then compile a private header that rejects only MSVC; Clang compiles the same admitted SDK. Discovery locator is a test fixture.',
    private_probe_and_build=True,msvc_to_clang_only=True,zig_gnu_abi_not_qualified=True),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(dict(status='PASS',commands=len(records),real_msvc_child_failure=True,real_clang_fallback=True,
                     shared_build_selection=True,explicit_and_policy_fail_closed=True,production_release=False)))
