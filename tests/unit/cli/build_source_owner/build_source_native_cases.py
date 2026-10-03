"""Consume real cmd_build C with the SDK-owned launcher and strict MSVC C11."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import traceback


LIBRARIES = (
    'lib/xray_compile_resources.lib',
    'lib/xray_xir_admission.lib',
    'lib/xray_xir_declarations.lib',
    'lib/xray_xir_scalar.lib',
    'lib/xray_xir_runtime_host.lib',
)
LAUNCHER = 'src/execution/xr_xir_native_main.inc.c'


def file_fact(path):
    data = path.read_bytes()
    return dict(path=str(path), length=len(data), sha256=hashlib.sha256(data).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'driver', 'output', 'sdk', 'cc', 'linker'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    record = dict(status='RUNNING', commands=[], cases=[], inputs_before=[])

    def save():
        (output/'record.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')

    try:
        root, driver, sdk, cc, linker = (
            path.resolve(strict=True) for path in (args.root, args.driver, args.sdk, args.cc, args.linker))
        # Import only the established test command recipe. This is a C consumer
        # gate, not SDK admission or a native Invocation/Target authority proof.
        sys.dont_write_bytecode = True
        sys.path.insert(0, str(root/'tests/unit/xir'))
        import sdk_native_consumers
        provider = argparse.Namespace(provider='msvc', compiler=str(cc), linker=str(linker))
        environment = dict(os.environ, XRAY_STDLIB_PATH=str(root/'stdlib'), NO_COLOR='1')

        def run(label, command, expected=(0, None, None)):
            command = list(map(str, command))
            stdout_path, stderr_path = output/(label+'.stdout'), output/(label+'.stderr')
            item = dict(label=label, command=command, cwd=str(output),
                        stdout_path=str(stdout_path), stderr_path=str(stderr_path),
                        expected_returncode=expected[0],
                        expected_stdout=None if expected[1] is None else expected[1].hex(),
                        expected_stderr=None if expected[2] is None else expected[2].hex())
            record['commands'].append(item)
            save()
            started = time.perf_counter()
            try:
                result = subprocess.run(command, cwd=output, env=environment,
                                        capture_output=True, timeout=120)
            except subprocess.TimeoutExpired as error:
                stdout_path.write_bytes(error.stdout or b'')
                stderr_path.write_bytes(error.stderr or b'')
                item.update(status='TIMEOUT', seconds=time.perf_counter()-started)
                save()
                raise
            stdout_path.write_bytes(result.stdout)
            stderr_path.write_bytes(result.stderr)
            item.update(returncode=result.returncode, seconds=time.perf_counter()-started,
                        stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                        stderr_sha256=hashlib.sha256(result.stderr).hexdigest())
            passed = (result.returncode == expected[0]
                      and (expected[1] is None or result.stdout == expected[1])
                      and (expected[2] is None or result.stderr == expected[2]))
            item['status'] = 'PASS' if passed else 'FAIL'
            save()
            if not passed:
                raise AssertionError(f'{label}: returncode={result.returncode}; '
                                     f'original stdout/stderr: {stdout_path}, {stderr_path}')
            return result.stdout

        serial = 0

        def checked(command, **kwargs):
            nonlocal serial
            if kwargs:
                raise AssertionError(f'Unexpected provider runner options: {tuple(kwargs)}')
            serial += 1
            return run('provider-' + str(serial), command)

        sdk_native_consumers.checked = checked
        manifest_path = sdk/'sdk_manifest.json'
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        if manifest['crt'] != 2 or manifest['c_dialect'] != 11:
            raise AssertionError('This fixed recipe requires the actual MD/C11 SDK')
        record['sdk_payload_sanitizers'] = manifest['sanitizers']
        record['coverage'] = ('Actual cmd_build --c-only and SDK launcher C consumption; '
                              'host-driver ASan does not instrument this SDK payload. '
                              'Manifest checks below identify test inputs, not production SDK admission. '
                              'No native Invocation, guard, or complete Target authority is exercised.')
        libraries = []
        # Check the particular launcher/archive bytes consumed here, without
        # introducing another product SDK reader or claiming the whole closure.
        for relative in (*LIBRARIES, LAUNCHER):
            rows = [row for row in manifest['files'] if row['path'] == relative]
            if len(rows) != 1:
                raise AssertionError(f'Expected one SDK manifest input: {relative}')
            row, fact = rows[0], file_fact(sdk/relative)
            if (row['kind'], row['length'], row['sha256']) != (
                    3 if relative == LAUNCHER else 5, fact['length'], fact['sha256']):
                raise AssertionError(f'SDK input mismatch: {relative}')
            if relative != LAUNCHER:
                libraries.append(str(sdk/relative))

        # The admitted multi-module fixture owns mutable Atomic state inside an
        # imported module. The expected output is independent of generated C/VM.
        multi = root/'tests/fixtures/xir_compile_owner'
        cases = [('multi', multi/'root.xr', b'owner\xe4\xb8\xad-ok 42 true\n')]
        programs = (
            ('utf8', 'print("owner中-ok")\n', b'owner\xe4\xb8\xad-ok\n'),
            ('embedded-nul', 'print("before\\u{0}after")\n', b'before\x00after\n'),
            ('long', 'print("long-owned-' + 'x'*6147 + '")\n', b'long-owned-' + b'x'*6147 + b'\n'),
        )
        for name, source, expected in programs:
            directory = output/'fixtures'/('输入 空格-' + name)
            directory.mkdir(parents=True, exist_ok=True)
            path = directory/'main.xr'
            path.write_text(source, encoding='utf-8', newline='\n')
            cases.append((name, path, expected))

        inputs = [Path(__file__).resolve(), driver, cc, linker, manifest_path,
                  root/'tests/unit/xir/sdk_native_consumers.py', sdk/LAUNCHER,
                  *(sdk/name for name in LIBRARIES), multi/'root.xr', multi/'counter.xr', multi/'label.xr',
                  *(path for _, path, _ in cases[1:])]
        record['inputs_before'] = [file_fact(path) for path in inputs]
        save()
        launcher_object = output/'launcher.obj'
        sdk_native_consumers.compile_source(provider, sdk, sdk/LAUNCHER, launcher_object, 'xray_app_program')
        for name, source, expected in cases:
            generated, object_file, executable = (output/(name+suffix) for suffix in ('.c', '.obj', '.exe'))
            case = dict(name=name, source=str(source), expected_returncode=0,
                        expected_stdout=expected.hex(), expected_stderr='', status='RUNNING')
            record['cases'].append(case)
            save()
            run(name+'-generate', [driver, '--c-only', source, '-o', generated])
            case['generated'] = file_fact(generated)
            save()
            if re.search(r'\(\s*\{', generated.read_text(encoding='utf-8')):
                raise AssertionError(f'{name}: generated C contains a GNU statement expression')
            sdk_native_consumers.compile_source(provider, sdk, generated, object_file)
            sdk_native_consumers.link(provider, executable, [str(object_file), str(launcher_object)], libraries)
            case['executable'] = file_fact(executable)
            # The SDK host launcher uses binary streams. No CRLF normalization,
            # decoding, stripping, or NUL truncation is allowed in this oracle.
            run(name+'-native', [executable], (0, expected, b''))
            case['status'] = 'PASS'
            save()
        record['inputs_after'] = [file_fact(path) for path in inputs]
        if record['inputs_after'] != record['inputs_before']:
            raise AssertionError('A recorded test input changed during native consumption')
        record['status'] = 'PASS'
        save()
        print('4 real cmd_build C products: multi-module state, UTF8, NUL and >6KiB string; '
              'SDK-owned main + five archives + strict MSVC C11 + byte-exact native PASS')
    except Exception:
        record['status'] = 'FAIL'
        record['failure'] = traceback.format_exc()
        (output/'failure.txt').write_text(record['failure'], encoding='utf-8')
        save()
        raise


if __name__ == '__main__':
    main()
