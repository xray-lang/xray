"""Account actual runtime archive objects and force their complete link closure."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build','compiler','linker','archiver','provider','output'):
        parser.add_argument('--'+name,required=True)
    args=parser.parse_args();assert args.provider in ('msvc','clang')
    root=Path(__file__).resolve().parents[3];build=Path(args.build).resolve(strict=True)
    parent=Path(args.output).resolve();assert parent!=build and parent.is_relative_to(build)
    parent.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix=args.provider+'-',dir=parent));commands=[]
    environment=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',PYTHONIOENCODING='utf-8')
    environment.pop('XRAY_STDLIB_PATH',None)
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

    def run(label,argv):
        result=subprocess.run(list(map(str,argv)),cwd=out,env=environment,
                              capture_output=True,timeout=120)
        (out/(label+'.stdout')).write_bytes(result.stdout)
        (out/(label+'.stderr')).write_bytes(result.stderr)
        commands.append(dict(name=label,argv=list(map(str,argv)),exit_code=result.returncode))
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n',encoding='utf-8')
        assert not result.returncode,(label,result.returncode,result.stdout,result.stderr)
        return result.stdout

    sdk=build/'xir-runtime-sdk';manifest=json.loads((sdk/'sdk_manifest.json').read_text(encoding='utf-8'))
    recipe=json.loads((sdk/'sdk_recipe.json').read_text(encoding='utf-8'))
    assert manifest['crt']==2 and manifest['sanitizers']==0
    groups=recipe['archive_source_groups'];assert len(groups)==5
    assert set(groups)=={'xray_compile_resources','xray_xir_admission','xray_xir_declarations',
                         'xray_xir_scalar','xray_xir_runtime_host'}
    assert [r['path'] for r in recipe['files']]==[r['path'] for r in manifest['files']]
    inputs={str(sdk/r['path']):r['sha256'] for r in manifest['files']}
    inputs[str(sdk/'sdk_manifest.json')]=sha(sdk/'sdk_manifest.json')
    inputs[str(sdk/'sdk_recipe.json')]=sha(sdk/'sdk_recipe.json')
    assert all(sha(Path(p))==h for p,h in inputs.items())
    for row in manifest['files']:
        if row['kind']!=5:
            assert (sdk/row['path']).read_bytes()==(root/row['path']).read_bytes(),row['path']
    narrow=out/'runtime-only';narrow.mkdir()
    for row in manifest['files']:
        destination=narrow/row['path'];destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(sdk/row['path'],destination)
    shutil.copyfile(sdk/'sdk_manifest.json',narrow/'sdk_manifest.json')
    assert {p.relative_to(narrow).as_posix() for p in narrow.rglob('*') if p.is_file()}==set(recipe_row['path'] for recipe_row in recipe['files'])|{'sdk_manifest.json'}
    assert not any(p.suffix in ('.xr','.chk','.exe','.dll') for p in narrow.rglob('*'))

    database=json.loads(run('actual-ninja-compdb',['ninja','-C',sdk/'build','-t','compdb']))
    compiled={Path(r['output'].replace('\\','/')).as_posix():r for r in database
              if r['file'].endswith('.c') and any('CMakeFiles/'+name+'.dir/' in r['output'].replace('\\','/') for name in groups)}
    dependencies=run('actual-ninja-deps',['ninja','-C',sdk/'build','-t','deps']).decode('utf-8')
    header_records={}
    for line in dependencies.replace('\\','/').splitlines():
        match=re.fullmatch(r'(.+): #deps (\d+),.*\((VALID|STALE)\)',line)
        if match and match[1] in compiled:
            assert int(match[2])>0 and match[3]=='VALID',line
            header_records[match[1]]=int(match[2])
    assert set(header_records)==set(compiled)
    inventory=[];objects=out/'objects';objects.mkdir()
    forbidden=('src/frontend/','src/vm/','src/program/','src/aot/','src/api/','src/runtime/','src/coro/','stdlib/')
    for row in manifest['files']:
        assert not row['path'].startswith(forbidden),row['path']
        backend=row['path'].startswith(('src/xir/xxir_vm','src/xir/xxir_emit',
                                        'src/xir/xxir_ctfe','src/xir/xxir_source'))
        assert not backend or row['path']=='src/xir/xxir_source_facts.inc.c',row['path']
    for name,units in groups.items():
        archive=sdk/'lib'/(name+'.lib')
        members=run('members-'+name,[args.archiver,'/nologo','/list',archive]).decode('utf-8').splitlines()
        assert len(members)==len(units) and len(set(members))==len(members)
        rows=[]
        for index,member in enumerate(members):
            relative=member.replace('\\','/');assert relative in compiled,relative
            command=compiled[relative];source=Path(command['file']).resolve(strict=True)
            unit=source.relative_to(sdk).as_posix()
            assert unit in units and not unit.startswith(forbidden),unit
            assert re.search(r'(?:^|\s)[/-]MD(?:\s|$)',command['command'])
            assert not re.search(r'(?:^|\s)[/-](?:MT|MDd|MTd)(?:\s|$)|fsanitize=',command['command'])
            assert source.read_bytes()==(root/unit).read_bytes()
            original=(sdk/'build'/relative).resolve(strict=True)
            assert original.is_relative_to((sdk/'build').resolve())
            extracted=objects/(name+'-'+str(index)+'.obj')
            run('extract-'+name+'-'+str(index),[args.archiver,'/nologo','/extract:'+member,
                                              '/out:'+str(extracted),archive])
            assert extracted.read_bytes()==original.read_bytes()
            rows.append(dict(member=member,source=unit,object_sha256=sha(extracted),
                             source_sha256=sha(source),actual_compile_command=command['command']))
        assert {r['source'] for r in rows}==set(units)
        inventory.append(dict(archive=name,members=rows))
    assert sum(len(r['members']) for r in inventory)==len(compiled)
    (out/'actual-object-inventory.json').write_text(json.dumps(inventory,indent=2)+'\n',encoding='utf-8')

    sys.path.insert(0,str(root/'scripts'))
    from derive_xir_sdk_abi import prepare
    proof=out/'independent-abi';prepare(proof)
    expected=json.loads((proof/'EXPECTED.json').read_text(encoding='utf-8'))['rows']
    expected=[dict(id=r['id'],value=r['value']) for r in expected]
    (out/'expected-before-provider.json').write_text(json.dumps(expected,indent=2)+'\n',encoding='utf-8')
    obj=out/'abi.obj';exe=out/'whole-runtime.exe';link_map=out/'whole-runtime.map'
    if args.provider=='msvc':
        command=[args.compiler,'/nologo','/std:c11','/utf-8','/experimental:c11atomics',
                 '/MD','/O2','/W4','/WX','/DNDEBUG','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN',
                 '/D_CRT_SECURE_NO_WARNINGS','/I'+str(narrow/'src'),'/c',proof/'abi_probe.c','/Fo'+str(obj)]
    else:
        command=[args.compiler,'--target=x86_64-windows-msvc','-std=c11','-O2','-Wall','-Wextra',
                 '-Werror','-pedantic-errors','-DNDEBUG','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN',
                 '-D_CRT_SECURE_NO_WARNINGS','-fms-runtime-lib=dll','-isystem',narrow/'src',
                 '-c',proof/'abi_probe.c','-o',obj]
    run('compile-independent-abi',command)
    libraries=[narrow/'lib'/(name+'.lib') for name in groups]
    assert manifest['system_libraries']==['kernel32']
    run('force-all-objects-link',[args.linker,'/nologo','/incremental:no','/opt:noref','/opt:noicf',
                                 '/verbose:lib','/map:'+str(link_map),'/out:'+str(exe),obj,
                                 *['/wholearchive:'+str(p) for p in libraries],
                                 'kernel32.lib','/defaultlib:MSVCRT','/defaultlib:OLDNAMES'])
    link_log=(out/'force-all-objects-link.stdout').read_text(encoding='utf-8',errors='strict')
    loaded={name.lower() for name in re.findall(r'\b([a-z_0-9]+\.lib)\b',link_log,re.I)}
    allowed={name.lower()+'.lib' for name in groups}
    allowed.update({'kernel32.lib','msvcrt.lib','oldnames.lib','vcruntime.lib','ucrt.lib'})
    assert loaded==allowed,loaded
    actual=json.loads(run('run-complete-link',[exe]));assert actual==expected
    mapping=link_map.read_text(encoding='utf-8');mapped=[]
    for archive in inventory:
        for row in archive['members']:
            basename=Path(row['member'].replace('\\','/')).name
            assert basename in mapping,(archive['archive'],basename)
            mapped.append(dict(archive=archive['archive'],object=basename))
    assert all(sha(Path(p))==h for p,h in inputs.items())
    assert all(sha(narrow/row['path'])==row['sha256'] for row in manifest['files'])
    summary=dict(status='PASS',provider=args.provider,archives=len(inventory),
                 actual_objects=len(compiled),mapped_objects=len(mapped),ABI_rows=len(expected),
                 SDK_files=len(manifest['files'])+1,manifest_sha256=sha(sdk/'sdk_manifest.json'),
                 archive_bytes_equal_actual_ninja_objects=True,source_bytes_equal_owned_recipe_units=True,
                 all_recipe_source_and_header_bytes_match_checkout=True,valid_header_dependency_records=len(header_records),
                 no_frontend_VM_CGen_legacy_or_stdlib_cache_sources=True,
                 wholearchive_all_five=True,linker_reference_and_identical_code_elimination_disabled=True,
                 loaded_libraries=sorted(loaded),
                 pure_prefix_without_Core_CLI_or_Source_SDK=True,all_input_guards=True,
                 actual_inputs=inputs,output=str(out),
                 scope='Actual runtime source/object/link closure; no arbitrary native body proof, fullFI, mixed or production qualification',
                 production_release=False)
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k!='actual_inputs'}))


if __name__=='__main__':main()
