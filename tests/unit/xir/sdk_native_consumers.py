"""Execute original Source inputs against a production-held same-source SDK."""
from pathlib import Path
import argparse, json, re, subprocess, sys, tempfile


def checked(command,**kwargs):
    result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=120,**kwargs)
    assert not result.returncode,(command,(result.stdout+result.stderr).decode(errors='replace'))
    return result.stdout


def compile_source(args,sdk,source,object_file,program_symbol=None):
    if args.provider=='msvc':
        command=[args.compiler,'/nologo','/std:c11','/utf-8','/experimental:c11atomics',
                 '/MD','/O2','/W4','/WX','/DNDEBUG','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN',
                 '/D_CRT_SECURE_NO_WARNINGS','/I'+str(sdk/'src'),'/I'+str(sdk/'include'),
                 '/I'+str(sdk/'generated'),'/c',str(source),'/Fo'+str(object_file)]
    else:
        command=[args.compiler,*(['cc'] if args.provider=='zig' else []),
                 '--target=x86_64-windows-msvc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                 '-pedantic-errors','-DNDEBUG','-DNOMINMAX','-DWIN32_LEAN_AND_MEAN',
                 '-D_CRT_SECURE_NO_WARNINGS','-D_DLL','-D_MT']
        if args.provider=='clang':command.append('-fms-runtime-lib=dll')
        for directory in ('src','include','generated'):command+=['-isystem',str(sdk/directory)]
        command+=['-c',str(source),'-o',str(object_file)]
    if program_symbol:
        command.append(('/D' if args.provider=='msvc' else '-D')+'XIR_SDK_PROGRAM_SYMBOL='+program_symbol)
    checked(command)


