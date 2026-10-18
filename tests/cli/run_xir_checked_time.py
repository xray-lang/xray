"""Exercise published Checked clocks and timers without their source files."""
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path


SOURCE = '''import time
const before = time.nanos()
time.sleep(2)
const after = time.nanos()
print(time.now())
print(time.clock() >= 0)
print(time.localOffsetAt(0))
print(time.micros() >= after / 1000 && time.monotonic() >= after / 1000000)
print(after - before >= 2000000)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binary, root = args.binary.resolve(), args.root.resolve()
    temporary = None
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='xir-checked-time-')
        output = Path(temporary.name).resolve()
    records = []
    binary_digest = hashlib.sha256(binary.read_bytes()).hexdigest()

    def invoke(label, executable, arguments, environment):
        argv = [str(executable), *map(str, arguments)]
        started = time.time()
        process = subprocess.run(argv, cwd=root, env=environment, capture_output=True, timeout=60)
        finished = time.time()
        (output / (label + '.stdout')).write_bytes(process.stdout)
        (output / (label + '.stderr')).write_bytes(process.stderr)
        records.append(dict(label=label, argv=argv, exit_code=process.returncode,
                            wall_start=started, wall_end=finished))
        (output / 'commands.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
        assert process.returncode == 0 and not process.stderr, (label, process.returncode, process.stderr)
        return process.stdout, started, finished

    def check_clocks(label, observation):
        stdout, started, finished = observation
        rows = stdout.splitlines()
        assert len(rows) == 5 and rows[1] == b'true' and rows[3:] == [b'true', b'true'], (label, rows)
        assert started - 5 <= int(rows[0]) / 1000 <= finished + 5, (label, rows[0], started, finished)
        assert -1440 <= int(rows[2]) <= 1440, (label, rows[2])

    try:
        version, _, _ = invoke('version', binary, ['--version', '--json'], os.environ.copy())
        identity = json.loads(version)
        for scenario in ('source_absent', 'source_poisoned'):
            directory = output / scenario
            stdlib = directory / 'stdlib'
            stdlib.mkdir(parents=True)
            source = directory / 'main.xr'
            source.write_text(SOURCE, encoding='utf-8')
            poison = stdlib / 'time/time.xr'
            if scenario == 'source_poisoned':
                poison.parent.mkdir()
                poison.write_text('invalid source: published Checked time must supply the module\n', encoding='utf-8')
            original = poison.read_bytes() if poison.exists() else None
            environment = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR='1')
            check_clocks(scenario + '-vm', invoke(scenario + '-vm', binary, ['run', source], environment))
            generated = directory / 'program.c'
            invoke(scenario + '-emit', binary, ['build', '--c-only', source, '-o', generated], environment)
            code = generated.read_bytes()
            assert b'XrXirCallEntry' in code and b'const XrXirProgramSpec xray_app_program' in code
            assert b'XrProto' not in code and b'xrt_vm_run' not in code and b'({' not in code
            native = directory / ('program.exe' if os.name == 'nt' else 'program')
            invoke(scenario + '-build', binary, ['build', source, '-o', native], environment)
            check_clocks(scenario + '-native', invoke(scenario + '-native', native, [], environment))
            assert (poison.read_bytes() if poison.exists() else None) == original
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == binary_digest
        result = dict(status='PASS', binary=str(binary), binary_sha256=binary_digest, version=identity,
                      commands=len(records), source_absent_and_poisoned=True,
                      real_VM_native_and_portable_C=True, production_release=False)
        (output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print('Checked time: VM/native clocks, offsets and timers without source fallback PASS')
    finally:
        if temporary:
            temporary.cleanup()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
