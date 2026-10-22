"""Run the real Source driver and its shared output leaf."""
import argparse
from pathlib import Path
import subprocess
import tempfile


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
        run('whole-closure', 'print(1)\nfn value(x:f64)->f64{return x+1.0}\nvalue(3.0)\n', 1, b'bytecode')
        run('division-rejected', 'fn quotient(n:i64)->i64{return n/2}\nquotient(8)\n', 1, b'DIV_INT')
        run('malformed', 'fn broken(\n', 1, b'source')
        missing = root / 'missing.xr'
        result = subprocess.run([args.driver, str(missing)], capture_output=True, timeout=15)
        assert result.returncode == 1 and not result.stdout and str(missing).encode() in result.stderr
        result = subprocess.run([args.driver], capture_output=True, timeout=15)
        assert result.returncode == 2 and not result.stdout and b'usage:' in result.stderr
    print('Source driver integration passed, including shared output')


if __name__ == '__main__':
    main()
