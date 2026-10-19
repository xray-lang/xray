"""Prove public VM/native programs consume published Checked output libraries."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    temporary = None
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='xir-checked-stdlib-')
        output = Path(temporary.name).resolve()
    receipts = []
    try:
        for scenario in ('source_absent', 'source_poisoned'):
            stdlib = output / scenario / 'stdlib'
            stdlib.mkdir(parents=True)
            poison = stdlib / 'io/output.xr'
            if scenario == 'source_poisoned':
                poison.parent.mkdir()
                poison.write_text('this is deliberately invalid Xray and must never be read\n', encoding='utf-8')
            before = hashlib.sha256(poison.read_bytes()).hexdigest() if poison.exists() else None
            evidence = output / scenario / 'commands'
            subprocess.run([sys.executable, str(root / 'tests/cli/run_xir_public_cli_product.py'),
                            '--binary', str(binary), '--root', str(root),
                            '--fixtures', str(root / 'tests/fixtures/xir_cli_product'),
                            '--stdlib', str(stdlib), '--output', str(evidence)], check=True, timeout=100)
            summary = json.loads((evidence / 'summary.json').read_text(encoding='utf-8'))
            assert summary['commands'] == 16 and summary['status'] == 'PUBLIC_CLI_VM_NATIVE_LITERAL_PASS'
            assert summary['stdlib_root'] == str(stdlib)
            assert (hashlib.sha256(poison.read_bytes()).hexdigest() if poison.exists() else None) == before
            receipts.append({'scenario': scenario, 'source_sha256': before, 'summary': summary})
        sentinel = output / 'preserved.exe'
        original = b'existing publication survives an invalid explicit stdlib directory'
        sentinel.write_bytes(original)
        invalid = output / 'explicit-missing-directory'
        environment = dict(os.environ, XRAY_STDLIB_PATH=str(invalid), NO_COLOR='1')
        process = subprocess.run([str(binary), 'build', str(root / 'tests/fixtures/xir_cli_product/main.xr'),
                                  '-o', str(sentinel)], env=environment, cwd=root,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        (output / 'invalid-stdlib.stdout').write_bytes(process.stdout)
        (output / 'invalid-stdlib.stderr').write_bytes(process.stderr)
        assert process.returncode == 1 and not process.stdout and process.stderr
        assert sentinel.read_bytes() == original and not invalid.exists()
        record = {'status': 'PUBLIC_CLI_CHECKED_STDLIB_NO_SOURCE_FALLBACK_PASS',
                  'scenarios': receipts, 'commands': 33, 'invalid_explicit_root_exit': process.returncode,
                  'complete_stdlib_installation': 'OPEN', 'production_release': False}
        (output / 'summary.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        print('Public CLI Checked stdlib: missing/poisoned output source, independent VM/native literal outputs PASS')
    finally:
        if temporary:
            temporary.cleanup()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
