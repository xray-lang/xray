"""Run the complete cast fixture and preserve its rejection frontier."""
from pathlib import Path
import argparse
import ast
import json
import os
import re
import sys
import tempfile


def cast_sources(root):
    text = (root/'tests/unit/hir/cast/cast_fixture.h').read_text(encoding='utf-8')
    result = {}
    for name in ('cast_source', 'cast_expected'):
        match = re.search(r'\b'+name+r'\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', text, re.S)
        assert match, name
        result[name] = b''.join(ast.literal_eval(token).encode('latin1')
                                for token in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
    return result


def cast_refusal_sources(root):
    text = (root/'tests/unit/hir/cast/cast_fixture.h').read_text(encoding='utf-8')
    pattern = r'\{"([^"]+)",("(?:\\.|[^"\\])*"),("(?:\\.|[^"\\])*"),XR_SOURCE_[A-Z_]+,[0-9]+\}'
    return [(name, ast.literal_eval(source), ast.literal_eval(message))
            for name, source, message in re.findall(pattern, text)]


def cast_panic_sources(root):
    text = (root/'tests/unit/hir/cast/cast_fixture.h').read_text(encoding='utf-8')
    text = text.split('static const CastPanic cast_panics[] = {')[1]
    return [(name, ast.literal_eval(source)) for name, source in
            re.findall(r'\{"([^"]+)",("(?:\\.|[^"\\])*")\}', text)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    values = cast_sources(root)
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    records = []
    assert len(cast_refusal_sources(root)) == 18
    with tempfile.TemporaryDirectory(prefix='xray-hir-cast-') as temporary:
        directory = Path(temporary)

        def run(label, command, expected=b'', refusal=None):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(list(map(str, command)), cwd=directory, env=environment,
                                 stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0); actual = stdout.read(); stderr.seek(0); errors = stderr.read()
            records.append(dict(label=label, command=list(map(str, command)), exit=code))
            if evidence:
                (evidence/(label+'.stdout')).write_bytes(actual)
                (evidence/(label+'.stderr')).write_bytes(errors)
                (evidence/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
            assert (code, actual) == (1 if refusal else 0, expected), (label, code, actual, errors)
            assert refusal.encode() in errors if refusal else not errors, (label, errors)

        source = directory/'cast.xr'; source.write_bytes(values['cast_source'])
        run('check', [binary, 'check', source])
        run('vm', [binary, 'run', source], values['cast_expected'])
        generated = directory/'cast.c'
        run('c', [binary, 'build', source, '-c', '-o', generated], ('Generated: '+str(generated)+'\n').encode())
        assert generated.is_file() and b'({' not in generated.read_bytes()
        native = directory/'cast.exe'
        run('build', [binary, 'build', source, '--toolchain', 'msvc', '-o', native], ('Built: '+str(native)+'\n').encode())
        run('native', [native], values['cast_expected'])
        if evidence:
            for path in (source, generated, native):
                (evidence/path.name).write_bytes(path.read_bytes())
        for name, text, reason in cast_refusal_sources(root):
            source = directory/(name+'.xr');source.write_text(text, encoding='utf-8')
            run(name, [binary, 'check', source], refusal=reason)
        run('original-numeric-as-negative', [binary, 'check', root/'tests/regression/03_operators/0006_as_numeric_cast.xr'],
            refusal='numeric cast requires a concrete nonnullable numeric target')
        for name, text in cast_panic_sources(root):
            source = directory/(name+'.xr');source.write_text(text, encoding='utf-8')
            run(name+'-check', [binary, 'check', source])
            reason = 'E0422: numeric conversion is out of range'
            run(name+'-vm', [binary, 'run', source], refusal=reason)
            native = directory/(name+'.exe')
            run(name+'-build', [binary, 'build', source, '--toolchain', 'msvc', '-o', native], ('Built: '+str(native)+'\n').encode())
            run(name+'-native', [native], refusal=reason)
            if evidence:
                for path in (source, native):
                    (evidence/path.name).write_bytes(path.read_bytes())
    print('Cast HIR: complete numeric and scalar text CLI VM/C/MSVC, original refusals and E0422 failures PASS')


if __name__ == '__main__':
    main()
