"""Compile real constructed Task bodies against five production-admitted archives."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import tempfile


def checked(command):
    result = subprocess.run(command, capture_output=True, timeout=120)
    assert result.returncode == 0, (command, (result.stdout + result.stderr).decode('utf-8', errors='replace'))
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('lease', 'emitter', 'bundle', 'compiler', 'linker', 'provider', 'output'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output_base = Path(args.output).resolve()
    assert output_base.is_relative_to(root) and output_base != root, 'Owned build output directory required'
    output_base.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=output_base))
    source = output / 'task_constructed_native.c'
    header = output / 'task_constructed_generated.h'
    checked([args.emitter, str(source), str(header)])
    assert not re.search(r'\(\s*\{', source.read_text(encoding='utf-8'))
    bundle = Path(args.bundle)
    lease = subprocess.Popen([args.lease, str(bundle), str(bundle / 'sdk_manifest.json')],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, encoding='utf-8')
    record = {}
    try:
        ready = lease.stdout.readline().strip().split()
        assert ready[0] == 'READY' and int(ready[1]) == 24 and int(ready[2]) == 2, ready
        sdk = Path(lease.stdout.readline().strip())
        libraries = [Path(lease.stdout.readline().strip()) for _ in range(5)]
        assert sdk.is_dir() and all(p.is_file() for p in libraries)
        manifest = json.loads((sdk / 'sdk_manifest.json').read_text(encoding='utf-8'))
        for row in manifest['files']:
            p = sdk / row['path']
            assert p.is_file() and p.stat().st_size == row['length']
            assert hashlib.sha256(p.read_bytes()).hexdigest() == row['sha256'], row['path']
        objects = []
        for src in (source, root / 'tests/unit/xir/xir_task_constructed_sdk_consumer.c'):
            obj = output / (src.stem + '.obj')
            if args.provider == 'msvc':
                command = [args.compiler, '/nologo', '/std:c11', '/utf-8', '/experimental:c11atomics',
                           '/MD', '/O2', '/W4', '/WX', '/DNDEBUG', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
                           '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(output)]
                command += ['/I' + str(sdk / p) for p in ('src', 'include', 'generated')]
                command += ['/c', str(src), '/Fo' + str(obj)]
            else:
                command = [args.compiler, '--target=x86_64-windows-msvc', '-std=c11', '-O2',
                           '-Wall', '-Wextra', '-Werror', '-pedantic-errors', '-fms-runtime-lib=dll',
                           '-DNDEBUG', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS',
                           '-D_DLL', '-D_MT', '-I' + str(output)]
                for p in ('src', 'include', 'generated'):
                    command += ['-isystem', str(sdk / p)]
                command += ['-c', str(src), '-o', str(obj)]
            checked(command)
            objects.append(obj)
        executable = output / 'task-sdk.exe'
        checked([args.linker, '/nologo', '/incremental:no', '/out:' + str(executable),
                 *(str(p) for p in objects + libraries), 'kernel32.lib',
                 '/defaultlib:MSVCRT', '/defaultlib:OLDNAMES'])
        actual = checked([str(executable)]).decode('utf-8').strip()
        assert actual == 'TASK_SDK families=8 twoInstances repeatedAwait escapedStickyCopies2 hostedOutcomesAfterCodeDeath lastCodeLease1 ownerBaselines=restored', actual
        record = {'status': 'PASS', 'provider': args.provider, 'output': str(output), 'sdk_files': int(ready[3]),
                  'sdk_sanitizers': manifest['sanitizers'], 'actual': actual,
                  'source': {'bytes': source.stat().st_size, 'sha256': hashlib.sha256(source.read_bytes()).hexdigest()},
                  'archives': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in libraries},
                  'manifest_sha256': hashlib.sha256((sdk / 'sdk_manifest.json').read_bytes()).hexdigest(),
                  'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest()}
    finally:
        if lease.poll() is None:
            lease.stdin.write('q')
            lease.stdin.flush()
        _, error = lease.communicate(timeout=10)
        assert lease.returncode == 0, error
    (output / 'result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(record), flush=True)


if __name__ == '__main__':
    main()
