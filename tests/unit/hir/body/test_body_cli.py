"""Exercise complete Source binding/place/call programs and admitted String backends."""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'binary', 'client', 'pipeline'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    text = (root/'tests/unit/hir/body/body_fixture.h').read_text(encoding='ascii')
    arrays = {name: b''.join(ast.literal_eval(token).encode('ascii') for token in
              re.findall(r'"(?:\\.|[^"\\])*"', tokens)) for name, tokens in
              re.findall(r'\b(\w+)\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', text)}
    refusals = [(name, ast.literal_eval(source).encode(), ast.literal_eval(expected).encode())
                for name, source, expected in re.findall(
                    r'\{"([^"]+)",("(?:\\.|[^"\\])*"),("(?:\\.|[^"\\])*")\}', text)]
    assert len(refusals) == 14
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    records = []
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    with tempfile.TemporaryDirectory(prefix='xray-body-') as temporary:
        folder = Path(temporary)
        empty = folder/'empty'; empty.mkdir()

        def run(name, command, expected=None, code=0, message=None):
            command = list(map(str, command))
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                actual_code = run_child(command, cwd=folder, env=environment,
                                        stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0); actual = stdout.read(); stderr.seek(0); errors = stderr.read()
            records.append(dict(name=name, command=command, exit=actual_code))
            if evidence:
                (evidence/(name+'.stdout')).write_bytes(actual)
                (evidence/(name+'.stderr')).write_bytes(errors)
                (evidence/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
            assert actual_code == code, (name, actual_code, actual, errors)
            if expected is not None:
                assert actual == expected, (name, actual, expected)
            assert message in errors if message else not errors, (name, errors)

        inputs = [('control', arrays['body_control'], arrays['body_expected']),
                  ('uses', arrays['body_uses'], arrays['body_uses_expected']),
                  ('places', arrays['body_places'], arrays['body_places_expected']),
                  ('callback', arrays['body_callback'], arrays['body_callback_expected']),
                  ('callback-family', arrays['body_callback_family'], arrays['body_callback_family_expected']),
                  ('handlers', arrays['body_handlers'], arrays['body_handlers_expected'])]
        for name, raw, expected in inputs:
            source = folder/(name+'.xr'); source.write_bytes(raw)
            run(name+'-check', [args.binary, 'check', source], b'')
            run(name+'-vm', [args.binary, 'run', source], expected)
            golden = folder/(name+'.out'); golden.write_bytes(expected)
            run(name+'-sdk', [args.client, empty, source, golden])
        original = root/'tests/diff/cases/semantics/string_owning_parameter.xr'
        before = original.read_bytes()
        run('original-check', [args.binary, 'check', original], b'')
        run('original-vm', [args.binary, 'run', original], arrays['body_original_expected'])
        assert original.read_bytes() == before
        for name, raw, expected in refusals:
            source = folder/(name+'.xr'); source.write_bytes(raw)
            message = expected.split(b' ', 3)[3].split(b'\r\n', 1)[0]
            run(name+'-check', [args.binary, 'check', source], b'', 1, message)
            run(name+'-sdk', [args.client, empty, source, '-'], expected)
        # Both original programs are already admitted through StringCell LIR.
        # Their complete contents and independent oracles remain unchanged.
        hello = root/'demos/01-basics/hello.xr'
        hello_expected = subprocess.check_output(['git', 'show', 'HEAD:tests/smoke/expected/hello.txt'], cwd=root)
        assert len(hello_expected) == 76
        backends = [('hello', hello, hello_expected),
                    ('storage', root/'tests/lir/programs/owned-string-storage.xr', b'\xc3\xa9\0\nv12\n\n')]
        for name, source, expected in backends:
            raw = source.read_bytes()
            run(name+'-vm', [args.binary, 'run', source], expected)
            generated = folder/(name+'.c')
            run(name+'-c', [args.binary, 'build', source, '-c', '-o', generated],
                ('Generated: '+str(generated)+'\n').encode())
            assert generated.is_file() and b'({' not in generated.read_bytes()
            native = folder/(name+'.exe')
            run(name+'-build', [args.binary, 'build', source, '--toolchain', 'msvc', '-o', native],
                ('Built: '+str(native)+'\n').encode())
            run(name+'-native', [native], expected)
            for mode in ('raw', 'sccp-inline-inline-sccp'):
                run(name+'-'+mode, [args.pipeline, source, mode, folder/(name+'-'+mode+'.c'),
                    folder/(name+'-'+mode+'.json'), '0', '2', 'normal'], expected*2)
            assert source.read_bytes() == raw
            records.append(dict(name=name+'-source', bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
            if evidence:
                for path in (source, generated, native):
                    (evidence/(name+path.suffix)).write_bytes(path.read_bytes())
    print('Complete body Source, exact refusal, persistent storage and admitted String CLI/C/native/RBC PASS')


if __name__ == '__main__':
    main()
