"""Compile real Source-generated C and a public host against an admitted SDK."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import sdk_native_consumers as native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('lease', 'generator', 'bundle', 'compiler', 'linker'):
        parser.add_argument('--'+name, required=True)
    parser.add_argument('--provider', choices=('msvc', 'clang', 'zig'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    sdk = Path(args.bundle).resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=args.provider+'-', dir=args.output.resolve()))
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', TEMP=str(work), TMP=str(work))
    records = []

    def checked(command, **kwargs):
        result = subprocess.run(command, cwd=work, env=environment,
                                capture_output=True, timeout=120, **kwargs)
        label = 'command-'+str(len(records)+1)
        (work/(label+'.stdout')).write_bytes(result.stdout)
        (work/(label+'.stderr')).write_bytes(result.stderr)
        records.append(dict(command=list(map(str, command)), returncode=result.returncode))
        (work/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert result.returncode == 0, (command, (result.stdout+result.stderr).decode('utf-8', errors='replace'))
        return result.stdout

    native.checked = checked
    manifest = json.loads((sdk/'sdk_manifest.json').read_text(encoding='utf-8'))
    assert manifest['crt'] == 2
    lease = subprocess.Popen([args.lease, str(sdk), str(sdk/'sdk_manifest.json')],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding='utf-8', env=environment)
    try:
        ready = lease.stdout.readline().strip().split()
        assert ready == ['READY', str(manifest['value_abi']), '2', str(len(manifest['files']))], ready
        admitted = Path(lease.stdout.readline().strip()).resolve()
        libraries = [lease.stdout.readline().strip() for _ in range(5)]
        assert admitted == sdk and all(Path(path).is_file() for path in libraries)
        generated = work/'source-host.c'
        checked([args.generator, str(generated)])
        assert generated.is_file() and not re.search(r'\(\s*\{', generated.read_text(encoding='utf-8'))
        consumer = root/'tests/unit/xir/test_xir_host_call_linked.c'
        objects = []
        for source in (generated, consumer):
            object_file = work/(source.stem+'.obj')
            native.compile_source(args, sdk, source, object_file)
            objects.append(str(object_file))
        executable = work/'source-host.exe'
        native.link(args, executable, objects, libraries)
        actual = checked([str(executable)]).replace(b'\r\n', b'\n')
        expected = (
            b'opaque persistent host: state, callable authority, independent owners, child drain and owned result PASS\n'
            b'Production host archive: first suspension cancellation, independent resume and owned result PASS\n'
        )
        assert actual == expected, (actual, expected)
        summary = dict(provider=args.provider, generated_sha256=hashlib.sha256(generated.read_bytes()).hexdigest(),
            generated_bytes=generated.stat().st_size, executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
            sdk_manifest_sha256=hashlib.sha256((sdk/'sdk_manifest.json').read_bytes()).hexdigest(),
            source='tests/fixtures/xir_host_call/root.xr', public_host='tests/unit/xir/test_xir_host_call_linked.c',
            independent_cancel_trace=[1, 1], independent_resume_trace=[1, 11, 11],
            owned_result='held-result', owned_result_bytes=11, status='PASS', production_release=False)
        (work/'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
        print(args.provider+': real Source C + five admitted SDK archives + public host cancellation/result lifetime PASS')
    finally:
        if lease.poll() is None:
            lease.stdin.write('q'); lease.stdin.flush()
        _, error = lease.communicate(timeout=10)
        assert lease.returncode == 0, error


if __name__ == '__main__':
    main()
