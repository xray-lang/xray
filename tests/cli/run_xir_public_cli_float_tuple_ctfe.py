"""Verify imported governed Float methods and bounded owning Tuple constants."""
from __future__ import annotations
import argparse,hashlib,json,os,subprocess,tempfile
from fractions import Fraction
from pathlib import Path
from xir_no_child_process import NoChildProcess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--output',type=Path);args=parser.parse_args()
    root,binary=args.root.resolve(),args.binary.resolve();binary.relative_to(root);temporary=None
    if args.output:output=args.output.resolve();output.relative_to(root);output.mkdir(parents=True,exist_ok=False)
    else:temporary=tempfile.TemporaryDirectory(prefix='float-tuple-ctfe-',dir=binary.parent);output=Path(temporary.name)
    records=[]
    try:
        policy=NoChildProcess(root,output);stdlib=output/'stdlib';(stdlib/'types').mkdir(parents=True)
        poison=stdlib/'types/f64.xr';poison.write_text('invalid builtin source must not supply compiler operations\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1');hidden=dict(env,PATH=str(output/'no-C-provider'))
        module=('export fn trunc(x:f64)->i64{return x.toI64()}\n'
                'export fn down(x:f64)->i64{return x.floor()}\n'
                'export fn up(x:f64)->i64{return x.ceil()}\n'
                'export fn rounded(x:f64)->i64{return x.round()}\n'
                'export fn positive(x:f64)->bool{return x.abs()>=0.0}\n'
                'export fn notnumber(x:f64)->bool{return x.isNaN()}\n'
                'export fn ieee()->bool{const n=0.0/0.0;const i=1.0/0.0;const z=-0.0;return n.isNaN() && n.abs().isNaN() && (-i).abs()==i && 1.0/z.abs()==i}\n'
                'export fn constants()->(i16,i16,i16,i16){return (2,3,5,7)}\n'
                'export fn nested()->((i8,()),string){var t:((i8,()),string)=((1,()),"x");t=((7,()),"é😀\\0");return t}\n'
                'export fn echo<T>(value:T)->(T,()){return (value,())}\n'
                'export fn coordinates()->(f32,f32){return (1.5,2.5)}\n')
        (output/'future-numbers.xr').write_text(module,encoding='utf-8')
        (output/'scope-a.xr').write_text('fn label()->string{return "A\\0é"}\nexport fn a<T>(value:T)->(T,string){return (value,label())}\n',encoding='utf-8')
        (output/'scope-b.xr').write_text('fn label()->string{return "B\\0中"}\nexport fn b<T>(value:T)->(T,string){return (value,label())}\n',encoding='utf-8')
        prefix='import {trunc,down,up,rounded,positive,notnumber,ieee,constants,nested,echo,coordinates} from "./future-numbers"\n'
        numbers=[0.0,-0.0,0.49999999999999994,0.5,0.5000000000000001,-0.49999999999999994,-0.5,-0.5000000000000001,
                 1.5,-1.5,2.5,-2.5,4503599627370495.5,4503599627370496.0,4503599627370497.0,
                 9007199254740991.0,9223372036854774784.0,-9223372036854775808.0,5e-324,-5e-324]
        floating=prefix;answer=bytearray()
        for number in numbers:
            value=Fraction.from_float(number);magnitude=abs(value)
            nearest=(2*magnitude.numerator+magnitude.denominator)//(2*magnitude.denominator)
            if value<0:nearest=-nearest
            literal=repr(number)
            floating+='print('+','.join(f'comptime {method}({literal})' for method in ['trunc','down','up','rounded','positive','notnumber'])+')\n'
            answer.extend(f'{int(value)} {value//1} {-((-value)//1)} {nearest} true false\n'.encode())
        floating+='print(comptime ieee())\n';answer.extend(b'true\n')
        tuples=(prefix+'const (a,b,c,d)=comptime constants()\nprint(a,b,c,d,(a as i64)+(b as i64)*10+(c as i64)*100+(d as i64)*1000)\n'
                'const t=comptime nested()\nprint(t.0.0,t.1)\n'
                'const text=comptime echo<string>("a\\0b")\nprint(text.0)\n'
                'const point=comptime coordinates()\nprint(point.0,point.1)\n')
        tuple_answer=b'2 3 5 7 7532\n'+b'7 '+ 'é😀\0\n'.encode()+b'a\0b\n1.5 2.5\n'
        scopes=('import {a} from "./scope-a"\nimport {b} from "./scope-b"\n'
                'const x=comptime a<i8>(7)\nconst y=comptime b<i8>(9)\nprint(x.0,x.1)\nprint(y.0,y.1)\n')
        scope_answer=b'7 A\0'+ 'é\n'.encode()+b'9 B\0'+'中\n'.encode()
        cases=[('floating',floating,bytes(answer)),('tuples',tuples,tuple_answer),('scopes',scopes,scope_answer)]
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
        negative=[('range',prefix+'print(comptime trunc(1.0/0.0))\n',b'comptime value range violation'),
                  ('numeric-text','fn text(x:f64)->string{return x.toString()}\nprint(comptime text(1.5))\n',b'admitted deterministic scalar direct-call closure'),
                  ('io','fn forbidden()->f64{print(1);return 1.5}\nprint(comptime forbidden())\n',b'admitted deterministic scalar direct-call closure'),
                  ('private-import','import {label} from "./scope-a"\nprint(label())\n',b'import requires an exported declaration')]
        for name,text,message in negative:
            entry=output/(name+'.xr');entry.write_text(text,encoding='utf-8');result=invoke(name,binary,['run',str(entry)],hidden)
            assert result.returncode==1 and not result.stdout and message in result.stderr,result
        (output/'summary.json').write_text(json.dumps(dict(status='FLOAT_TUPLE_CTFE_IMPORT_PASS',commands=len(records),
            binary64_rational_cases=20,float_method_calls=120,IEEE_nan_infinity_positive_zero=True,
            owning_Tuple_cases=4,Tuple_logical_output_max_bytes=8,private_Tuple_Cell=True,Tuple_generic_echo=True,
            duplicate_private_helper_names_isolated=True,duplicate_generic_parameter_names_isolated=True,private_import_refused=True,
            source_absent_and_poisoned=True,Windows_absolute_child_denial=policy.control,
            numeric_TO_STRING_ctfe=False,user_class_CTFE=False,authorized_derivation=False,complete_standard_library=False),indent=2)+'\n',encoding='utf-8')
        print('Float/Tuple CTFE: exact rational Float methods, IEEE and owned nested/generic table literals PASS');return 0
    finally:
        if temporary:temporary.cleanup()
if __name__=='__main__':raise SystemExit(main())
