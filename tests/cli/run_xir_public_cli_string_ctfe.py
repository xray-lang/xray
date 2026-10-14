"""Consume governed owning strings and searches through imported generic CTFE."""
from __future__ import annotations
import argparse,hashlib,json,os,subprocess,tempfile
from pathlib import Path
from xir_no_child_process import NoChildProcess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--output',type=Path);args=parser.parse_args()
    root,binary=args.root.resolve(),args.binary.resolve();binary.relative_to(root);temporary=None
    if args.output:output=args.output.resolve();output.relative_to(root);output.mkdir(parents=True,exist_ok=False)
    else:temporary=tempfile.TemporaryDirectory(prefix='string-ctfe-',dir=binary.parent);output=Path(temporary.name)
    records=[]
    try:
        process_policy=NoChildProcess(root,output)
        stdlib=output/'stdlib';(stdlib/'types').mkdir(parents=True)
        poison=stdlib/'types/string.xr';poison.write_text('invalid string source must never supply CTFE semantics\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1');hidden=dict(env,PATH=str(output/'no-C-provider'))
        module=('export fn copied(value:string)->string { return value.toString() }\n'
                'export fn repeated(value:string,n:i64)->string { return value.repeat(n) }\n'
                'export fn echo<T>(value:T)->T { return value }\n'
                'export fn genericCopy(value:string)->string { return echo<string>(value).toString() }\n'
                'export fn nested(value:string,n:i64)->string { return repeated(copied(value),n) }\n'
                'export fn search(value:string,pattern:string,start:i64)->i64 { return value.indexOf(pattern,start) }\n'
                'export fn last(value:string,pattern:string)->i64 { return value.lastIndexOf(pattern) }\n'
                'export fn predicates(value:string,pattern:string)->bool { return value.contains(pattern) && value.startsWith(pattern) && value.endsWith(pattern) }\n')
        (output/'future-text.xr').write_text(module,encoding='utf-8')
        prefix='import {copied,repeated,genericCopy,nested,search,last,predicates} from "./future-text"\n'
        text=prefix;expected=bytearray();pairs=[('',9223372036854775807),('a',-9223372036854775808),('ab',0),('ab',3),('é',3),('😀',1),('a\0',3)]
        def spelling(value):return json.dumps(value,ensure_ascii=False).replace('\\u0000','\\0')
        for word,count in pairs:
            source_word=spelling(word)
            text+=f'print(comptime copied({source_word}),comptime genericCopy({source_word}),comptime nested({source_word},{count}))\n'
            expected.extend((word+' '+word+' '+word*max(0,count)+'\n').encode())
        reads=prefix;read_expected=bytearray();queries=[('é😀é','é',0),('é😀é','é',1),('a\0ba\0','\0',0),('abc','z',0),('abc','',2),('','',0)]
        for word,pattern,start in queries:
            reads+=f'print(comptime search({spelling(word)},{spelling(pattern)},{start}),comptime last({spelling(word)},{spelling(pattern)}),comptime predicates({spelling(pattern)},{spelling(pattern)}))\n'
            read_expected.extend(f'{word.find(pattern,start)} {word.rfind(pattern)} true\n'.encode())
        cases=[('owned',text,bytes(expected)),('reads',reads,bytes(read_expected))]
        def invoke(name,executable,arguments,environment):
            argv=[str(executable),*arguments]
            result=(process_policy.run(name,argv,environment) if environment is hidden else
                    subprocess.run(argv,cwd=root,env=environment,capture_output=True,timeout=60))
            (output/(name+'.stdout')).write_bytes(result.stdout);(output/(name+'.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name,argv=argv,exit_code=result.returncode,
                CLI_child_processes_denied=process_policy.supported and environment is hidden,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output/'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8');return result
        for name,source_text,answer in cases:
            entry=output/(name+'.xr');entry.write_text(source_text,encoding='utf-8')
            result=invoke(name+'-vm',binary,['run',str(entry)],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,answer,b''),result
            generated=output/(name+'.c');result=invoke(name+'-c',binary,['build','--c-only',str(entry),'-o',str(generated)],env)
            assert result.returncode==0 and not result.stderr and b'({' not in generated.read_bytes(),result
            native=output/(name+'.exe');result=invoke(name+'-build',binary,['build',str(entry),'-o',str(native)],env)
            assert result.returncode==0 and not result.stderr,result
            result=invoke(name+'-native',native,[],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,answer,b''),result
        poison.unlink()
        for name,_,answer in cases:
            result=invoke(name+'-source-absent',binary,['run',str(output/(name+'.xr'))],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,answer,b''),result
        for name,source_text,diagnostic in [('negative-start',prefix+'print(comptime search("a","",-1))\n',b'comptime value range violation'),
                                           ('past-end',prefix+'print(comptime search("é","",2))\n',b'comptime value range violation'),
                                           ('float-text', 'fn text(value:f64)->string { return value.toString() }\nprint(comptime text(1.5))\n',b'admitted deterministic scalar direct-call closure')]:
            entry=output/(name+'.xr');entry.write_text(source_text,encoding='utf-8');result=invoke(name,binary,['run',str(entry)],hidden)
            assert result.returncode==1 and not result.stdout and diagnostic in result.stderr,result
        (output/'summary.json').write_text(json.dumps(dict(status='STRING_CTFE_IMPORT_PASS',commands=len(records),owning_cases=len(pairs),
            owned_direct_generic_nested_calls=len(pairs)*3,read_cases=len(queries),read_ctfe_calls=len(queries)*3,Unicode_NUL=True,
            source_poisoned_and_absent=True,VM_C_provider_PATH_hidden=True,user_generic_scalar_instantiation=True,
            Windows_absolute_child_denial=process_policy.control,
            user_aggregate_ctfe=False,library_generic_templates=False,numeric_TO_STRING_ctfe=False,complete_standard_library=False),indent=2)+'\n',encoding='utf-8')
        print('String CTFE: imported owning/generic/copy/repeat, Unicode/NUL searches and exact range refuses PASS');return 0
    finally:
        if temporary:temporary.cleanup()
if __name__=='__main__':raise SystemExit(main())
