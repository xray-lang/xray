"""Run the real Source driver and its shared output leaf."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import hashlib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    args = parser.parse_args()
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='source-', dir=args.scratch) as folder:
        root = Path(folder)

        def run(name, text, expected=0, diagnostic=b'', stdout=b''):
            path = root / (name + '.xr')
            path.write_text(text, encoding='utf-8')
            result = subprocess.run([args.driver, str(path)], capture_output=True, timeout=15)
            assert result.returncode == expected, (name, result.returncode, result.stderr)
            assert result.stdout == stdout, (name, result.stdout)
            if expected:
                assert diagnostic in result.stderr and str(path).encode() in result.stderr, (name, result.stderr)
            else:
                assert result.stderr == b'', (name, result.stderr)

        run('empty', '')
        run('ordinary-main', 'fn main()->i64{print(99)\nreturn 7}\n')
        run('recursive-call', 'fn fib(n:i64)->i64{if(n<2){return n}\n'
            'return fib(n-1)+fib(n-2)}\nfib(10)\n')
        run('loop', 'fn sum(n:i64)->i64{var i=0\nvar total=0\n'
            'while(i<n){total=total+i\ni=i+1}\nreturn total}\nsum(1000)\n')
        run('output', 'print(7)\n', stdout=b'7\n')
        literal_input = (Path(__file__).resolve().parents[1] /
            'diff/cases/semantics/output/print_terminator_exact.xr').read_bytes()
        assert hashlib.sha256(literal_input.replace(b'\r\n', b'\n')).hexdigest() == \
            '32f13c28e415c770c81bd0eafbfb7d4008b8bdf192f7cb6cade5c01f2845a6a2'
        literal_path = root/'print_terminator_exact.xr'
        literal_path.write_bytes(literal_input)
        literal_oracle = bytes.fromhex('6c310a6c320a6c330a6d310a6d320a7a0a0a74200a300a310a320a656e640a')
        for options in ([], ['--twice'], ['--optimize'], ['--twice', '--optimize']):
            result = subprocess.run([args.driver, str(literal_path), *options], capture_output=True, timeout=15)
            assert result.returncode == 0 and not result.stderr, (options, result.returncode, result.stderr)
            assert result.stdout == literal_oracle * (2 if '--twice' in options else 1), (options, result.stdout)
            assert literal_path.read_bytes() == literal_input
        run('literal-nul-unicode-empty', 'print("A\\0é")\nprint(7)\nprint("")\n',
            stdout=b'A\x00\xc3\xa9\n7\n\n')
        original_cell = (Path(__file__).resolve().parents[1] /
                         'diff/cases/semantics/default_params/default_evaluated_per_call.xr').read_bytes()
        assert hashlib.sha256(original_cell.replace(b'\r\n', b'\n')).hexdigest() == \
            '8678fe40214cebbf01fc88a552610ec957b9092ffc92a6c49cf884fe0b3f6f47'
        path = root/'default_evaluated_per_call.xr'
        path.write_bytes(original_cell)
        for options in ([], ['--twice'], ['--optimize'], ['--twice', '--optimize']):
            result = subprocess.run([args.driver, str(path), *options], capture_output=True, timeout=15)
            assert result.returncode == 0 and not result.stderr, (options, result.returncode, result.stderr)
            assert result.stdout == b'1\n2\n99\n3\n3\n' * (2 if '--twice' in options else 1), \
                (options, result.stdout)
            assert path.read_bytes() == original_cell
        run('literal-zero', 'const n=0\nprint(n)\n', stdout=b'0\n')
        run('printing-closure', 'fn leaf(x:i64)->i64{print(x)\nreturn x}\n'
            'fn call(x:i64)->i64{return leaf(x)}\nprint(call(7))\n', stdout=b'7\n7\n')
        original = (Path(__file__).resolve().parents[2] / 'bench/native/integer_loop.xr').read_bytes()
        needle = b'const n = 100000000'
        assert original.count(needle) == 1
        for n, expected in ((0, b'0\n'), (1000, b'46995\n')):
            path = root / ('literal-loop-' + str(n) + '.xr')
            path.write_bytes(original.replace(needle, b'const n = ' + str(n).encode()))
            result = subprocess.run([args.driver, str(path)], capture_output=True, timeout=15)
            assert result.returncode == 0 and result.stdout == expected and not result.stderr
        run('i64-cast', 'fn identity(x:i64)->i64{return x as i64}\n'
            'print(identity(-9223372036854775807-1))\nprint(identity(-1))\n'
            'print(identity(0))\nprint(identity(9223372036854775807))\n',
            stdout=b'-9223372036854775808\n-1\n0\n9223372036854775807\n')
        run('whole-closure', 'print(1)\nfn value(x:f64)->f64{return x+1.0}\nvalue(3.0)\n', 1, b'bytecode')
        run('division-fixed', 'fn quotient(n:i64)->i64{return n/2}\nprint(quotient(8))\n', stdout=b'4\n')
        run('remainder-fixed', 'fn remainder(n:i64)->i64{return n%3}\nprint(remainder(-7))\n', stdout=b'-1\n')
        run('division-dynamic-rejected', 'fn quotient(n:i64,d:i64)->i64{return n/d}\nquotient(8,2)\n', 1, b'DIV_INT')
        run('remainder-dynamic-rejected', 'fn remainder(n:i64,d:i64)->i64{return n%d}\nremainder(8,2)\n', 1, b'REM_INT')
        run('division-zero-rejected', 'fn quotient(n:i64)->i64{return n/0}\nquotient(8)\n', 1, b'DIV_INT')
        run('remainder-zero-rejected', 'fn remainder(n:i64)->i64{return n%0}\nremainder(8)\n', 1, b'REM_INT')
        run('division-minus-one', 'fn q(n:i64)->i64{return n/(-1)}\n'
            'print(q(-9223372036854775807-1))\nprint(q(-7))\nprint(q(7))\n'
            'print(q(9223372036854775807))\nprint(q(0))\n',
            stdout=b'-9223372036854775808\n7\n-7\n-9223372036854775807\n0\n')
        run('remainder-minus-one', 'fn r(n:i64)->i64{return n%(-1)}\n'
            'print(r(-9223372036854775807-1))\nprint(r(-7))\nprint(r(7))\n'
            'print(r(9223372036854775807))\nprint(r(0))\n', stdout=b'0\n0\n0\n0\n0\n')
        run('malformed', 'fn broken(\n', 1, b'source')
        missing = root / 'missing.xr'
        result = subprocess.run([args.driver, str(missing)], capture_output=True, timeout=15)
        assert result.returncode == 1 and not result.stdout and str(missing).encode() in result.stderr
        result = subprocess.run([args.driver], capture_output=True, timeout=15)
        assert result.returncode == 2 and not result.stdout and b'usage:' in result.stderr
    print('Source driver integration passed, including shared output')


if __name__ == '__main__':
    main()
