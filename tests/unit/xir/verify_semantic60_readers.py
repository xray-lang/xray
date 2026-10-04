"""Qualify every preserved current packet with the freshly built real reader.

Historical inputs remain intact. Framing generation uses independent Python
oracles; acceptance and rejection use the supplied compiled Checked reader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reader', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    assert args.prepare_only or args.reader, 'a freshly built reader is required'
    directory = Path(__file__).parent
    args.output.mkdir(parents=True, exist_ok=True)
    old59 = args.output/'semantic59-60'
    old58 = args.output/'semantic58-60'
    for script, output in (('derive_semantic60_migration.py', old59),
                           ('derive_test_roles_vectors.py', old58)):
        subprocess.run([sys.executable, '-B', str(directory/script),
                        '--output', str(output)], check=True, timeout=30)
    groups = [(sorted(old59.glob('*-current60.chk')), 0, 19),
              (sorted(old59.glob('*-old59.chk')), 1, 19),
              (sorted(old58.glob('*-old.chk')), 1, 18)]
    records = []
    for paths, expected, count in groups:
        assert len(paths) == count, (expected, len(paths), count)
        for path in paths:
            row = {'path': str(path.resolve()), 'expected': expected,
                   'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                   'status': 'NOT_RUN'}
            if not args.prepare_only:
                result = subprocess.run([str(args.reader.resolve()),
                                         str(path.resolve()), str(expected)],
                                        text=True, capture_output=True, timeout=30)
                row.update(exit=result.returncode, stdout=result.stdout,
                           stderr=result.stderr,
                           status='PASS' if result.returncode == 0 else 'FAIL')
                records.append(row)
                (args.output/'reader-results.json').write_text(
                    json.dumps(records, indent=2)+'\n', encoding='utf-8')
                assert result.returncode == 0, row
            else:
                records.append(row)
    assert len(records) == 56
    (args.output/'reader-results.json').write_text(
        json.dumps(records, indent=2)+'\n', encoding='utf-8')
    print('56 preserved packet probes prepared; real reader NOT_RUN' if args.prepare_only
          else '56 real reader probes PASS: 19 current60 / 19 old59 / 18 old22/58')


if __name__ == '__main__':
    main()
