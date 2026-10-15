"""Verify imported String updates and scalar templates with owned user fields."""
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
    else:temporary=tempfile.TemporaryDirectory(prefix='string-append-',dir=binary.parent);output=Path(temporary.name)
    records=[]
    try:
        policy=NoChildProcess(root,output);stdlib=output/'stdlib';(stdlib/'types').mkdir(parents=True)
        poison=stdlib/'types/string.xr';poison.write_text('invalid String source must not supply the admitted operation\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1');hidden=dict(env,PATH=str(output/'no-C-provider'))
        (output/'future-text.xr').write_text(
            'fn seed()->Array<i8>{var xs:Array<i8>=[7,11,13];return xs.reserve(8)}\n'
            'export struct TextPacket<T>{value:T;init(value){this.value=value}}\n'
            'export struct TextSeed{items:Array<i8>=comptime seed();init(){}}\n'
            'export fn snapshots()->(string,string){var text="A\\0é";const old=text;text+="中😀";text+=text;return (old,text)}\n'
            'export fn referenced(value:ref string)->string{value+="r";return value}\n'
            'export fn scalarText()->string{var text="";var integer:i64=-17;text+="${integer}";text+="|";var truth=true;text+="${truth}";text+="|";var letter=\'Ω\';text+="${letter}";text+="|";var number:f64=1.5;text+="${number}";text+="|";var wide:u64=18446744073709551615;text+="${wide}";return text}\n'
            'export fn repeated(count:i64)->string{var text="";var i=0;while(i<count){text+="x\\0Ω";i+=1};return text}\n',encoding='utf-8')
        prefix='import {TextPacket,TextSeed,snapshots,referenced,scalarText,repeated} from "./future-text"\n'
        content=(prefix+'const packet=TextPacket<string>("L\\0Ω")\nconst batch=TextSeed()\nvar changed=batch.items\nchanged.push(17)\n'
                 'print(packet.value)\nprint(len(batch.items),batch.items.capacity,len(changed),changed[0],changed[1],changed[2],changed[3])\n'
                 'const pair=snapshots()\nprint(pair.0)\nprint(pair.1)\nprint(scalarText())\n'
                 'var text="q"\nconst old=text\nconst result=referenced(ref text)\ntext+="s"\nprint(old,result,text)\n')
        answer='L\0Ω\n3 8 4 7 11 13 17\nA\0é\n'.encode()+('A\0é中😀'*2+'\n').encode()+ '-17|true|Ω|1.5|18446744073709551615\nq qr qrs\n'.encode()
        repetitions=prefix+'print(repeated(64))\n';repeat_answer=('x\0Ω'*64+'\n').encode()
        cases=[('content',content,answer),('repetitions',repetitions,repeat_answer),
               ('ctfe','fn text()->string{var value="a";value+="b";return value}\nprint(comptime text())\n',b'ab\n')]
        def invoke(name,executable,arguments,environment):
            argv=[str(executable),*arguments]
            result=policy.run(name,argv,environment) if environment is hidden else subprocess.run(argv,cwd=root,env=environment,capture_output=True,timeout=60)
            (output/(name+'.stdout')).write_bytes(result.stdout);(output/(name+'.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name,argv=argv,exit_code=result.returncode,CLI_child_processes_denied=policy.supported and environment is hidden,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output/'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8');return result
        for name,text,expected in cases:
            entry=output/(name+'.xr');entry.write_text(text,encoding='utf-8')
            result=invoke(name+'-vm',binary,['run',str(entry)],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,expected,b''),result
            generated=output/(name+'.c');result=invoke(name+'-c',binary,['build','--c-only',str(entry),'-o',str(generated)],env)
            assert result.returncode==0 and not result.stderr and b'({' not in generated.read_bytes(),result
            native=output/(name+'.exe');result=invoke(name+'-build',binary,['build',str(entry),'-o',str(native)],env)
            assert result.returncode==0 and not result.stderr,result
            result=invoke(name+'-native',native,[],hidden);assert (result.returncode,result.stdout,result.stderr)==(0,expected,b''),result
        poison.unlink()
        for name,_,expected in cases:
            result=invoke(name+'-source-absent',binary,['run',str(output/(name+'.xr'))],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,expected,b''),result
        negatives=[('immutable','const text="a"\ntext+="b"\nprint(text)\n',b'compound assignment requires a mutable binding')]
        for name,text,message in negatives:
            entry=output/(name+'.xr');entry.write_text(text,encoding='utf-8');result=invoke(name,binary,['run',str(entry)],hidden)
            assert result.returncode==1 and not result.stdout and message in result.stderr,result
        (output/'summary.json').write_text(json.dumps(dict(status='STRING_APPEND_OWNED_USER_FIELDS_PASS',commands=len(records),
            String_COW_self_append_Unicode_NUL=True,ref_String_snapshot=True,scalar_templates_i64_bool_Rune_f64_u64=True,
            imported_generic_user_struct=True,nongeneric_owning_Array_CTFE_default_and_capacity8=True,retained_Array_COW=True,
            open_generic_CTFE_default=False,
            full_repeated_UTF8_bytes=256,source_absent_and_poisoned=True,Windows_absolute_child_denial=policy.control,
            immutable_root_refused=True,append_CTFE=True,library_private_var_opened=False,Builder_performance_qualified=False,
            complete_standard_library=False),indent=2)+'\n',encoding='utf-8')
        print('String append: full Unicode/NUL COW, scalar templates and owned generic fields PASS');return 0
    finally:
        if temporary:temporary.cleanup()
if __name__=='__main__':raise SystemExit(main())
