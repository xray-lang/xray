"""Exercise real component installs, relocation and fail-closed Checked publication."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from run_xir_public_cli_product import process_times


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root, build = args.root.resolve(), args.build.resolve()
    temporary = tempfile.TemporaryDirectory(prefix='xir-installed-product-') if not args.output else None
    output = Path(temporary.name) if temporary else args.output.resolve()
    if not temporary:
        output.mkdir(parents=True, exist_ok=False)
    commands = []
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    environment.pop('XRAY_STDLIB_PATH', None)
    environment['PATH'] = os.pathsep.join(part for part in environment['PATH'].split(os.pathsep)
                                        if not (Path(part)/'clang_rt.asan_dynamic-x86_64.dll').is_file())
    checked_relative = Path('lib/xray/stdlib-xir')
    sdk_relative = Path('lib/xray/xir-runtime-sdk')
    expected = build/'src/xir/generated/stdlib-checked/stdlib-manifest.json'
    sdk_expected = build/'xir-runtime-sdk/sdk_manifest.json'
    verifier = root/'scripts/verify_installed_xir_components.py'

    def run(label, argv, success=True, env=None):
        begun = time.perf_counter()
        process = subprocess.Popen([str(x) for x in argv], cwd=output,
                                   env=env or environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(timeout=120)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        (output/(label+'.stdout')).write_bytes(stdout)
        (output/(label+'.stderr')).write_bytes(stderr)
        commands.append({'label': label, 'argv': [str(x) for x in argv], 'pid': process.pid,
                         'exit_code': process.returncode, 'seconds': time.perf_counter()-begun,
                         'stdout_sha256': hashlib.sha256(stdout).hexdigest(),
                         'stderr_sha256': hashlib.sha256(stderr).hexdigest(), **process_times(process)})
        (output/'commands.json').write_text(json.dumps(commands, indent=2)+'\n', encoding='utf-8')
        assert (process.returncode == 0) == success, (label, process.returncode, stderr)
        return stdout, stderr

    def verify(label, checked, sdk, trusted=expected, publish=False, success=True, sdk_only=False):
        argv = [sys.executable, '-B', verifier, '--directory', checked, '--expected', trusted,
                '--sdk-root', sdk, '--expected-sdk', sdk_expected]
        if publish:
            argv.append('--publish')
        if sdk_only:
            argv.append('--sdk-only')
        return run(label, argv, success)

    def inventory(prefix):
        manifest = json.loads((prefix/'share/xray/install/payload-manifest.json').read_bytes())
        rows = manifest['files']
        assert not any(Path(row['path']).name in ('io-output.native', 'io-output.c', 'pair.json', 'registry-manifest.json') for row in rows)
        actual = {p.relative_to(prefix).as_posix() for p in prefix.rglob('*') if p.is_file()}
        assert actual == {r['path'] for r in rows} | {'share/xray/install/payload-manifest.json'}
        for row in rows:
            path = prefix/row['path']
            assert path.stat().st_size == row['size'] and sha(path) == row['sha256']
            if row['path'].startswith('lib/xray/xir-runtime-sdk/'):
                assert row['component'] == 'sdk'
            if row['path'].startswith('lib/xray/stdlib-xir/'):
                assert row['component'] == 'core'
        return {'files': len(rows), 'components': manifest['components'],
                'commit': manifest['commit'], 'dirty': manifest['dirty']}

    try:
        prefixes = {name: output/('install-'+name) for name in ('core', 'sdk', 'full')}
        for name, prefix in prefixes.items():
            argv = ['cmake', '--install', build, '--prefix', prefix]
            if name != 'full':
                argv += ['--component', 'XrayCore' if name == 'core' else 'XraySDK']
            run('install-'+name, argv)
        inventories = {name: inventory(prefix) for name, prefix in prefixes.items()}
        assert not (prefixes['core']/sdk_relative).exists()
        assert not (prefixes['sdk']/checked_relative).exists()
        assert inventories['core']['components'] == ['core']
        assert inventories['sdk']['components'] == ['sdk']
        assert inventories['full']['components'] == ['core', 'sdk']
        for name in ('core', 'full'):
            verify('verify-'+name, prefixes[name]/checked_relative, prefixes[name]/sdk_relative)
        verify('verify-sdk-only', prefixes['sdk']/checked_relative, prefixes['sdk']/sdk_relative, sdk_only=True)
        program = output/'program'
        shutil.copytree(root/'tests/fixtures/xir_cli_product', program)
        source = program/'main.xr'
        source.write_text(source.read_text(encoding='utf-8') +
            '\nimport time as Clock\nconst initial=Clock.nanos()\nClock.sleep(1)\n'
            'print(Clock.nanos()-initial>=1000000)\n' +
            (root/'tests/fixtures/xir_installed_library_methods/main.xr').read_text(encoding='utf-8'), encoding='utf-8')
        relocated = output/'relocated'
        assert prefixes['full'].resolve().is_relative_to(output) and relocated.resolve().is_relative_to(output)
        prefixes['full'].rename(relocated)
        assert not prefixes['full'].exists()
        library_stdout = (b'42 -42 -0x2a 7 -42 42.0\n'
            b'true 9223372036854775807 -9223372036854775808 true\n'
            b'1 63 0 72057594037927936 2 2\n'
            b'2.5 -2 -3 -2 -3 false -2.5 true false 65 A\n'
            b'A\0B A\0BA\0B true true true 2 5\n')
        expected_stdout, expected_stderr = b'A\0B41 42 42 true true 1.5\ntrue\n'+library_stdout, b'E\0R'
        for name, prefix in (('core', prefixes['core']), ('relocated', relocated)):
            binary = prefix/'bin/xray.exe'
            version, errors = run(name+'-version', [binary, '--version', '--json'])
            assert not errors and json.loads(version)['product'] == 'xray-lang'
            publication = (prefix/checked_relative/'stdlib-manifest.json').read_bytes()
            output_source = prefix/'lib/xray/stdlib/io/output.xr'
            time_source = prefix/'lib/xray/stdlib/time/time.xr'
            original = output_source.read_bytes()
            original_time = time_source.read_bytes()
            type_sources = {prefix/('lib/xray/stdlib/types/'+name+'.xr'):
                (prefix/('lib/xray/stdlib/types/'+name+'.xr')).read_bytes()
                for name in ('i64', 'f64', 'bool', 'rune', 'string')}
            for scenario in ('source-absent', 'source-poisoned'):
                if scenario == 'source-absent':
                    output_source.unlink()
                    time_source.unlink()
                    for path in type_sources:
                        path.unlink()
                else:
                    output_source.write_bytes(b'invalid Xray; Checked input must own this declaration\n')
                    time_source.write_bytes(b'invalid time source; Checked input must own the clock closure\n')
                    for path in type_sources:
                        path.write_bytes(b'invalid native type source must not grant library operations\n')
                out, err = run(name+'-'+scenario+'-vm', [binary, 'run', source])
                assert (out, err) == (expected_stdout, expected_stderr)
                generated = output/(name+'-'+scenario+'.c')
                out, err = run(name+'-'+scenario+'-emit', [binary, 'build', '--c-only', source, '-o', generated])
                assert not err and b'({' not in generated.read_bytes()
                native = output/(name+'-'+scenario+'.exe')
                if name == 'core':
                    native.write_bytes(b'existing executable publication')
                    run(name+'-'+scenario+'-native-missing', [binary, 'build', source, '-o', native], False)
                    assert native.read_bytes() == b'existing executable publication'
                else:
                    run(name+'-'+scenario+'-native-build', [binary, 'build', source, '-o', native])
                    out, err = run(name+'-'+scenario+'-native-run', [native])
                    assert (out, err) == (expected_stdout, expected_stderr)
                assert (prefix/checked_relative/'stdlib-manifest.json').read_bytes() == publication
            output_source.write_bytes(original)
            time_source.write_bytes(original_time)
            for path, data in type_sources.items():
                path.write_bytes(data)
            sentinel = output/(name+'-preserved.exe')
            sentinel.write_bytes(b'existing executable publication')
            invalid_env = dict(environment, XRAY_STDLIB_PATH=str(output/'missing-stdlib'))
            run(name+'-invalid-root', [binary, 'build', source, '-o', sentinel], False, invalid_env)
            assert sentinel.read_bytes() == b'existing executable publication'
        checked = relocated/checked_relative
        sdk = relocated/sdk_relative
        old = b'previous publication must survive every failed verification\n'
        attacks = []

        def rejected(label, path, data=None, trusted=expected):
            original = path.read_bytes()
            publication = checked/'stdlib-manifest.json'
            publication.write_bytes(old)
            try:
                if data is None:
                    path.unlink()
                else:
                    path.write_bytes(data)
                verify(label, checked, sdk, trusted, publish=True, success=False)
                assert publication.read_bytes() == old
                attacks.append(label)
            finally:
                path.write_bytes(original)
                publication.write_bytes(expected.read_bytes())

        for name in (row['path'] for row in json.loads(expected.read_bytes())['components']):
            path = checked/name
            rejected('missing-'+name.replace('/', '-'), path)
            data = bytearray(path.read_bytes())
            data[0] ^= 1
            rejected('corrupt-'+name.replace('/', '-'), path, data)
        trusted = output/'invalid-trusted-record.json'
        cases = ('old-schema', 'unknown-schema', 'unknown-field', 'role', 'escape',
                 'module-identity', 'noncanonical-identity', 'empty', 'duplicate-identity',
                 'duplicate-path', 'digest', 'length')
        for case in cases:
            record = json.loads(expected.read_bytes())
            if case == 'old-schema': record['schema'] = 1
            elif case == 'unknown-schema': record['schema'] = 3
            elif case == 'unknown-field': record['unknown'] = 1
            elif case == 'role': record['components'][0]['role'] = 'source'
            elif case == 'escape': record['components'][0]['path'] = '../io-output.chk'
            elif case == 'module-identity': record['components'][0]['module_identity'] = 'untrusted'
            elif case == 'noncanonical-identity': record['components'][0]['module_identity'] = 'stdlib-module-v1:module=2:io:path=13:io//output.xr'
            elif case == 'empty': record['components'] = []
            elif case == 'duplicate-identity': record['components'].append(dict(record['components'][0]))
            elif case == 'duplicate-path': record['components'][1]['path'] = record['components'][0]['path']
            elif case == 'digest': record['components'][0]['sha256'] = '0'*64
            elif case == 'length': record['components'][0]['length'] += 1
            trusted.write_text(json.dumps(record), encoding='utf-8')
            publication = checked/'stdlib-manifest.json'
            publication.write_bytes(old)
            verify('manifest-'+case, checked, sdk, trusted, publish=True, success=False)
            assert publication.read_bytes() == old
            publication.write_bytes(expected.read_bytes())
            attacks.append('manifest-'+case)
        sdk_files = json.loads(sdk_expected.read_bytes())['files']
        sdk_attacks = [('sdk-manifest', sdk/'sdk_manifest.json'),
            ('sdk-host-header', sdk/'src/execution/xr_xir_host_execution.h')]
        sdk_attacks += [('sdk-archive-'+Path(row['path']).stem, sdk/row['path'])
                        for row in sdk_files if row['kind'] == 5]
        assert len(sdk_attacks) == 7
        for case, path in sdk_attacks:
            rejected('missing-'+case, path)
            data = bytearray(path.read_bytes())
            data[0] ^= 1
            rejected('corrupt-'+case, path, data)
        (checked/'stdlib-manifest.json').write_bytes(old)
        verify('untrusted-installed-publication', checked, sdk, success=False)
        assert (checked/'stdlib-manifest.json').read_bytes() == old
        verify('publish-valid-final', checked, sdk, publish=True)
        verify('verify-valid-final', checked, sdk)
        summary = {'status': 'INSTALLED_CORE_SDK_RELOCATION_PUBLIC_LITERAL_AND_REJECTION_PASS',
                   'inventories': inventories, 'attacks': attacks, 'commands': len(commands),
                   'native_provider': 'ordinary MSVC with san0 MD SDK', 'production_release': False}
        (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(summary))
    finally:
        if temporary:
            temporary.cleanup()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
