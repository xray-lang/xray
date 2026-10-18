"""Compare definition-owned integer calls with independent mathematical expectations."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path
from xir_no_child_process import NoChildProcess


def wrapped(value: int, bits: int = 64, signed: bool = True) -> int:
    value %= 1 << bits
    return value - (1 << bits) if signed and value >= (1 << (bits - 1)) else value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    binary.relative_to(root)
    temporary = None
    if args.output:
        output = args.output.resolve()
        output.relative_to(root)
        output.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='integer-methods-', dir=binary.parent)
        output = Path(temporary.name)
    records = []
    try:
        process_policy = NoChildProcess(root, output)
        stdlib = output / 'stdlib'
        (stdlib / 'types').mkdir(parents=True)
        (stdlib / 'types/i64.xr').write_text('invalid source must not provide numeric declarations\n', encoding='utf-8')
        env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR='1')
        hidden = dict(env, PATH=str(output / 'no-C-provider'))
        lo, hi = -(1 << 63), (1 << 63) - 1
        pairs = [(lo, -1), (-1, lo), (lo, 0), (0, lo), (lo, 1), (lo, 2), (hi, 2),
                 (hi, 1), (hi, -1), (lo, hi), (hi, lo), (0, 0), (-7, 6), (7, -6), (7, 6)]
        body = 'fn row(a:i64,b:i64) {\n print(a.abs(),a.min(b),a.max(b),a.wrappingAdd(b),a.wrappingSub(b),a.wrappingMul(b))\n print(a.addOverflows(b),a.subOverflows(b),a.mulOverflows(b))\n print(a.saturatingAdd(b),a.saturatingSub(b),a.saturatingMul(b))\n print(a.checkedAdd(b)==null,a.checkedSub(b)==null,a.checkedMul(b)==null)\n}\n'
        expected = bytearray()
        for a, b in pairs:
            body += f'row({a},{b})\n'
            results = [a + b, a - b, a * b]
            overflow = [not lo <= v <= hi for v in results]
            rows = [[wrapped(abs(a)), min(a,b), max(a,b), *[wrapped(v) for v in results]],
                    ['true' if v else 'false' for v in overflow],
                    [max(lo,min(hi,v)) for v in results],
                    ['true' if v else 'false' for v in overflow]]
            for row in rows:
                expected.extend((' '.join(str(v) for v in row) + '\n').encode())
        body += 'print(7.checkedMul(6)==42,7.checkedMul(0)==0,(-42).toString(),42.toF64())\n'
        expected.extend(b'true true -42 42.0\n')
        for a, b in pairs:
            results = [a + b, a - b, a * b]
            for method, result in zip(('checkedAdd', 'checkedSub', 'checkedMul'), results):
                if lo <= result <= hi:
                    body += f'print(({a}).{method}({b})=={result})\n'
                    expected.extend(b'true\n')
        cases = [('integer-controls',body,bytes(expected)),
                 ('hex','const lo:i64=-9223372036854775807-1\nprint(lo.toHex(),0.toHex(),(-42).toHex(),9223372036854775807.toHex())\n',
                  b'-0x8000000000000000 0x0 -0x2a 0x7fffffffffffffff\n')]
        width_source, width_expected = '', bytearray()
        for spelling, bits, signed in [('i8',8,True),('u8',8,False),('i16',16,True),('u16',16,False),
                                      ('i32',32,True),('u32',32,False),('i64',64,True),('u64',64,False)]:
            maximum = (1 << (bits - int(signed))) - 1
            width_source += f'var v_{spelling}:{spelling}={maximum}\nprint(v_{spelling}.wrappingAdd(1),v_{spelling}.wrappingSub(-1),v_{spelling}.wrappingMul(2))\n'
            width_expected.extend((' '.join(str(wrapped(v,bits,signed)) for v in (maximum+1,maximum+1,maximum*2))+'\n').encode())
        width_source += 'var v:i8=127\nprint(v.wrappingAdd(257))\n'
        width_expected.extend(b'-128\n')
        cases.append(('fixed-width',width_source,bytes(width_expected)))
        cases.append(('comptime-scalar',
            'fn absolute(x:i64)->i64 { return x.abs() }\n'
            'fn wrap(x:i8)->i8 { return x.wrappingAdd(257) }\n'
            'print(comptime absolute(-9223372036854775807-1),comptime wrap(127))\n',
            b'-9223372036854775808 -128\n'))

        def invoke(name, executable, arguments, environment):
            argv=[str(executable),*arguments]
            restricted=environment is hidden and (not arguments or arguments[0]=='run')
            result=(process_policy.run(name,argv,environment) if restricted else
                    subprocess.run(argv,cwd=root,env=environment,capture_output=True,timeout=60))
            (output/(name+'.stdout')).write_bytes(result.stdout)
            (output/(name+'.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name,argv=[str(executable),*arguments],exit_code=result.returncode,
                CLI_child_processes_denied=process_policy.supported and restricted,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output/'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
            return result

        for name, text, expected_bytes in cases:
            source = output/(name+'.xr');source.write_text(text,encoding='utf-8')
            result = invoke(name+'-vm',binary,['run',str(source)],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,expected_bytes,b''),(name,result)
            generated = output/(name+'.c')
            result = invoke(name+'-c',binary,['build','--c-only',str(source),'-o',str(generated)],env)
            assert result.returncode==0 and not result.stderr,result
            assert b'({' not in generated.read_bytes()
            native = output/(name+'.exe')
            result = invoke(name+'-build',binary,['build',str(source),'-o',str(native)],env)
            assert result.returncode==0 and not result.stderr,result
            result = invoke(name+'-native',native,[],hidden)
            assert (result.returncode,result.stdout,result.stderr)==(0,expected_bytes,b''),(name,result)
        for name, text in [('wrong-type','print(7.wrappingAdd("wrong"))\n'),
                           ('wrong-arity','print(7.abs(1))\n'),
                           ('extra-type','print(7.wrappingAdd<i64>(1))\n'),
                           ('string-ref','print(7.toString(ref 1))\n'),
                           ('narrow-control','var v:i8=127\nprint(v.checkedAdd(1))\n')]:
            source = output/(name+'.xr');source.write_text(text,encoding='utf-8')
            result = invoke(name,binary,['run',str(source)],hidden)
            assert result.returncode==1 and not result.stdout and result.stderr,result
        (stdlib/'types/i64.xr').unlink()
        result = invoke('source-absent-vm',binary,['run',str(output/'fixed-width.xr')],hidden)
        assert (result.returncode,result.stdout,result.stderr)==(0,cases[2][2],b'')
        result = invoke('source-absent-comptime-vm',binary,['run',str(output/'comptime-scalar.xr')],hidden)
        assert (result.returncode,result.stdout,result.stderr)==(0,cases[3][2],b'')
        for number, count in [('0301_int64_overflow',32),('0304_fixed_width_wrapping',10)]:
            source = root/'tests/regression/03_operators'/(number+'.xr')
            report = output/(number+'.json')
            result = invoke(number+'-original',binary,['test',str(source),'--report',str(report)],hidden)
            assert result.returncode==0 and not result.stderr,result
            data = json.loads(report.read_text(encoding='utf-8'))
            assert len(data['files'])==1,data
            actual = data['files'][0]
            assert actual['tests']==count and actual['passed']==count,actual
            assert all(actual[key]==0 for key in ('failed','errors','timeouts','skipped')),actual
            assert len(actual['cases'])==count and all(row['status']=='passed' for row in actual['cases']),actual
        (output/'summary.json').write_text(json.dumps(dict(status='INTEGER_DEFINITION_VM_NATIVE_ORACLE_PASS',
            commands=len(records),mathematical_pairs=len(pairs),widths=8,source_absent_and_poisoned=True,
            VM_C_provider_PATH_hidden=True,Windows_absolute_child_denial=process_policy.control,
            complete_standard_library=False),indent=2)+'\n',encoding='utf-8')
        print('Integer definitions: 18 recipes, exact mathematical oracle, 8 widths, no-source VM/native and rejections PASS')
        return 0
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
