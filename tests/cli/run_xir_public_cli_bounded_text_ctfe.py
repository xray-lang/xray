"""Verify imported private Array paths and bounded scalar text constants."""
from __future__ import annotations
import argparse, hashlib, json, os, subprocess, tempfile
from pathlib import Path
from xir_no_child_process import NoChildProcess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--output',type=Path);args=parser.parse_args()
    root,binary=args.root.resolve(),args.binary.resolve();binary.relative_to(root);temporary=None
    if args.output:output=args.output.resolve();output.relative_to(root);output.mkdir(parents=True,exist_ok=False)
    else:temporary=tempfile.TemporaryDirectory(prefix='bounded-text-ctfe-',dir=binary.parent);output=Path(temporary.name)
    records=[]
    try:
        policy=NoChildProcess(root,output);stdlib=output/'stdlib';(stdlib/'types').mkdir(parents=True)
        poisons=[stdlib/'types'/name for name in ['Array.xr','string.xr','rune.xr','f64.xr','i64.xr','bool.xr']]
        for path in poisons:path.write_text('invalid declaration source must not supply compiler constants\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1');hidden=dict(env,PATH=str(output/'no-C-provider'))
        module=(
            'fn identity<T>(value:T)->T{return value}\n'
            'export fn paths()->(Array<i8>,Array<i8>,i8){var xs:Array<Array<i8>>=[[2,3],[5]];const old=xs;'
            'xs[0][1]=7;xs[1].push(11);xs[0].reserve(8);return (xs[0],old[0],xs[1][1])}\n'
            'export fn ordering()->bool{var xs:Array<Array<i8>>=[[1,2,3]];const old=xs;'
            'xs[0].reverse();xs[0].unshift(7);const a:Array<i8>=[7,3,2,1];const b:Array<i8>=[1,2,3];'
            'return xs[0]==a && old[0]==b && old.contains(b) && old.indexOf(b)==0}\n'
            'export fn filled()->bool{var xs:Array<Array<string>>=[["a","b","c"]];const old=xs;'
            'xs[0].fill("x\\0y",1,2);xs[0].fill("unused",3,3);'
            'const nan:Array<f64>=[0.0/0.0];return xs[0][0]=="a" && xs[0][1]=="x\\0y" && '
            'xs[0][2]=="c" && old[0][1]=="b" && !nan.contains(nan[0])}\n'
            'export fn integers()->string{const xs:Array<i8>=[-7,12];return identity<string>(xs.join(":"))}\n'
            'export fn floats()->string{const xs:Array<f64>=[3.5,-0.0];return xs.join(",")}\n'
            'export fn unicode()->string{const xs:Array<string>=["A\\0","中"];return xs.join("|")}\n'
            'export fn runeText()->string{return rune(128512).toString()}\n'
            'export fn boolText()->string{return true.toString()}\n'
            'export fn wide()->bool{const signed:i64=-9223372036854775807-1;const unsigned:u64=18446744073709551615;'
            'const tiny:f64=5e-324;const maximum:f64=1.7976931348623157e308;'
            'return signed.toString()=="-9223372036854775808" && unsigned.toString()=="18446744073709551615" && '
            'tiny.toString()=="5e-324" && maximum.toString()=="1.7976931348623157e+308"}\n')
        (output/'future-text-values.xr').write_text(module,encoding='utf-8')
        prefix='import {paths,ordering,filled,integers,floats,unicode,runeText,boolText,wide} from "./future-text-values"\n'
        arrays=prefix+('const pair=comptime paths()\nprint(pair.0[0],pair.0[1],pair.1[0],pair.1[1],pair.2,pair.0.capacity>=8)\n'
                       'print(comptime ordering(),comptime filled())\n')
        text=prefix+('print(comptime integers())\nprint(comptime floats())\nprint(comptime unicode())\n'
                     'print(comptime runeText(),comptime boolText(),comptime wide())\n')
        cases=[('wide-output','fn bad()->string{return (9223372036854775807 as i64).toString()}\nprint(comptime bad())\n',b'9223372036854775807\n'),('paths',arrays,b'2 7 2 3 11 true\ntrue true\n'),
               ('text',text,'-7:12\n3.5,-0.0\nA\0|中\n😀 true true\n'.encode('utf-8'))]
        def invoke(name,executable,arguments,environment):
            argv=[str(executable),*arguments]
            result=policy.run(name,argv,environment) if environment is hidden else subprocess.run(argv,cwd=root,env=environment,capture_output=True,timeout=60)
            (output/(name+'.stdout')).write_bytes(result.stdout);(output/(name+'.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name,argv=argv,exit_code=result.returncode,CLI_child_processes_denied=policy.supported and environment is hidden,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output/'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8');return result
        for name,source,expected in cases:
            entry=output/(name+'.xr');entry.write_text(source,encoding='utf-8')
            result=invoke(name+'-vm',binary,['run',str(entry)],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,expected,b''),result
            generated=output/(name+'.c');result=invoke(name+'-c',binary,['build','--c-only',str(entry),'-o',str(generated)],env)
            assert result.returncode==0 and not result.stderr and b'({' not in generated.read_bytes(),result
            native=output/(name+'.exe');result=invoke(name+'-build',binary,['build',str(entry),'-o',str(native)],env)
            assert result.returncode==0 and not result.stderr,result
            result=invoke(name+'-native',native,[],hidden);assert (result.returncode,result.stdout,result.stderr)==(0,expected,b''),result
        for path in poisons:path.unlink()
        for name,_,expected in cases:
            result=invoke(name+'-source-absent',binary,['run',str(output/(name+'.xr'))],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,expected,b''),result
        negatives=[
            ('bounds','fn bad()->i8{var xs:Array<Array<i8>>=[[1]];xs[0][2]=3;return 0}\nprint(comptime bad())\n',b'comptime index out of bounds'),
            ('fill-range','fn bad()->i64{var xs:Array<Array<i8>>=[[1,2]];xs[0].fill(7,-1,2);return 42}\nprint(comptime bad())\n',b'comptime value range violation'),
            ('io','fn bad()->string{print(1);return true.toString()}\nprint(comptime bad())\n',b'admitted deterministic scalar direct-call closure'),
            ('append','fn bad()->string{var value:string="a";value+="b";return value}\nprint(comptime bad())\n',b'admitted deterministic scalar direct-call closure'),
            ('private-import','import {identity} from "./future-text-values"\nprint(identity<i64>(7))\n',b'import requires an exported declaration')]
        for name,source,message in negatives:
            entry=output/(name+'.xr');entry.write_text(source,encoding='utf-8');result=invoke(name,binary,['run',str(entry)],hidden)
            assert result.returncode==1 and not result.stdout and message in result.stderr,result
        (output/'summary.json').write_text(json.dumps(dict(status='BOUNDED_TEXT_ARRAY_CTFE_IMPORT_PASS',commands=len(records),
            private_nested_Array_paths=True,COW_old_snapshots=True,owning_capacity_preserved=True,ordinary_Array_methods=True,
            scalar_TO_STRING_and_numeric_join=True,Unicode_NUL=True,full_binary64_and_integer_endpoint_spelling=True,
            source_absent_and_poisoned=True,Windows_absolute_child_denial=policy.control,
            output_policy_version=2,maximum_logical_output_bytes=8*1024*1024,IO_and_append_CTFE_refused=True,private_import_refused=True,
            open_generic_CTFE_defaults=False,foreign_aggregate_inputs=False,user_class_CTFE=False,
            library_private_var_opened=False,complete_standard_library=False),indent=2)+'\n',encoding='utf-8')
        print('Bounded scalar text and private Array paths: independent exact bytes, COW/capacity, native and refusals PASS');return 0
    finally:
        if temporary:temporary.cleanup()


if __name__=='__main__':raise SystemExit(main())
