"""Check complete cleanup inputs on the actual VM, SDK build and native host."""
from pathlib import Path
import argparse
import json
import shutil
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--native-driver', type=Path)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.root.resolve()/'scripts'))
    from xray_process import run_child
    fixtures = args.fixtures.resolve()
    oracle = json.loads((fixtures/'cleanup_replaced.expected.json').read_text(encoding='utf-8'))
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='xray-cleanup-owned-') as temporary:
        directory = Path(temporary)

        def check(name, command, expected):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(command, cwd=directory, stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0)
                actual = stdout.read()
                stderr.seek(0)
                errors = stderr.read()
            wanted = expected['stdout'].encode('utf-8'), expected['stderr'].encode('utf-8')
            if evidence:
                (evidence/(name+'.stdout')).write_bytes(actual)
                (evidence/(name+'.stderr')).write_bytes(errors)
            assert (code, actual, errors) == (expected['exit'], *wanted), (name, command, code, actual, errors, expected)
            print(f'{name}: exit{code}, stdout{len(actual)}B, stderr{len(errors)}B')

        if args.native_driver:
            check('generated native', [str(args.native_driver.resolve())], oracle['owned'])
            return
        binary = str(args.binary.resolve())
        for name in ('plain', 'optional', 'owned', 'fatal'):
            source = fixtures/f'cleanup_replaced_{name}.xr'
            if name == 'fatal':
                source = directory/'fatal.xr'
                source.write_bytes((fixtures/'cleanup_replaced_owned.xr').read_bytes()+b'\nprint(fatalPanic())\n')
            executable = directory/(name+'.exe')
            check(name+' VM', [binary, 'run', str(source)], oracle[name])
            check(name+' build', [binary, 'build', str(source), '-o', str(executable)],
                  dict(exit=0, stdout='Built: '+str(executable)+'\n', stderr=''))
            assert executable.is_file()
            check(name+' native', [str(executable)], oracle[name])
            if evidence:
                shutil.copyfile(source, evidence/(name+'.xr'))
                shutil.copyfile(executable, evidence/(name+'.exe'))
    print('Actual cleanup VM/build/native raw outputs and outermost fatal70 passed')


if __name__ == '__main__':
    main()
