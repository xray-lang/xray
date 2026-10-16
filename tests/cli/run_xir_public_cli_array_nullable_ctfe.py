"""Verify imported Array COW/capacity and active Nullable compiler constants."""
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
    else:temporary=tempfile.TemporaryDirectory(prefix='array-nullable-ctfe-',dir=binary.parent);output=Path(temporary.name)
    records=[]
    try:
        policy=NoChildProcess(root,output);stdlib=output/'stdlib';(stdlib/'types').mkdir(parents=True)
        poison=stdlib/'types/Array.xr';poison.write_text('invalid library source must not implement compiler values\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1');hidden=dict(env,PATH=str(output/'no-C-provider'))
        module=('export fn cow()->(Array<i8>,Array<i8>){var xs:Array<i8>=[1,2];const old=xs;xs[0]=7;xs.push(3);return (xs,old)}\n'
                'export fn room()->Array<i8>{return Array<i8>.withCapacity(7)}\n'
                'export fn repeat()->Array<i8>{return Array<i8>(3,7)}\n'
                'export fn optional(flag:bool)->string?{return flag ? "é\\0" : null}\n'
                'fn layer<T>(value:T)->T?{return value}\n'
                'export fn nested()->i8??{return layer<i8?>(null)}\n'
                'export fn flag(value:bool)->bool?{return layer<bool>(value)}\n')
        (output/'future-values.xr').write_text(module,encoding='utf-8')
        prefix='import {cow,room,repeat,optional,nested,flag} from "./future-values"\n'
        arrays=(prefix+'const pair=comptime cow()\nprint(pair.0[0],pair.0[1],pair.0[2],pair.1[0],pair.1[1])\n'
                'const spare=comptime room()\nprint(len(spare),spare.capacity)\n'
                'const copies=comptime repeat()\nprint(copies[0],copies[1],copies[2])\n')
        nullable=(prefix+'const text=comptime optional(true)\nprint(text!)\n'
                  'const empty=comptime optional(false)\nprint(empty==null)\n'
                  'const wrapped=comptime nested()\nprint(wrapped==null,wrapped! == null)\n'
                  'const present=comptime flag(false)\nprint(present!)\n')
        cases=[('wide-output','fn bad()->Array<i8>{return Array<i8>(9,1)}\nconst xs=comptime bad()\nprint(len(xs))\n',b'9\n'),('arrays',arrays,b'7 2 3 1 2\n0 7\n7 7 7\n'),('nullable',nullable,'é\0\ntrue\nfalse true\nfalse\n'.encode())]
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
        negatives=[('none','fn bad()->i8{const n:i8?=null;return n!}\nprint(comptime bad())\n',b'comptime null unwrap'),
                   ('bounds','fn bad()->i8{const xs:Array<i8>=[1];return xs[-1]}\nprint(comptime bad())\n',b'comptime index out of bounds'),
                   ('deep','fn bad()->Array<Array<i8?>>{return [[null,7]]}\nconst xs=comptime bad()\nprint(len(xs))\n',b'comptime result must belong to an admitted closed value family'),
                   ('io','fn bad()->Array<i8>{print(1);return [1]}\nconst xs=comptime bad()\nprint(len(xs))\n',b'admitted deterministic scalar direct-call closure')]
        for name,text,message in negatives:
            entry=output/(name+'.xr');entry.write_text(text,encoding='utf-8');result=invoke(name,binary,['run',str(entry)],hidden)
            assert result.returncode==1 and not result.stdout and message in result.stderr,result
        (output/'summary.json').write_text(json.dumps(dict(status='ARRAY_NULLABLE_CTFE_IMPORT_PASS',commands=len(records),
            private_Array_Cell_COW=True,old_snapshot_preserved=True,empty_capacity_7_preserved=True,repeat=True,
            Nullable_String_Unicode_NUL=True,Some_None_distinct_from_None=True,user_generic_primitive_Nullable=True,
            None_unwrap_refused=True,negative_index_refused=True,output_policy_version=2,maximum_logical_output_bytes=8*1024*1024,deep_dynamic_Array_refused=True,
            source_absent_and_poisoned=True,Windows_absolute_child_denial=policy.control,
            foreign_aggregate_inputs=False,user_class_CTFE=False,library_private_var_opened=False,complete_standard_library=False),indent=2)+'\n',encoding='utf-8')
        print('Array/Nullable CTFE: exact COW snapshots, capacity and active variants; bounds and closure refusals PASS');return 0
    finally:
        if temporary:temporary.cleanup()
if __name__=='__main__':raise SystemExit(main())
