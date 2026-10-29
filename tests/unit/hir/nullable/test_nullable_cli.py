"""Execute complete nullable programs, original goldens and rejection frontier."""
from pathlib import Path
import argparse
import ast
import json
import os
import re
import sys
import tempfile


def fixtures(root):
    text = (root/'tests/unit/hir/nullable/nullable_fixture.h').read_text(encoding='ascii')
    arrays = {}
    for name, tokens in re.findall(r'\b(\w+)\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', text, re.S):
        arrays[name] = b''.join(ast.literal_eval(token).encode('latin1')
                                for token in re.findall(r'"(?:\\.|[^"\\])*"', tokens))
    pattern = r'\{"([^"]+)",("(?:\\.|[^"\\])*"),("(?:\\.|[^"\\])*"),XR_SOURCE_[A-Z_]+,[0-9]+,[0-9]+\}'
    refusals = [(name, ast.literal_eval(source).encode('latin1'), ast.literal_eval(message))
                for name, source, message in re.findall(pattern, text)]
    assert len(refusals) == 12
    return arrays, refusals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    arrays, refusals = fixtures(root)
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    records = []
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    with tempfile.TemporaryDirectory(prefix='xray-hir-nullable-') as temporary:
        directory = Path(temporary)

        def run(label, command, expected=None, code=0, message=None):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                actual_code = run_child(list(map(str, command)), cwd=directory, env=environment,
                                        stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0); actual = stdout.read(); stderr.seek(0); errors = stderr.read()
            records.append(dict(label=label, command=list(map(str, command)), exit=actual_code))
            if evidence:
                (evidence/(label+'.stdout')).write_bytes(actual)
                (evidence/(label+'.stderr')).write_bytes(errors)
                (evidence/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
            assert actual_code == code, (label, actual_code, actual, errors)
            if expected is not None:
                assert actual == expected, (label, actual, expected)
            assert message in errors if message else not errors, (label, errors)

        positive = ['nullable_operators', 'nullable_contract_factory_lazy_trace', 'complete']
        panics = ['nullable_unwrap_panic', 'nullable_contract_triple_third_fault']
        for name in positive+panics:
            source = directory/(name+'.xr'); source.write_bytes(arrays[name+'_source'])
            expected = arrays[name+'_expected']; panic = name in panics
            message = b'E0413: cannot unwrap a null value' if panic else None
            run(name+'-check', [binary, 'check', source], b'')
            run(name+'-vm', [binary, 'run', source], expected, 1 if panic else 0, message)
            generated = directory/(name+'.c')
            run(name+'-c', [binary, 'build', source, '-c', '-o', generated],
                ('Generated: '+str(generated)+'\n').encode())
            assert generated.is_file() and b'({' not in generated.read_bytes()
            native = directory/(name+'.exe')
            run(name+'-build', [binary, 'build', source, '--toolchain', 'msvc', '-o', native],
                ('Built: '+str(native)+'\n').encode())
            run(name+'-native', [native], expected, 1 if panic else 0, message)
            if evidence:
                for path in (source, generated, native):
                    (evidence/path.name).write_bytes(path.read_bytes())
        for name, text, message in refusals:
            source = directory/(name+'.xr'); source.write_bytes(text)
            run('refuse-'+name, [binary, 'check', source], b'', 1, message.encode())
    print('Nullable HIR: complete programs/goldens, claims and lazy effects, original refusals and null panic through CLI VM/C/MSVC PASS')


if __name__ == '__main__':
    main()
