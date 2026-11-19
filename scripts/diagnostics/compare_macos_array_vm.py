"""Compare two exact Array VM builds on each declared macOS layout."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
from statistics import median
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import array_access_benchmark as evidence
import new_path_baseline as source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', action='append', type=Path, required=True)
    parser.add_argument('--binary', action='append', type=Path, required=True)
    parser.add_argument('--commit', action='append', required=True)
    parser.add_argument('--alignment', type=int, choices=(16, 32, 64), required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    assert sys.platform == 'darwin' and platform.machine() == 'arm64'
    parent, candidate = args.root
    args.work.mkdir(parents=True)
    log = args.work / 'result.json'
    result = {'state': 'PREPARING', 'alignment': args.alignment, 'products': [], 'cases': {}}
    try:
        for root, binary, commit in zip(args.root, args.binary, args.commit, strict=True):
            entry = {'product': evidence.fingerprint(binary), 'source': commit}
            version = evidence.execute([binary, '--version', '--json'], args.work, commit + '-version',
                                       evidence.environment(root))
            assert version['ok']
            data = json.loads(Path(version['streams']['stdout']['path']).read_bytes())
            assert data['commit'] == commit and data['dirty'] is False and data['buildProfile'] == 'Release'
            entry.update(version=data, receipt=version)
            result['products'].append(entry)
        relative = Path('bench/native/push_sum.xr')
        original = (parent / relative).read_bytes()
        assert (candidate / relative).read_bytes() == original
        writeback = b'    var sum: i64 = 0\n    for (var i: i64 = 0; i < n; i++) {'
        assert original.count(writeback) == 1
        literal = original.replace(writeback, writeback.replace(b'i < n;', b'i < 10000000;'))
        computed = original.replace(writeback, b'    const end = n + (n % 10)\n' +
                                    writeback.replace(b'i < n;', b'i < end;'))
        array_sum = b'49999995000000\n'
        cases = (
            ('official', original, array_sum), ('literal', literal, array_sum),
            ('computed', computed, array_sum), ('fib', source.FIB_XR.encode(), b'2178309\n'),
            ('loop100m', source.LOOP_XR.replace('@N@', '100000000').encode(), b'14999999550000005\n'),
            ('mandelbrot', source.MANDEL_XR.encode(), None),
        )
        for name, text, _ in cases:
            path = args.work / (name + '.xr')
            path.write_bytes(text)
            result['cases'][name] = {'input': evidence.fingerprint(path), 'samples': []}
        result['state'] = 'RUNNING'
        evidence.save(log, result)
        for round_number in range(9):
            order = range(2) if round_number % 2 == 0 else (1, 0)
            for name, _, expected in cases if round_number % 2 == 0 else reversed(cases):
                record = result['cases'][name]
                for arm in order:
                    sample = evidence.execute([args.binary[arm], 'run', record['input']['path']], args.work,
                                              f'{name}-{round_number}-{arm}', evidence.environment(args.root[arm]),
                                              expected)
                    sample.update(arm=arm, round=round_number)
                    record['samples'].append(sample)
                    evidence.save(log, result)
                    assert sample['ok']
                    assert not Path(sample['streams']['stderr']['path']).read_bytes()
                    digest = hashlib.sha256(Path(sample['streams']['stdout']['path']).read_bytes()).hexdigest()
                    if expected is None:
                        assert digest == 'c068b829d127ac1037426147a9dab7fa2eec13bf55066866813ad7f28157b9da'
        for row in result['cases'].values():
            row['medians'] = [median(s['processWallSeconds'] for s in row['samples'] if s['arm'] == arm)
                              for arm in range(2)]
            row['candidateToParent'] = row['medians'][1] / row['medians'][0]
        evidence.verify_fingerprints([x['product'] for x in result['products']])
        evidence.verify_fingerprints([x['input'] for x in result['cases'].values()])
        result['officialWithinM2Target'] = result['cases']['official']['medians'][1] <= .600
        result['state'] = 'OBSERVED'
    except BaseException as error:
        result.update(state='FAILED', error=repr(error))
        raise
    finally:
        evidence.save(log, result)


if __name__ == '__main__':
    main()
