"""Exercise complete CSV parsing, writing and typed error propagation."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    sys.path.insert(0, str(root / 'scripts'))
    from xray_process import run_child

    args.output.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='run-', dir=args.output))
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / 'stdlib'), NO_COLOR='1')
    record = dict(state='RUNNING', commands=[], inputs=[])

    def fact(path):
        raw = path.read_bytes()
        return dict(path=str(path), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())

    def save():
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')

    def run(name, argv, expected=None, rejected=False):
        argv = list(map(str, argv))
        stdout, stderr = out / (name + '.stdout'), out / (name + '.stderr')
        with stdout.open('wb') as a, stderr.open('wb') as b:
            code = run_child(argv, cwd=root, env=environment, stdout=a, stderr=b, timeout=30)
        record['commands'].append(dict(name=name, argv=argv, exit=code,
                                       stdout=fact(stdout), stderr=fact(stderr)))
        save()
        if rejected:
            assert code == 1 and stdout.read_bytes() == b'', (name, code)
            assert b'E0330: throwing call requires try or a try body' in stderr.read_bytes(), name
        else:
            assert code == 0 and not stderr.read_bytes(), (name, code, stderr.read_bytes())
            if expected is not None:
                assert stdout.read_bytes() == expected, (name, stdout.read_bytes(), expected)

    save()
    try:
        imported = out / 'import.xr'
        imported.write_bytes(b'import csv\nprint(1)\n')
        cases = [('import', imported, b'1\n')]
        fixtures = root / 'tests/cli/fixtures/csv'
        for name in ('behavior', 'errors'):
            source = fixtures / (name + '.xr')
            cases.append((name, source, (fixtures / (name + '.stdout')).read_bytes()))
        for name, source, expected in cases:
            record['inputs'].append(fact(source))
            run(name + '-vm', [args.binary, 'run', source], expected)
            executable = out / (name + ('.exe' if os.name == 'nt' else ''))
            run(name + '-build', [args.binary, 'build', source, '--native', '-o', executable])
            run(name + '-native', [executable], expected)
        for name, statement in (
            ('parse', 'csv.parse("x")'),
            ('report', 'csv.parseReport("x")'),
            ('write', 'csv.stringify(CsvDocument())'),
        ):
            source = out / (name + '-unhandled.xr')
            source.write_text('import csv\nimport { CsvDocument } from csv\n' + statement + '\n',
                              encoding='utf-8', newline='\n')
            record['inputs'].append(fact(source))
            for mode in ('check', 'run', 'build'):
                argv = [args.binary, mode, source]
                if mode == 'build':
                    target = out / (name + '-rejected.c')
                    argv += ['-c', '-o', target]
                run(name + '-unhandled-' + mode, argv, rejected=True)
                if mode == 'build':
                    assert not target.exists(), name
        record['state'] = 'PASS'
    except BaseException as error:
        record.update(state='FAIL', error=repr(error))
        raise
    finally:
        save()
    print('CSV whole import, document/recovery/writer, typed failures and unhandled-call rejection PASS')


if __name__ == '__main__':
    main()
