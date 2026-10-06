"""Verify literal VM and native outputs through the public single-pipeline CLI."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def process_times(process: subprocess.Popen) -> dict:
    if os.name != 'nt':
        return {}
    import ctypes
    from ctypes import wintypes
    times = [wintypes.FILETIME() for _ in range(4)]
    query = ctypes.WinDLL('kernel32', use_last_error=True).GetProcessTimes
    query.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    if not query(process._handle, *[ctypes.byref(item) for item in times]):
        raise ctypes.WinError(ctypes.get_last_error())
    values = [(item.dwHighDateTime << 32) | item.dwLowDateTime for item in times]
    return {'creation_filetime': values[0], 'exit_filetime': values[1]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    root = args.root.resolve()
    fixtures = args.fixtures.resolve()
    if args.output:
        evidence = args.output.resolve()
        evidence.mkdir(parents=True, exist_ok=False)
        temporary = None
    else:
        temporary = tempfile.TemporaryDirectory(prefix='xir-public-cli-product-')
        evidence = Path(temporary.name).resolve()
    records: list[dict] = []
    expected = (
        ('plain_go', root / 'tests/fixtures/xir_go_product/main.xr', b'A\0B\n21\n', b''),
        ('cached_state_go', fixtures / 'main.xr', b'A\0B41 42 42 true true 1.5\n', b'E\0R'),
    )
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / 'stdlib'), NO_COLOR='1')
    initial_binary = digest(binary)

    def save() -> None:
        (evidence / 'commands.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')

    def invoke(label: str, executable: Path, *arguments: str) -> subprocess.CompletedProcess:
        argv = [str(executable), *arguments]
        begun = time.perf_counter()
        process = subprocess.Popen(argv, cwd=root, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            (evidence / (label + '.stdout')).write_bytes(stdout)
            (evidence / (label + '.stderr')).write_bytes(stderr)
            records.append({'label': label, 'argv': argv, 'pid': process.pid, 'exit_code': process.returncode,
                            'timed_out': True, 'seconds': time.perf_counter() - begun, **process_times(process)})
            save()
            raise
        (evidence / (label + '.stdout')).write_bytes(stdout)
        (evidence / (label + '.stderr')).write_bytes(stderr)
        records.append({'label': label, 'argv': argv, 'pid': process.pid, 'exit_code': process.returncode,
                        'timed_out': False, 'seconds': time.perf_counter() - begun,
                        **process_times(process),
                        'stdout_sha256': hashlib.sha256(stdout).hexdigest(),
                        'stderr_sha256': hashlib.sha256(stderr).hexdigest()})
        save()
        return subprocess.CompletedProcess(argv, process.returncode, stdout, stderr)

    def require(condition: bool, message: str) -> None:
        if not condition:
            raise AssertionError(message)

    try:
        version = invoke('version', binary, '--version', '--json')
        require(version.returncode == 0 and not version.stderr, 'public CLI version query failed')
        identity = json.loads(version.stdout)
        require(identity.get('product') == 'xray-lang' and identity.get('target') == 'windows-x86_64',
                f'wrong CLI product or target: {identity}')
        for name, source, stdout, stderr in expected:
            for repeat in range(2):
                result = invoke(name + '-vm-' + str(repeat), binary, 'run', str(source))
                require((result.returncode, result.stdout, result.stderr) == (0, stdout, stderr),
                        f'{name} VM literal output failed: {result.returncode} {result.stdout!r} {result.stderr!r}')
            generated = evidence / (name + '.c')
            emitted = invoke(name + '-emit', binary, 'build', '--c-only', str(source), '-o', str(generated))
            require(emitted.returncode == 0 and not emitted.stderr, f'{name} public C projection failed: {emitted.stderr!r}')
            text = generated.read_bytes()
            require(b'const XrXirProgramSpec xray_app_program' in text and b'XrXirCallEntry' in text,
                    f'{name} lost the canonical native Program and typed call table')
            require(b'XrProto' not in text and b'xrt_vm_run' not in text and b'({' not in text,
                    f'{name} generated C regained a legacy route or GNU statement expression')
            native = evidence / (name + '.exe')
            built = invoke(name + '-build', binary, 'build', str(source), '-o', str(native))
            require(built.returncode == 0 and not built.stderr, f'{name} public native build failed: {built.stderr!r}')
            native_digest = digest(native)
            for repeat in range(2):
                result = invoke(name + '-native-' + str(repeat), native)
                require((result.returncode, result.stdout, result.stderr) == (0, stdout, stderr),
                        f'{name} native literal output failed: {result.returncode} {result.stdout!r} {result.stderr!r}')
                require(digest(native) == native_digest, f'{name} native image changed during execution')
        rejected_library = evidence / 'rejected-library.xr'
        rejected_library.write_text('var serial:i64=40\nexport fn read()->i64 { return serial }\n', encoding='utf-8')
        rejected_entry = evidence / 'rejected-entry.xr'
        rejected_entry.write_text('import "./rejected-library" as Library\nprint(Library.read())\n', encoding='utf-8')
        rejected_output = evidence / 'rejected-output.exe'
        sentinel = b'existing publication must survive invalid library state'
        rejected_output.write_bytes(sentinel)
        for route in ('run', 'build', 'emit'):
            arguments = [str(rejected_entry)] if route == 'run' else (
                ['--c-only', str(rejected_entry), '-o', str(rejected_output)] if route == 'emit' else
                [str(rejected_entry), '-o', str(rejected_output)])
            result = invoke('reject-library-var-' + route, binary, 'run' if route == 'run' else 'build', *arguments)
            require(result.returncode == 1 and not result.stdout and
                    b'library mutable or exported state is not admitted' in result.stderr,
                    f'{route} lost library mutable-state rejection: {result.returncode} {result.stdout!r} {result.stderr!r}')
            require(rejected_output.read_bytes() == sentinel, f'{route} changed an existing publication after source rejection')
        require(digest(binary) == initial_binary, 'public CLI image changed during qualification')
        summary = {'status': 'PUBLIC_CLI_VM_NATIVE_LITERAL_PASS', 'binary': str(binary),
                   'binary_sha256': initial_binary, 'version_json': identity, 'commands': len(records),
                   'expected': [{'name': name, 'stdout_hex': stdout.hex(), 'stderr_hex': stderr.hex()}
                                for name, _, stdout, stderr in expected],
                   'source_root': str(root), 'fixture_root': str(fixtures),
                   'production_release': False, 'physical_release_instrumentation': 'NOT_RUN'}
        (evidence / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
        print('Public CLI: plain GO and cached multi-module state; VM/native independently match literal NUL/typed outputs')
        return 0
    finally:
        save()
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
