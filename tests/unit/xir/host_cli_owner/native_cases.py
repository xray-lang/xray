"""Compile actual Source products against a held SDK-owned public launcher."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'sdk', 'lease', 'cases', 'output', 'compiler', 'linker', 'provider'):
        parser.add_argument('--'+name, required=True)
    args = parser.parse_args()
    root, sdk, cases, output = map(lambda value: Path(value).resolve(), (args.root, args.sdk, args.cases, args.output))
    output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(root/'tests/unit/xir'))
    import sdk_native_consumers
    sys.path.insert(0, str(root/'scripts'))
    from derive_xir_sdk_abi import prepare
    records = []

    def run(label, command, expected=(0, None, None)):
        start = time.perf_counter()
        result = subprocess.run(command, capture_output=True, timeout=120)
        (output/(label+'.stdout')).write_bytes(result.stdout)
        (output/(label+'.stderr')).write_bytes(result.stderr)
        records.append(dict(label=label, command=list(map(str, command)), returncode=result.returncode,
                            seconds=time.perf_counter()-start, stdout=result.stdout.hex(), stderr=result.stderr.hex()))
        (output/'record.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert result.returncode == expected[0], records[-1]
        assert expected[1] is None or result.stdout == expected[1], records[-1]
        assert expected[2] is None or result.stderr == expected[2], records[-1]
        return result.stdout

    # Reuse the established provider command construction while recording every
    # actual compile and link command; this does not change runtime output bytes.
    serial = 0
    def checked(command, **kwargs):
        nonlocal serial
        assert not kwargs
        serial += 1
        return run('command-'+str(serial), command)
    sdk_native_consumers.checked = checked
    manifest = json.loads((sdk/'sdk_manifest.json').read_text(encoding='utf-8'))
    lease = subprocess.Popen([args.lease, str(sdk), str(sdk/'sdk_manifest.json')],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, encoding='utf-8')
    try:
        ready = lease.stdout.readline().strip().split()
        assert ready == ['READY', str(manifest['value_abi']), str(manifest['crt']), str(len(manifest['files']))], ready
        actual_root = Path(lease.stdout.readline().strip())
        libraries = [lease.stdout.readline().strip() for _ in range(5)]
        assert actual_root.resolve() == sdk and all(Path(path).is_file() for path in libraries)
        relative = 'src/execution/xr_xir_native_main.inc.c'
        row = next(item for item in manifest['files'] if item['path'] == relative)
        launcher = sdk/relative
        assert row['kind'] == 3 and hashlib.sha256(launcher.read_bytes()).hexdigest() == row['sha256']
        prepare(output/'abi')
        abi_object, abi_exe = output/'abi.obj', output/'abi.exe'
        sdk_native_consumers.compile_source(args, sdk, output/'abi/abi_probe.c', abi_object)
        sdk_native_consumers.link(args, abi_exe, [str(abi_object)], libraries)
        actual = json.loads(run('abi', [str(abi_exe)]))
        expected = json.loads((output/'abi/EXPECTED.json').read_text())['rows']
        assert actual == [dict(id=row['id'], value=row['value']) for row in expected] and len(actual) == 220
        launcher_object = output/'launcher.obj'
        sdk_native_consumers.compile_source(args, sdk, launcher, launcher_object, 'host_fixture_program')
        for record in json.loads((cases/'vm-record.json').read_text()):
            name = record['name']
            generated = cases/(name+'.c')
            assert hashlib.sha256(generated.read_bytes()).hexdigest() == record['generated_sha256']
            assert not re.search(r'\(\s*\{', generated.read_text(encoding='utf-8'))
            object_file, executable = output/(name+'.obj'), output/(name+'.exe')
            sdk_native_consumers.compile_source(args, sdk, generated, object_file)
            sdk_native_consumers.link(args, executable, [str(object_file), str(launcher_object)], libraries)
            expected = (record['expected_returncode'], bytes.fromhex(record['expected_stdout']), bytes.fromhex(record['expected_stderr']))
            run(name, [str(executable)], expected)
            # Piped stderr stays plain independently of NO_COLOR's value.
            saved = os.environ.get('NO_COLOR')
            try:
                os.environ['NO_COLOR'] = '1'
                run(name+'-no-color', [str(executable)], expected)
            finally:
                if saved is None:
                    del os.environ['NO_COLOR']
                else:
                    os.environ['NO_COLOR'] = saved
        print(args.provider+': SDK-owned main, 220 ABI facts and all byte-exact programs PASS')
    finally:
        if lease.poll() is None:
            lease.stdin.write('q'); lease.stdin.flush()
        _, error = lease.communicate(timeout=15)
        (output/'lease.stderr').write_text(error, encoding='utf-8')
        assert lease.returncode == 0, (lease.returncode, error)


if __name__ == '__main__':
    main()