def link(args,executable,objects,libraries):
    checked([args.linker,'/nologo','/incremental:no','/out:'+str(executable),*objects,*libraries,
             'kernel32.lib','/defaultlib:MSVCRT','/defaultlib:OLDNAMES'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('lease','generator','bundle','stdlib','compiler','linker'):parser.add_argument('--'+name,required=True)
    parser.add_argument('--provider',choices=('msvc','clang','zig'),required=True)
    parser.add_argument('--original-generator')
    parser.add_argument('--hash-generator')
    parser.add_argument('--index-generator')
    parser.add_argument('--projection-generator')
    parser.add_argument('--algebra-generator')
    parser.add_argument('--membership-generator')
    parser.add_argument('--foreach-generator')
    parser.add_argument('--foreach-matrix-generator')
    parser.add_argument('--worker-generator')
    parser.add_argument('--key-path-generator')
    args=parser.parse_args();root=Path(__file__).resolve().parents[3];bundle=Path(args.bundle)
    lease=subprocess.Popen([args.lease,str(bundle),str(bundle/'sdk_manifest.json')],
                           stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    try:
        ready=lease.stdout.readline().strip().split();assert ready[0]=='READY' and ready[1:3]==['26','2'],ready
        sdk=Path(lease.stdout.readline().strip());libraries=[lease.stdout.readline().strip() for _ in range(5)]
        assert sdk.is_dir() and all(Path(name).is_file() for name in libraries)
        with tempfile.TemporaryDirectory(prefix='xir-sdk-native-') as temporary:
            work=Path(temporary);consumer=root/'tests/unit/xir/xir_sdk_consumer.c'
            sys.path.insert(0,str(root/'scripts'))
            from derive_xir_sdk_abi import prepare
            proof=work/'abi';prepare(proof);probe_object=proof/'abi.obj';probe_exe=proof/'abi.exe'
            compile_source(args,sdk,proof/'abi_probe.c',probe_object)
            link(args,probe_exe,[str(probe_object)],libraries)
            expected=json.loads((proof/'EXPECTED.json').read_text())['rows']
            actual=json.loads(checked([str(probe_exe)]))
            assert actual==[dict(id=row['id'],value=row['value']) for row in expected]
            print(args.provider+': fresh admitted-header '+str(len(expected))+'-field independent ABI PASS',flush=True)
            cases=[('multi',root/'tests/fixtures/xir_compile_owner/root.xr',b'owner\xe4\xb8\xad-ok 42 true\n',
                    args.generator,'multi','linked_source_program')]
            if args.original_generator:
                cases += [('retired',root/'tests/unit/fixtures/assertion/retired_names_are_ordinary.xr',b'',
                           args.original_generator,'empty','original_source_program'),
                          ('array',root/'tests/unit/fixtures/runtime/array_growth_accounting.xr',b'array-growth-accounting-ok\n',
                           args.original_generator,'array','original_source_program')]
            for name,path,expected,generator,mode,symbol in cases:
                generated=work/(name+'.c')
                checked([generator,str(path.parent),str(path),args.stdlib,'accept',str(generated),mode,'normal'])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,consumer):
                    object_file=work/(name+'-'+source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file,symbol if source==consumer else None)
                executable=work/(name+'.exe')
                link(args,executable,objects,libraries)
                output=checked([str(executable)]).replace(b'\r\n',b'\n');assert output==expected,(name,output,expected)
                print(args.provider+' '+name+': actual C11 + admitted five archives + owned host result PASS',flush=True)
            if args.hash_generator:
                generated=work/'hash-members.c'
                checked([args.hash_generator,str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_hash_members_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'hash-members.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'hash-members-sdk-ok\n'
                print(args.provider+' hash members: actual Source/C11 + admitted archives + two Instances + late Nullable PASS',flush=True)
            if args.index_generator:
                generated=work/'map-index.c'
                checked([args.index_generator,str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_map_index_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'map-index.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'map-index-sdk-ok\n'
                print(args.provider+' Map index: actual Source/C11 + admitted archives + E0431 + late PanicInfo PASS',flush=True)
            if args.projection_generator:
                generated=work/'hash-projection.c'
                checked([args.projection_generator,str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_hash_projection_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'hash-projection.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'hash-projection-sdk-ok\n'
                print(args.provider+' ordered hash projections: actual Source/C11 + admitted archives + late owned arrays/tuples PASS',flush=True)
            if args.algebra_generator:
                generated=work/'set-algebra.c'
                checked([args.algebra_generator,str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_set_algebra_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'set-algebra.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'set-algebra-sdk-ok\n'
                print(args.provider+' Set algebra: actual Source/C11 + admitted archives + owned late Set results PASS',flush=True)
            if args.membership_generator:
                generated=work/'map-membership.c'
                checked([args.membership_generator,str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_map_membership_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'map-membership.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'map-membership-sdk-ok\n'
                print(args.provider+' Map value membership: actual Source/C11 + admitted archives + owned late Map/Array results PASS',flush=True)
            if args.foreach_generator:
                generated=work/'hash-foreach.c'
                checked([args.foreach_generator,str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_hash_foreach_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'hash-foreach.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'hash-foreach-sdk-ok\n'
                print(args.provider+' Map/Set forEach: actual Source/C11 + admitted archives + authenticated suspension and owned late results PASS',flush=True)
            if args.foreach_matrix_generator:
                generated=work/'hash-foreach-matrix.c'
                checked([args.foreach_matrix_generator,str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_hash_foreach_matrix_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'hash-foreach-matrix.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'hash-foreach-matrix-sdk-ok\n'
                print(args.provider+' All four Map/Set traversal graphs: actual Source/C11 + admitted archives + authenticated suspension and owned late results PASS',flush=True)
            if args.worker_generator:
                generated=work/'hash-worker.c'
                checked([args.worker_generator,str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_hash_worker_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'hash-worker.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'hash-worker-sdk-ok\n'
                print(args.provider+' Map/Set worker transport: actual Source/C11 + admitted archives + authenticated suspension and owned late results PASS',flush=True)
            if args.key_path_generator:
                generated=work/'map-key-path.c'
                checked([args.key_path_generator,'--emit',str(generated)])
                assert not re.search(r'\(\s*\{',generated.read_text(encoding='utf-8'))
                objects=[]
                for source in (generated,root/'tests/unit/xir/xir_sdk_map_key_path_consumer.c'):
                    object_file=work/(source.stem+'.obj');objects.append(str(object_file))
                    compile_source(args,sdk,source,object_file)
                executable=work/'map-key-path.exe';link(args,executable,objects,libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'map-key-path-sdk-ok\n'
                print(args.provider+' All eight typed Map key path graphs: actual Source/C11 + admitted archives + authenticated suspension and owned late results PASS',flush=True)
    finally:
        lease.stdin.write('q');lease.stdin.flush()
        _,error=lease.communicate(timeout=10);assert not lease.returncode,error


if __name__=='__main__':main()
