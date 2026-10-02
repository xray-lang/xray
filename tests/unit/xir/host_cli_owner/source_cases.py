"""Produce actual Source C and check byte-exact VM outcomes before native use."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--emitter', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cases = [
        ('multi', root/'tests/fixtures/xir_compile_owner/root.xr', b'owner\xe4\xb8\xad-ok 42 true\n', b'', 0),
        ('initialization', root/'tests/fixtures/xir_initialization_failure/root.xr', b'A\nB\n',
         b'[Uncaught Error] InitFailure.Stopped("initialization-owned-long-payload-after-owner-destruction", 41)\n', 1),
    ]
    programs = {
        'error': (
            'enum Detail { Message { text:string } }\n'
            'enum Failure { Failed { detail:Detail, code:i64, ok:bool, real:f64 } }\n'
            'fn fail() { throw Failure.Failed { detail:Detail.Message { text:"owned"+"-error" }, '
            'code:-7, ok:true, real:1.5 } }\nprint("before")\nfail()\n',
            b'before\n', b'[Uncaught Error] Failure.Failed(Detail.Message("owned-error"), -7, true, 1.5)\n', 1),
        'panic': ('assert(false, "public"+"-panic")\n', b'', b'[Uncaught Panic] E0445: public-panic\n', 1),
        'empty': ('enum EmptyError { Failed }\nfn fail() { throw EmptyError.Failed }\nfail()\n',
                  b'', b'[Uncaught Error] EmptyError.Failed\n', 1),
        'fatal': ('fn bad() { defer { assert(false, "cleanup") } }\nbad()\n', b'',
                  b'E0443: error escaped a defer body: E0445: cleanup\n'
                  b'  a defer body must not let errors escape (spec 8.3.1); this is not catchable\n', 70),
    }
    for name, (source, stdout, stderr, status) in programs.items():
        directory = output/'fixtures'/name
        directory.mkdir(parents=True, exist_ok=True)
        path = directory/'main.xr'
        path.write_text(source, encoding='utf-8', newline='\n')
        cases.append((name, path, stdout, stderr, status))
    records = []
    for name, path, stdout, stderr, status in cases:
        generated = output/(name+'.c')
        command = [str(args.emitter), str(path.parent), str(path), str(root/'stdlib'), str(generated)]
        result = subprocess.run(command, capture_output=True, timeout=60)
        (output/(name+'.vm.stdout')).write_bytes(result.stdout)
        (output/(name+'.vm.stderr')).write_bytes(result.stderr)
        row = dict(name=name, command=command, returncode=result.returncode,
                   expected_returncode=status, expected_stdout=stdout.hex(), expected_stderr=stderr.hex(),
                   actual_stdout=result.stdout.hex(), actual_stderr=result.stderr.hex())
        if generated.exists():
            row['generated_sha256'] = hashlib.sha256(generated.read_bytes()).hexdigest()
        records.append(row)
        (output/'vm-record.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert (result.returncode, result.stdout, result.stderr) == (status, stdout, stderr), row
    print('Six actual Source products: independent byte-exact VM outcomes PASS')


if __name__ == '__main__':
    main()
