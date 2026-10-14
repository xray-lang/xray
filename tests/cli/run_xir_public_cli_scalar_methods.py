"""Compare governed scalar methods against exact binary64 rational oracles."""
from __future__ import annotations
import argparse
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    root,binary=args.root.resolve(),args.binary.resolve()
    binary.relative_to(root)
    temporary=None
    if args.output:
        output=args.output.resolve();output.relative_to(root);output.mkdir(parents=True,exist_ok=False)
    else:
        temporary=tempfile.TemporaryDirectory(prefix='scalar-methods-',dir=binary.parent)
        output=Path(temporary.name)
    records=[]
    try:
        stdlib=output/'stdlib';(stdlib/'types').mkdir(parents=True)
        for name in ('f64','bool','rune'):
            (stdlib/'types'/(name+'.xr')).write_text('invalid native declaration source\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1')
        hidden=dict(env,PATH=str(output/'no-C-provider'))
        numbers=[0.0,-0.0,0.49999999999999994,0.5,0.5000000000000001,
                 -0.49999999999999994,-0.5,-0.5000000000000001,
                 1.5,-1.5,2.5,-2.5,4503599627370495.5,4503599627370496.0,
                 4503599627370497.0,9007199254740991.0,9223372036854774784.0,
                 -9223372036854775808.0,5e-324,-5e-324]
        source='fn row(x:f64) { print(x.toI64(),x.floor(),x.ceil(),x.round(),x.abs()>=0.0,x.isNaN()) }\n'
        expected=bytearray()
        for x in numbers:
            q=Fraction.from_float(x);mag=abs(q)
            rounded=(2*mag.numerator+mag.denominator)//(2*mag.denominator)
            if q<0:rounded=-rounded
            source+=f'row({repr(x)})\n'
            expected.extend(f'{int(q)} {q//1} {-((-q)//1)} {rounded} true false\n'.encode())
        source+='var nan:f64=0.0/0.0\nvar inf:f64=1.0/0.0\nvar zero:f64=-0.0\n'
        source+='print(nan.isNaN(),nan.abs().isNaN(),(-inf).abs()==inf,1.0/zero.abs()==inf,zero.abs().toString())\n'
        expected.extend(b'true true true true 0.0\n')
        text='print(true.toString(),false.toString())\n'
        text_expected=bytearray(b'true false\n')
        for scalar in (0,65,0x1f600,0x10ffff):
            text+=f'var r_{scalar}:rune=rune({scalar})\nprint(r_{scalar}.toUInt32(),r_{scalar}.toString())\n'
            text_expected.extend(str(scalar).encode()+b' '+chr(scalar).encode('utf-8')+b'\n')
        text+='assertEqual(true.toString(),"true")\nassertEqual(rune(65).toUInt32(),65 as u32)\n'
        cases=[('floating',source,bytes(expected)),('text-and-rune',text,bytes(text_expected))]

        def invoke(name,executable,arguments,environment):
            result=subprocess.run([str(executable),*arguments],cwd=root,env=environment,capture_output=True,timeout=60)
            (output/(name+'.stdout')).write_bytes(result.stdout)
            (output/(name+'.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name,argv=[str(executable),*arguments],exit_code=result.returncode,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output/'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
            return result

        for name,body,wanted in cases:
            entry=output/(name+'.xr');entry.write_text(body,encoding='utf-8')
            result=invoke(name+'-vm',binary,['run',str(entry)],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,wanted,b''),(name,result)
            generated=output/(name+'.c')
            result=invoke(name+'-c',binary,['build','--c-only',str(entry),'-o',str(generated)],env)
            assert result.returncode==0 and not result.stderr,result
            assert b'({' not in generated.read_bytes()
            native=output/(name+'.exe')
            result=invoke(name+'-build',binary,['build',str(entry),'-o',str(native)],env)
            assert result.returncode==0 and not result.stderr,result
            result=invoke(name+'-native',native,[],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,wanted,b''),(name,result)
        for name,body in [('float-arity','print(1.0.abs(2))\n'),
                          ('float-typeargs','print(1.0.floor<i64>())\n'),
                          ('boolean-ref','print(true.toString(ref 1))\n'),
                          ('rune-unadmitted',"print(rune(65).isLetter())\n"),
                          ('float-unadmitted','print(1.0.sqrt())\n')]:
            entry=output/(name+'.xr');entry.write_text(body,encoding='utf-8')
            result=invoke(name,binary,['run',str(entry)],hidden)
            assert result.returncode==1 and not result.stdout and result.stderr,result
        for method in ('toI64','floor','ceil','round'):
            for index,expression in enumerate(('0.0/0.0','1.0/0.0','-1.0/0.0',
                                                '9223372036854775808.0','-9223372036854777856.0')):
                name=f'range-{method}-{index}'
                entry=output/(name+'.xr')
                entry.write_text(f'var value:f64={expression}\nprint(value.{method}())\n',encoding='utf-8')
                result=invoke(name+'-vm',binary,['run',str(entry)],hidden)
                assert result.returncode==1 and not result.stdout and b'E0422' in result.stderr,result
                if index==0:
                    native=output/(name+'.exe')
                    result=invoke(name+'-build',binary,['build',str(entry),'-o',str(native)],env)
                    assert result.returncode==0 and not result.stderr,result
                    result=invoke(name+'-native',native,[],hidden)
                    assert result.returncode==1 and not result.stdout and b'E0422' in result.stderr,result
        for path in (stdlib/'types').iterdir():path.unlink()
        for name,_,wanted in cases:
            result=invoke(name+'-absent',binary,['run',str(output/(name+'.xr'))],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,wanted,b''),result
        (output/'summary.json').write_text(json.dumps(dict(status='SCALAR_DEFINITION_ORACLE_PASS',
            commands=len(records),binary64_rational_cases=len(numbers),rune_cases=4,recipes=10,
            VM_numeric_range_cases=20,native_numeric_range_cases=4,
            source_absent_and_poisoned=True,VM_C_provider_PATH_hidden=True,complete_standard_library=False),indent=2)+'\n',encoding='utf-8')
        print('Governed float/bool/rune: exact binary64 rational oracle, scalar/text ownership, no-source VM/native PASS')
        return 0
    finally:
        if temporary:temporary.cleanup()


if __name__=='__main__':
    raise SystemExit(main())
