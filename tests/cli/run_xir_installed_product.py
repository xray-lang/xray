"""Exercise real component installs, relocation and fail-closed pair publication."""
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
    parser.add_argument('--other-object', type=Path)
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
    pair_relative = Path('lib/xray/stdlib-xir/io/output')
    sdk_relative = Path('lib/xray/xir-runtime-sdk')
    expected = build/'src/xir/generated/native-cache/registry-manifest.json'
    sdk_expected = build/'xir-runtime-sdk/sdk_manifest.json'
    verifier = root/'scripts/verify_installed_xir_stdlib_pair.py'

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

    def verify(label, pair, sdk, trusted=expected, publish=False, success=True, sdk_only=False):
        argv = [sys.executable, '-B', verifier, '--directory', pair, '--expected', trusted,
                '--sdk-root', sdk, '--expected-sdk', sdk_expected]
        if publish:
            argv.append('--publish')
        if sdk_only:
            argv.append('--sdk-only')
        return run(label, argv, success)

    def inventory(prefix):
        manifest = json.loads((prefix/'share/xray/install/payload-manifest.json').read_bytes())
        rows = manifest['files']
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
        assert not (prefixes['sdk']/pair_relative).exists()
        assert inventories['core']['components'] == ['core']
        assert inventories['sdk']['components'] == ['sdk']
        assert inventories['full']['components'] == ['core', 'sdk']
        for name in ('core', 'full'):
            verify('verify-'+name, prefixes[name]/pair_relative, prefixes[name]/sdk_relative)
        verify('verify-sdk-only', prefixes['sdk']/pair_relative, prefixes['sdk']/sdk_relative, sdk_only=True)
        program = output/'program'
        shutil.copytree(root/'tests/fixtures/xir_cli_product', program)
        source = program/'main.xr'
        relocated = output/'relocated'
        shutil.copytree(prefixes['full'], relocated)
        expected_stdout, expected_stderr = b'A\0B41 42 42 true true 1.5\n', b'E\0R'
        for name, prefix in (('core', prefixes['core']), ('relocated', relocated)):
            binary = prefix/'bin/xray.exe'
            version, errors = run(name+'-version', [binary, '--version', '--json'])
            assert not errors and json.loads(version)['product'] == 'xray-lang'
            publication = (prefix/pair_relative/'registry-manifest.json').read_bytes()
            output_source = prefix/'lib/xray/stdlib/io/output.xr'
            original = output_source.read_bytes()
            for scenario in ('source-absent', 'source-poisoned'):
                if scenario == 'source-absent':
                    output_source.unlink()
                else:
                    output_source.write_bytes(b'invalid Xray; Checked input must own this declaration\n')
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
                assert (prefix/pair_relative/'registry-manifest.json').read_bytes() == publication
            output_source.write_bytes(original)
            sentinel = output/(name+'-preserved.exe')
            sentinel.write_bytes(b'existing executable publication')
            invalid_env = dict(environment, XRAY_STDLIB_PATH=str(output/'missing-stdlib'))
            run(name+'-invalid-root', [binary, 'build', source, '-o', sentinel], False, invalid_env)
            assert sentinel.read_bytes() == b'existing executable publication'
        pair = relocated/pair_relative
        sdk = relocated/sdk_relative
        old = b'previous publication must survive every failed verification\n'
        attacks = []

        def rejected(label, path, data=None, trusted=expected):
            original = path.read_bytes()
            publication = pair/'registry-manifest.json'
            publication.write_bytes(old)
            try:
                if data is None:
                    path.unlink()
                else:
                    path.write_bytes(data)
                verify(label, pair, sdk, trusted, publish=True, success=False)
                assert publication.read_bytes() == old
                attacks.append(label)
            finally:
                path.write_bytes(original)
                publication.write_bytes(expected.read_bytes())

        for name in ('io-output.chk', 'io-output.native', 'io-output.c'):
            path = pair/name
            rejected('missing-'+name, path)
            data = bytearray(path.read_bytes())
            data[0] ^= 1
            rejected('corrupt-'+name, path, data)
        if args.other_object:
            assert args.other_object.read_bytes() != (pair/'io-output.native').read_bytes()
            rejected('provider-object-swap', pair/'io-output.native', args.other_object.read_bytes())
        trusted = output/'invalid-trusted-record.json'
        cases = ('old-schema', 'unknown-schema', 'unknown-field', 'role', 'escape',
                 'layout', 'semantic', 'sdk-source', 'binding', 'variant')
        for case in cases:
            record = json.loads(expected.read_bytes())
            if case == 'old-schema': record['schema'] = 1
            elif case == 'unknown-schema': record['schema'] = 3
            elif case == 'unknown-field': record['unknown'] = 1
            elif case == 'role': record['components'][0]['role'] = 'source'
            elif case == 'escape': record['components'][0]['path'] = '../io-output.chk'
            elif case == 'layout': record['runtime_layout'] = '0'*64
            elif case == 'semantic': record['semantic_id'] = '0'*64
            elif case == 'sdk-source': record['sdk_identity'] = '0'*64
            elif case == 'binding': record['binding_digest'] = '0'*64
            elif case == 'variant': record['variant'] += '|untrusted=1'
            trusted.write_text(json.dumps(record), encoding='utf-8')
            publication = pair/'registry-manifest.json'
            publication.write_bytes(old)
            verify('manifest-'+case, pair, sdk, trusted, publish=True, success=False)
            assert publication.read_bytes() == old
            publication.write_bytes(expected.read_bytes())
            attacks.append('manifest-'+case)
        for case, path in (('sdk-manifest', sdk/'sdk_manifest.json'),
                           ('sdk-source-file', sdk/json.loads(sdk_expected.read_bytes())['files'][0]['path']),
                           ('sdk-archive', sdk/next(r['path'] for r in json.loads(sdk_expected.read_bytes())['files'] if r['kind'] == 5))):
            rejected('missing-'+case, path)
            data = bytearray(path.read_bytes())
            data[0] ^= 1
            rejected('corrupt-'+case, path, data)
        (pair/'registry-manifest.json').write_bytes(old)
        verify('untrusted-installed-publication', pair, sdk, success=False)
        assert (pair/'registry-manifest.json').read_bytes() == old
        verify('publish-valid-final', pair, sdk, publish=True)
        verify('verify-valid-final', pair, sdk)
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
