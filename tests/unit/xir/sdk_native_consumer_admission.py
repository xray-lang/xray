"""Reach an injected generator failure after real SDK admission and ABI checks."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lease', required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--linker', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='run-', dir=output))
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', TEMP=str(work), TMP=str(work))
    records = []

    def run(label, command):
        result = subprocess.run(command, cwd=work, env=environment,
                                capture_output=True, timeout=120)
        (work/(label+'.stdout')).write_bytes(result.stdout)
        (work/(label+'.stderr')).write_bytes(result.stderr)
        records.append(dict(label=label, command=command, returncode=result.returncode))
        (work/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        return result

    source = work/'generator_fault.c'
    source.write_text('#include <stdio.h>\nint main(void) { '
                      'fputs("SDK_CONSUMER_GENERATOR_FAULT\\n", stderr); return 73; }\n',
                      encoding='utf-8')
    generator = work/'generator_fault.exe'
    compiled = run('generator-build', [args.compiler, '/nologo', '/std:c11', '/utf-8',
        '/W4', '/WX', '/MD', str(source), '/Fo'+str(work/'generator_fault.obj'),
        '/Fe'+str(generator)])
    assert compiled.returncode == 0, compiled.stderr.decode('utf-8', errors='replace')
    sys.path.insert(0, str(root/'scripts'))
    from derive_xir_sdk_abi import prepare
    prepare(work/'independent-abi')
    expected = json.loads((work/'independent-abi/EXPECTED.json').read_text(encoding='utf-8'))['rows']
    result = run('consumer', [sys.executable, '-B', str(root/'tests/unit/xir/sdk_native_consumers.py'),
        '--lease', args.lease, '--generator', str(generator), '--bundle', str(args.bundle.resolve()),
        '--stdlib', str(root/'stdlib'), '--compiler', args.compiler, '--linker', args.linker,
        '--provider', 'msvc'])
    message = ('msvc: fresh admitted-header '+str(len(expected))+'-field independent ABI PASS').encode()
    assert result.returncode != 0, 'The injected generator must fail'
    assert message in result.stdout, ('Consumer did not finish independent SDK ABI checks', result.stdout, result.stderr)
    assert b'SDK_CONSUMER_GENERATOR_FAULT' in result.stderr, result.stderr
    assert b'During handling of the above exception' not in result.stderr, result.stderr
    unsupported = work/'unsupported-crt'
    unsupported.mkdir()
    manifest = json.loads((args.bundle/'sdk_manifest.json').read_text(encoding='utf-8'))
    manifest['crt'] = 1
    (unsupported/'sdk_manifest.json').write_text(json.dumps(manifest)+'\n', encoding='utf-8')
    rejected = run('unsupported-crt', [sys.executable, '-B', str(root/'tests/unit/xir/sdk_native_consumers.py'),
        '--lease', args.lease, '--generator', str(generator), '--bundle', str(unsupported),
        '--stdlib', str(root/'stdlib'), '--compiler', args.compiler, '--linker', args.linker,
        '--provider', 'msvc'])
    assert rejected.returncode != 0 and b'Native SDK consumer requires MD CRT' in rejected.stderr
    assert not rejected.stdout and b'SDK_CONSUMER_GENERATOR_FAULT' not in rejected.stderr
    (work/'result.json').write_text(json.dumps(dict(
        status='PASS', independent_abi_fields=len(expected), injected_generator_exit=73,
        unsupported_crt_rejected_before_admission=True,
        generated_source_program=False, evidence_directory=str(work)), indent=2)+'\n', encoding='utf-8')
    print('Actual admitted SDK and '+str(len(expected))+' independent ABI fields reached the generator fault; lease cleanup PASS')


if __name__ == '__main__':
    main()
