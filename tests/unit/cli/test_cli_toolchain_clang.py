"""Run real clang-cl Source/native probes and builds in private Unicode paths."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,tempfile

exe,clang,msvc,parent,source_root=map(lambda s:Path(s).resolve(),sys.argv[1:6])
parent.mkdir(parents=True,exist_ok=True);records=[]
with tempfile.TemporaryDirectory(prefix='clang-原生-',dir=parent) as temporary:
    root=Path(temporary);env=dict(os.environ,LOCALAPPDATA=str(root/'用户配置'))
    env.update(CL='/DPOISONED_PARENT_CL=1',_CL_='/DPOISONED_PARENT_CL_SUFFIX=1',
               LINK='/POISONED_PARENT_LINK',INCLUDE=str(root/'absent include'),LIB=str(root/'absent lib'))
    def run(name,args,expected=0):
        command=[str(exe),*map(str,args)];r=subprocess.run(command,env=env,capture_output=True,timeout=120)
        row=dict(case=name,argv=command,exit=r.returncode,stdout=r.stdout.decode('utf-8',errors='replace'),stderr=r.stderr.decode('utf-8',errors='replace'))
        records.append(row);(parent/'records.json').write_text(json.dumps(records,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        assert r.returncode==expected,row
        return r
    def probe(name,args=(),ready=True):
        r=run(name,['toolchain','probe','--target','native',*args,'--json'],0 if ready else 3)
        assert not r.stderr,r.stderr;v=json.loads(r.stdout)
        assert set(v)=={'schema','xray','request','selection','capabilities','diagnostics','probe'} and v['schema']==1
        assert set(v['selection'])=={'provider','ownership','compiler','version','targetAbi','runtimeArtifact','ready','fallbackUsed'}
        assert v['selection']['ready'] is ready and v['probe']['cache']=='disabled'
        assert v['selection']['targetAbi']=='x86_64-windows-msvc' and v['selection']['fallbackUsed'] is False
        if ready:
            assert v['selection']['provider']=='clang' and Path(v['selection']['compiler']).samefile(clang)
            assert re.fullmatch(r'\d+\.\d+\.\d+\.\d+',v['selection']['version'])
            assert re.fullmatch(r'[a-f0-9]{64}',v['probe']['fingerprint']) and not v['diagnostics']
            assert v['xray']['sdkDigest']==v['selection']['runtimeArtifact']
            assert all(v['capabilities'][k]=='ok' for k in ['cCompile','sdkCompile','runtimeLink','nativeRun'])
        else:assert v['diagnostics'] and v['capabilities']['nativeRun']!='ok'
        return v
    source=root/'三模块';source.mkdir()
    for name in ['root.xr','counter.xr','label.xr']:shutil.copyfile(source_root/'tests/fixtures/xir_compile_owner'/name,source/name)
    def build(name,options=()):
        output=root/(name+'.exe');run(name,['build',source/'root.xr','-o',output,*options])
        observed=subprocess.run([str(output)],capture_output=True,timeout=30)
        assert (observed.returncode,observed.stdout,observed.stderr)==(0,b'owner\xe4\xb8\xad-ok 42 true\n',b''),observed
    probe('explicit-clang',['--provider','clang','--cc',clang])
    build('显式编译',('--toolchain','clang','--cc',clang))
    selected=json.loads(run('persist-clang',['toolchain','use','--target','native','clang','--cc',clang,'--json']).stdout)
    config=Path(selected['configPath']);before=config.read_bytes()
    probe('persisted-clang');assert config.read_bytes()==before
    build('持久配置编译');assert config.read_bytes()==before
    probe('wrong-explicit-driver',['--provider','clang','--cc',msvc],False);assert config.read_bytes()==before
    output=root/'拒绝时保全.exe';output.write_bytes(b'preserve original output')
    run('missing-explicit-compiler',['build',source/'root.xr','-o',output,'--cc',root/'missing-clang.exe'],1)
    assert output.read_bytes()==b'preserve original output' and config.read_bytes()==before
    run('reset-private',['toolchain','reset','--target','native','--json'])
print(json.dumps(dict(status='PASS',commands=len(records),actual_clang_native_probes=2,actual_clang_source_builds=2,unicode_paths=True,parent_compiler_environment_ignored=True,explicit_no_fallback=True,production_release=False)))
