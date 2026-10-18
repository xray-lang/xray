"""Exercise every observed runtime allocation cut for original byte-output consumers."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(ROOT / 'tests' / 'lib'))
from xraytest import proc


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--archives', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    manifest_bytes = (HERE / 'product_driver_cases.json').read_bytes()
    rows = [r for r in json.loads(manifest_bytes)['cases'] if r['id'] == args.case]
    if len(rows) != 1:
        raise ValueError('select exactly one preserved consumer')
    row = rows[0]
    if (row.get('source_context') != 'original_absolute' or row['entry'] != 'run'
            or row.get('arguments') or row.get('stdin_hex')
            or row.get('expected_exit_code', 0) or row.get('stderr_utf8')):
        raise ValueError('this public root entry requires an original zero-argument normal consumer')
    source = ROOT / row['source']
    if sha(source.read_bytes().replace(b'\r\n', b'\n')) != row['source_lf_sha256']:
        raise ValueError('the original consumer changed')
    archive_bytes = args.archives.read_bytes()
    archives = json.loads(archive_bytes)
    for entry in archives['archives']:
        if sha(Path(entry['path']).read_bytes()) != entry['sha256']:
            raise ValueError('linked production archive changed: ' + entry['path'])
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='runtime-', dir=args.output.resolve()))
    expected = row['stdout_utf8'].encode('utf-8')
    expected_path = output / 'expected.stdout'
    expected_path.write_bytes(expected)
    (output / 'source.xr').write_bytes(source.read_bytes())
    (output / 'producer-archives.json').write_bytes(archive_bytes)
    command = [str(binary), str(source), str(expected_path), str(ROOT / 'stdlib')]
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(ROOT / 'stdlib'), NO_COLOR='1')
    identity = sha(binary.read_bytes())
    begun = time.monotonic()
    child = proc.run(command, cwd=output, env=environment, timeout=120)
    (output / 'stdout').write_bytes(child.stdout)
    (output / 'stderr').write_bytes(child.stderr)
    complete = re.search(rb'complete original runtime sites=(\d+) covered=(\d+) final physical=0/0\r?\n\Z', child.stdout)
    passed = (not child.timed_out and child.returncode == 0 and not child.stderr
              and complete is not None and int(complete[1]) > 0 and complete[1] == complete[2])
    if sha(binary.read_bytes()) != identity:
        raise ValueError('runtime observer image changed during execution')
    result = {
        'case_id': row['id'], 'original_source': row['source'],
        'source_lf_sha256': row['source_lf_sha256'], 'expected_sha256': sha(expected),
        'manifest_sha256': sha(manifest_bytes), 'driver_sha256': sha(Path(__file__).read_bytes()),
        'binary': str(binary), 'binary_sha256': identity,
        'producer_archives_sha256': sha(archive_bytes), 'producer_archives': archives['archives'],
        'argv': command, 'cwd': str(output), 'exit_code': child.returncode,
        'timed_out': child.timed_out, 'seconds': time.monotonic() - begun,
        'stdout_hex': child.stdout.hex(), 'stderr_hex': child.stderr.hex(),
        'runtime_allocation_sites': int(complete[1]) if complete else None,
        'covered_runtime_allocation_sites': int(complete[2]) if complete else None,
        'status': 'PASS_COMPLETE_RUNTIME_FI_OWNER_SCOPE' if passed else 'FAIL_RUNTIME_FI_OWNER_SCOPE',
        'scope': 'Actual public module entry; compiler destroyed before execution; same-Call take retry; every observed runtime cut; final physical zero.',
        'remaining': ['Native runtime FI', 'Compiler FI for this exact consumer',
                      'Exact allocated/live/work boundaries and cancellation', 'Full applicable sanitizer/provider matrix'],
        'production_release': False,
    }
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(result['status'] + ': ' + str(output), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
