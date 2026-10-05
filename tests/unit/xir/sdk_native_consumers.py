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
    args=parser.parse_args();root=Path(__file__).resolve().parents[3];bundle=Path(args.bundle)
    lease=subprocess.Popen([args.lease,str(bundle),str(bundle/'sdk_manifest.json')],
                           stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    try:
        ready=lease.stdout.readline().strip().split();assert ready[0]=='READY' and ready[1:3]==['19','2'],ready
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
            assert actual==[dict(id=row['id'],value=row['value']) for row in expected] and len(actual)==233
            print(args.provider+': fresh admitted-header 233-field independent ABI PASS',flush=True)
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
    finally:
        lease.stdin.write('q');lease.stdin.flush()
        _,error=lease.communicate(timeout=10);assert not lease.returncode,error


if __name__=='__main__':main()
