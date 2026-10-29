"""Exercise unchanged complete metadata programs through actual CLI consumers."""
from pathlib import Path
import argparse
import os
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.root/'scripts'))
    from xray_process import run_child
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    sizes = {'implementation': 7658, 'nominal': 87251,
             'wide-parameters': 14304, 'wide-type-name': 16886, 'wide-callable': 15548}
    with tempfile.TemporaryDirectory(prefix='xray-type-copy-') as temporary:
        directory = Path(temporary)

        def run(command, expected=None):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(list(map(str, command)), cwd=directory, env=environment,
                                 stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0); actual = stdout.read(); stderr.seek(0); errors = stderr.read()
            assert code == 0 and not errors, (command, code, actual, errors)
            if expected is not None:
                assert actual == expected, (command, actual, expected)

        for name, size in sizes.items():
            source = (args.fixtures/(name+'.xr')).resolve()
            assert source.stat().st_size == size
            golden = (args.fixtures/(name+'.out')).read_bytes()
            assert golden == (b'42\n42\n42\n' if name == 'wide-callable' else
                              b'42\n42\n' if name == 'wide-parameters' else b'42\n')
            run([args.binary, 'check', source], b'')
            run([args.binary, 'run', source], golden)
            generated = directory/(name+'.c')
            run([args.binary, 'build', source, '-c', '-o', generated])
            assert b'({' not in generated.read_bytes()
            native = directory/(name+'.exe')
            run([args.binary, 'build', source, '--toolchain', 'msvc', '-o', native])
            run([native], golden)
    print('Whole original metadata programs: CLI check/VM/C/MSVC and exact goldens PASS')


if __name__ == '__main__':
    main()
