"""Run the complete owned cleanup source through compiler evaluation and products."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    fixtures = root/'tests/unit/xir/p11_ctfe_reference/fixtures'
    complete = (root/'tests/unit/xir/ctfe_cleanup/cleanup_owned.xr').read_bytes()
    assert complete.startswith((fixtures/'cleanup_replaced_owned.xr').read_bytes())
    expected = ('true\ntrue\n420\ntrue\n420\n42\ntrue\n446\n42\n'
                'try! failed: Failure.Bad("new é\0界", 31)\n42\n42\n42\n42\n42\n').encode()
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    records = []
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    with tempfile.TemporaryDirectory(prefix='xray-ctfe-cleanup-') as temporary:
        directory = Path(temporary)

        def command(label, argv, output=b'', exit_code=0, rejected=False):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(list(map(str, argv)), cwd=directory, env=environment,
                                 stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0)
                actual = stdout.read()
                stderr.seek(0)
                errors = stderr.read()
            records.append(dict(label=label, argv=list(map(str, argv)), exit_code=code,
                                stdout_sha256=hashlib.sha256(actual).hexdigest(),
                                stderr_sha256=hashlib.sha256(errors).hexdigest()))
            if evidence:
                (evidence/(label+'.stdout')).write_bytes(actual)
                (evidence/(label+'.stderr')).write_bytes(errors)
                (evidence/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
            assert code == exit_code and actual == output, (label, code, actual, errors)
            assert b'comptime panic escaped defer cleanup' in errors if rejected else not errors, (label, errors)

        original = directory/'original.xr'
        original.write_bytes((fixtures/'panic_cleanup_escape.xr').read_bytes()+
                             b'\nconst value = comptime answer()\nprint(value)\n')
        command('original-check', [binary, 'check', original])
        command('original-run', [binary, 'run', original], b'0\n')
        source = directory/'owned.xr'
        source.write_bytes(complete)
        command('owned-check', [binary, 'check', source])
        command('owned-vm', [binary, 'run', source], expected)
        generated = directory/'owned.c'
        command('owned-c', [binary, 'build', source, '-c', '-o', generated],
                ('Generated: '+str(generated)+'\n').encode())
        assert generated.is_file() and b'({' not in generated.read_bytes()
        native = directory/'owned.exe'
        command('owned-build', [binary, 'build', source, '--toolchain', 'msvc', '-o', native],
                ('Built: '+str(native)+'\n').encode())
        command('owned-native', [native], expected)
        for name in ('ctfeDouble', 'ctfeDouble446'):
            source.write_bytes(complete+f'\nconst denied = comptime {name}()\n'.encode())
            command(name, [binary, 'check', source], exit_code=1, rejected=True)
        if evidence:
            (evidence/'owned.xr').write_bytes(complete)
            (evidence/'owned.c').write_bytes(generated.read_bytes())
            (evidence/'summary.json').write_text(json.dumps(dict(state='PASS', commands=len(records),
                source_bytes=len(complete), source_sha256=hashlib.sha256(complete).hexdigest()), indent=2)+'\n')
    print('CTFE cleanup original/full source: VM, verified C, MSVC native and double-failure refusal PASS')


if __name__ == '__main__':
    main()
