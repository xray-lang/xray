"""Run a public four-module product against independent VM and C byte oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def fact(path):
    data = path.read_bytes()
    return dict(path=str(path), bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'cli', 'output', 'sdk', 'cc', 'linker'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    fixture = args.root / 'tests/fixtures/public_source_program'
    args.output.mkdir(parents=True, exist_ok=True)
    golden = (fixture / 'root.out').read_bytes()
    env = dict(os.environ, XRAY_STDLIB_PATH=str(args.root / 'stdlib'), NO_COLOR='1')
    record = dict(status='RUNNING', commands=[], scope='Public VM/check/test/C output, ordinary native publication and strict MSVC C11 consumption; interactive TTY NOT_RUN')
    libraries = [args.sdk / 'lib' / name for name in (
        'xray_compile_resources.lib', 'xray_xir_admission.lib', 'xray_xir_declarations.lib',
        'xray_xir_scalar.lib', 'xray_xir_runtime_host.lib')]
    launcher = args.sdk / 'src/execution/xr_xir_native_main.inc.c'
    inputs = [Path(__file__), args.cli, args.cc, args.linker, launcher,
              args.sdk / 'sdk_manifest.json', *libraries, *sorted(fixture.iterdir())]
    record['inputs_before'] = [fact(path) for path in inputs]

    def save():
        (args.output / 'record.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')

    def run(name, command, exit_code=0, stdout=None, stderr=None, environment=None):
        command = list(map(str, command))
        item = dict(name=name, command=command, expected_exit=exit_code,
                    expected_stdout=None if stdout is None else stdout.hex(),
                    expected_stderr=None if stderr is None else stderr.hex())
        record['commands'].append(item)
        save()
        start = time.monotonic()
        result = subprocess.run(command, cwd=args.output, env=environment or env,
                                capture_output=True, timeout=120)
        for label, data in [('stdout', result.stdout), ('stderr', result.stderr)]:
            path = args.output / (name + '.' + label)
            path.write_bytes(data)
            item[label] = fact(path)
        item.update(exit=result.returncode, seconds=time.monotonic() - start)
        passed = (result.returncode == exit_code and (stdout is None or result.stdout == stdout)
                  and (stderr is None or result.stderr == stderr))
        item['status'] = 'PASS' if passed else 'FAIL'
        save()
        assert passed, (name, result.returncode, result.stdout, result.stderr)
        return result.stdout

    version = json.loads(run('version', [args.cli, '--version', '--json'], stderr=b''))
    assert version['product'] == 'xray-lang' and version['target'] == 'windows-x86_64'
    run('vm-first', [args.cli, 'run', fixture / 'root.xr'], stdout=golden, stderr=b'')
    run('vm-new-process', [args.cli, 'run', fixture / 'root.xr'], stdout=golden, stderr=b'')
    run('check', [args.cli, 'check', fixture / 'root.xr'], stdout=b'', stderr=b'')
    for name, source, options, expected, expected_error, totals in (
        ('test', 'test_root.xr', [], 0, b'', (1, 1, 0, 0)),
        ('test-verbose', 'test_root.xr', ['--verbose'], 0, b'', (1, 1, 0, 0)),
        ('test-filtered', 'test_root.xr', ['--filter', 'absent'], 1,
         b'Error: 0 tests executed across 1 file(s)\n', (1, 0, 0, 1)),
        ('test-failed', 'test_fail.xr', [], 1, b'', (1, 0, 1, 0)),
    ):
        report = args.output / (name + '.json')
        data = run(name, [args.cli, 'test', fixture / source, '--report', report, *options],
                   expected, stderr=expected_error)
        assert b'\x1b[' not in data, (name, 'Redirected reports must be plain, without stripping')
        files = json.loads(report.read_text(encoding='utf-8'))['files']
        assert len(files) == 1
        row = files[0]
        assert tuple(row[k] for k in ('tests', 'passed', 'failed', 'skipped')) == totals
        assert row['errors'] == 0 and row['timeouts'] == 0
        assert len(row['cases']) == 1
    report = args.output / 'test-parallel.json'
    data = run('test-parallel', [args.cli, 'test', fixture / 'test_fail.xr',
               fixture / 'test_root.xr', '--jobs', '2', '--report', report], 1, stderr=b'')
    assert b'Running 2 files on 2 threads...' in data and b'\x1b[' not in data
    files = json.loads(report.read_text(encoding='utf-8'))['files']
    assert [Path(row['path']).name for row in files] == ['test_fail.xr', 'test_root.xr']
    assert [tuple(row[k] for k in ('tests', 'passed', 'failed', 'skipped')) for row in files] == [
        (1, 0, 1, 0), (1, 1, 0, 0)]
    assert all(row['errors'] == 0 and row['timeouts'] == 0 and len(row['cases']) == 1
               for row in files)
    color_env = dict(env)
    color_env.pop('NO_COLOR')
    data = run('explicit-color', [args.cli, '--color', 'test', fixture / 'test_root.xr'],
               stderr=b'', environment=color_env)
    assert b'\x1b[' in data
    data = run('explicit-plain', [args.cli, '--no-color', 'test', fixture / 'test_root.xr'], stderr=b'')
    assert b'\x1b[' not in data
    manifest = json.loads((args.sdk / 'sdk_manifest.json').read_text(encoding='utf-8'))
    assert manifest['c_dialect'] == 11 and manifest['crt'] == 2
    record['sdk_manifest'] = manifest
    generated = args.output / 'program.c'
    run('c-only', [args.cli, 'build', '--c-only', fixture / 'root.xr', '-o', generated], stderr=b'')
    text = generated.read_text(encoding='utf-8')
    assert not re.search(r'\(\s*\{', text), 'Generated C must not contain GNU statement expressions'
    objects = []
    for name, path, definitions in [('program', generated, []), ('launcher', launcher, ['/DXIR_SDK_PROGRAM_SYMBOL=xray_app_program'])]:
        obj = args.output / (name + '.obj')
        run('compile-' + name, [args.cc, '/nologo', '/std:c11', '/utf-8', '/experimental:c11atomics',
            '/MD', '/O2', '/W4', '/WX', '/DNDEBUG', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
            '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(args.sdk / 'src'),
            '/I' + str(args.sdk / 'include'), '/I' + str(args.sdk / 'generated'),
            *definitions, '/c', path, '/Fo' + str(obj)])
        objects.append(obj)
    executable = args.output / 'program.exe'
    run('link', [args.linker, '/nologo', '/incremental:no', '/out:' + str(executable),
                 *objects, *libraries, 'kernel32.lib', '/defaultlib:MSVCRT', '/defaultlib:OLDNAMES'])
    run('native-c-consumer', [executable], stdout=golden, stderr=b'')
    published = args.output / 'published-program.exe'
    run('public-native-build', [args.cli, 'build', fixture / 'root.xr', '-o', published], stderr=b'')
    run('public-native-program', [published], stdout=golden, stderr=b'')
    record['generated_artifacts'] = [fact(path) for path in [generated, *objects, executable]]
    record['generated_artifacts'].append(fact(published))
    record['inputs_after'] = [fact(path) for path in inputs]
    assert record['inputs_before'] == record['inputs_after'], 'Recorded inputs changed during qualification'
    record['status'] = 'PASS'
    save()
    print('Public four-module VM/check/test/C/native product and independent byte oracles PASS')


if __name__ == '__main__':
    main()
