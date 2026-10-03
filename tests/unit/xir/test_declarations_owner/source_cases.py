"""Exercise actual Source test declarations; no public runner is simulated."""
from pathlib import Path
import argparse
import json
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--stdlib', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    # These values are the public XrXirStatus enum, not diagnostic-text guesses.
    ok, structure, bad_type, bad_value = 0, 1, 3, 4
    cases = [
        ('empty', '', ok, b'count=0\n'),
        ('default', '@test\nfn example() {}\n', ok, b'count=1\nexample:1:0\n'),
        ('zero', '@test(timeout: 0)\nfn example() {}\n', ok, b'count=1\nexample:1:0\n'),
        ('max', '@test(timeout: 2147483647)\nfn example() {}\n', ok, b'count=1\nexample:1:2147483647\n'),
        ('overflow', '@test(timeout: 2147483648)\nfn example() {}\n', structure, b''),
        ('negative', '@test(timeout: -1)\nfn example() {}\n', structure, b''),
        ('empty_parameter', '@test()\nfn example() {}\n', structure, b''),
        ('unknown_parameter', '@test(unknown)\nfn example() {}\n', structure, b''),
        ('multiple_parameters', '@test(skip, timeout: 1)\nfn example() {}\n', structure, b''),
        ('camelcase', '@beforeEach\nfn example() {}\n', structure, b''),
        ('unknown_attribute', '@async\nfn example() {}\n', structure, b''),
        ('conflict', '@test\n@before_all\nfn example() {}\n', structure, b''),
        ('duplicate', '@test\n@test\nfn example() {}\n', structure, b''),
        ('parameter', '@test\nfn example(x:i64) {}\n', bad_type, b''),
        ('generic', '@test\nfn example<T>() {}\n', bad_type, b''),
        ('return', '@test\nfn example()->i64 { return 1 }\n', bad_type, b''),
        ('skip_typechecked', '@test(skip)\nfn example() { missing() }\n', bad_value, b''),
        ('member', 'struct Item { @test fn example(self) {} }\n', structure, b''),
        ('nested', 'fn outer() { @test fn example() {} }\n', structure, b''),
        ('closure', 'const closure = @test fn() {}\n', structure, b''),
        ('throw_only', 'enum Failure { Broken }\n@test\nfn example() { throw Failure.Broken }\n', ok,
         b'count=1\nexample:1:0\n'),
        ('cleanup', '@test\nfn example() { defer { print("cleanup") } }\n', ok,
         b'count=1\nexample:1:0\n'),
        ('export', '@test\nexport fn example() {}\n', ok, b'count=1\nexample:1:0\n'),
        ('initialization', 'enum Failure { Broken { message:string } }\n'
         'fn initialize()->i64 { print("init"); throw Failure.Broken { message:'
         '"owned initialization failure after Program and product destruction" } }\n'
         'var value=initialize()\n@before_all\nfn setup() { print("must not run") }\n'
         '@test\nfn example() { print("must not run") }\n', ok,
         b'initialization sticky; copied failure survives all producers\n'),
    ]
    records = []
    for name, source, status, expected in cases:
        root = args.output/name
        root.mkdir(exist_ok=True)
        entry = root/'main.xr'
        entry.write_text(source, encoding='utf-8', newline='\n')
        command = [str(args.driver), str(root.resolve()), str(entry.resolve()), str(args.stdlib), str(status), name]
        result = subprocess.run(command, capture_output=True, timeout=30)
        (root/'stdout.log').write_bytes(result.stdout)
        (root/'stderr.log').write_bytes(result.stderr)
        records.append(dict(name=name, command=command, returncode=result.returncode,
                            stdout=result.stdout.hex(), expected_stdout=expected.hex()))
        (args.output/'results.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert (result.returncode, result.stdout.replace(b'\r\n', b'\n')) == (0, expected), records[-1]
    print(f'{len(cases)} actual Source declaration and initialization cases PASS')


if __name__ == '__main__':
    main()
