"""Exercise the real run command with fixed byte-level output and failure oracles."""
from pathlib import Path
import argparse
import json
import os
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--injected', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['XRAY_STDLIB_PATH'] = str(root/'stdlib')
    env['NO_COLOR'] = '1'
    records = []

    def run(name, arguments, expected, *, executable=None, cwd=None, environment=None):
        command = [str(executable or args.driver), *map(str, arguments)]
        start = time.perf_counter()
        result = subprocess.run(command, cwd=cwd or root, env=environment or env,
                                capture_output=True, timeout=30)
        row = dict(name=name, command=command, cwd=str(cwd or root),
                   returncode=result.returncode, seconds=time.perf_counter()-start,
                   stdout=result.stdout.hex(), stderr=result.stderr.hex())
        records.append(row)
        (output/(name+'.stdout')).write_bytes(result.stdout)
        (output/(name+'.stderr')).write_bytes(result.stderr)
        (output/'record.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        if expected is not None:
            assert (result.returncode, result.stdout, result.stderr) == expected, row
        return result

    entry = root/'tests/fixtures/xir_compile_owner/root.xr'
    oracle = (0, 'owner中-ok 42 true\n'.encode(), b'')
    run('multi', [entry], oracle)
    run('relative', ['root.xr'], oracle, cwd=entry.parent)
    unicode = output/'fixture 空格'
    unicode.mkdir(exist_ok=True)
    for path in entry.parent.glob('*.xr'):
        shutil.copyfile(path, unicode/path.name)
    run('unicode', [unicode/'root.xr'], oracle)
    programs = {
        'declaration': ('fn value() -> i64 { return 7 }\n', (0, b'', b'')),
        'embedded-nul': ('print("before\\0after")\nprint("before\\u{0}after")\n',
                         (0, b'before\x00after\nbefore\x00after\n', b'')),
        'error': ('enum Failure { Failed { text:string, code:i64, real:f64 } }\n'
                  'fn fail() { throw Failure.Failed { text:"owned"+"-error", code:-7, real:1.5 } }\n'
                  'print("before")\nfail()\n',
                  (1, b'before\n', b'[Uncaught Error] Failure.Failed("owned-error", -7, 1.5)\n')),
        'panic': ('assert(false, "run"+"-panic")\n',
                  (1, b'', b'[Uncaught Panic] E0445: run-panic\n')),
        'fatal': ('fn bad() { defer { assert(false, "cleanup") } }\nbad()\n',
                  (70, b'', b'E0443: error escaped a defer body: E0445: cleanup\n'
                   b'  a defer body must not let errors escape (spec 8.3.1); this is not catchable\n')),
    }
    for name, (source, expected) in programs.items():
        directory = output/name
        directory.mkdir(exist_ok=True)
        path = directory/'main.xr'
        path.write_text(source, encoding='utf-8', newline='\n')
        run(name, [path], expected)
    run('initialization', [root/'tests/fixtures/xir_initialization_failure/root.xr'],
        (1, b'A\nB\n', b'[Uncaught Error] InitFailure.Stopped("initialization-owned-long-payload-after-owner-destruction", 41)\n'))
    run('no-input', [], (2, b'', b'xray run: exactly one source file is required\n'))
    run('two-inputs', [entry, entry], (2, b'', b'xray run: exactly one source file is required\n'))
    for name, argument in [('stdin', '-'), ('empty', '')]:
        run(name, [argument], (1, b'', b'XR_RUN_6011: canonical run requires a file-backed source authority\n'))
    for name, argument in [('wrong-suffix', 'main.txt'), ('uppercase-suffix', 'main.XR')]:
        run(name, [argument], (1, b'', b"XR_RUN_6013: canonical run accepts only an exact '.xr' source path\n"))
    run('passthrough', [entry, '--', 'argument'],
        (1, b'', b'XR_RUN_6010: a module initializer does not accept arguments\n'))
    run('missing', [output/'missing.xr'],
        (1, b'', b'XR_RUN_6001: source path lookup failed (stage=1 status=not-found io=2)\n'))
    directory = output/'directory.xr'
    directory.mkdir(exist_ok=True)
    run('directory', [directory],
        (1, b'', b'XR_RUN_6001: source path lookup failed (stage=1 status=invalid io=5)\n'))
    for name, stdlib, status, io in [('stdlib-missing', output/'missing-stdlib', b'not-found', b'2'),
                                     ('stdlib-file', entry, b'invalid', b'5')]:
        bad_env = dict(env, XRAY_STDLIB_PATH=str(stdlib))
        run(name, [entry], (1, b'', b'XR_RUN_6001: source path lookup failed (stage=4 status='+status+b' io='+io+b')\n'),
            environment=bad_env)
    rejected = {
        'syntax': 'fn broken( {\n',
        'semantic': 'var value = missing_symbol\n',
        'import': 'import {missing} from "./not_there"\nprint(missing())\n',
    }
    for name, text in rejected.items():
        directory = output/name
        directory.mkdir(exist_ok=True)
        path = directory/'main.xr'
        path.write_text(text, encoding='utf-8', newline='\n')
        if name == 'syntax':
            diagnostic = (b"error: expected '}'\n  --> " + str(path).encode() + b':2:1\n'
                          b'   |\n 2 | \n   | ^\n\nerror: aborting due to 1 previous error\n'
                          b'XR_RUN_6001: source build failed (stage=4 status=source-rejected): module graph build failed\n')
        elif name == 'semantic':
            diagnostic = str(path).encode() + b':1:13: error: name is not an initialized value\n'
        else:
            diagnostic = b'XR_RUN_6001: source build failed (stage=4 status=not-found): module not found\n'
        run(name, [path], (1, b'', diagnostic))
    run('retired-identifiers', [root/'tests/unit/fixtures/assertion/retired_names_are_ordinary.xr'], (0, b'', b''))
    run('array-growth', [root/'tests/unit/fixtures/runtime/array_growth_accounting.xr'],
        (0, b'array-growth-accounting-ok\n', b''))
    injected = run('injected', [entry], None, executable=args.injected)
    assert injected.returncode == 0, records[-1]
    assert injected.stdout == oracle[1] + (
        b'RUN_OWNER: real default policy, five phase OOM boundaries, one ledger, budget and physical zero PASS\n'), records[-1]
    assert b'AddressSanitizer' not in injected.stderr and b'runtime error:' not in injected.stderr, records[-1]
    print('Run Source owner: typed command inputs, owned Program, exact output/status, OOM and resource policy PASS')


if __name__ == '__main__':
    main()
