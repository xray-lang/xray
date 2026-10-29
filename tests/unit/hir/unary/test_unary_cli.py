"""Run the complete numeric unary fixture and preserve its rejection frontier."""
from pathlib import Path
import argparse
import ast
import json
import os
import re
import sys
import tempfile


def unary_sources(root):
    text = (root/'tests/unit/hir/unary/unary_fixture.h').read_text(encoding='utf-8')
    result = {}
    for name in ('unary_source', 'unary_expected'):
        match = re.search(r'\b'+name+r'\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', text, re.S)
        assert match, name
        result[name] = b''.join(ast.literal_eval(token).encode('latin1')
                                for token in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    values = unary_sources(root)
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    records = []
    with tempfile.TemporaryDirectory(prefix='xray-hir-unary-') as temporary:
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

        source = directory/'unary.xr'; source.write_bytes(values['unary_source'])
        run('check', [binary, 'check', source])
        run('vm', [binary, 'run', source], values['unary_expected'])
        generated = directory/'unary.c'
        run('c', [binary, 'build', source, '-c', '-o', generated], ('Generated: '+str(generated)+'\n').encode())
        assert generated.is_file() and b'({' not in generated.read_bytes()
        native = directory/'unary.exe'
        run('build', [binary, 'build', source, '--toolchain', 'msvc', '-o', native], ('Built: '+str(native)+'\n').encode())
        run('native', [native], values['unary_expected'])
        if evidence:
            for path in (source, generated, native):
                (evidence/path.name).write_bytes(path.read_bytes())
        refusals = (
            ('bool', 'fn f(a:bool)->bool{return -a}\n'),
            ('float-complement', 'fn f(a:f64)->f64{return ~a}\n'),
            ('string', 'fn f(a:string)->string{return -a}\n'),
            ('nullable', 'fn f(a:i8?)->i8?{return ~a}\n'))
        for name, text in refusals:
            source = directory/(name+'.xr'); source.write_text(text, encoding='utf-8')
            run(name, [binary, 'check', source], refusal='unary arithmetic requires an integer')
        run('literal-minimum-refusal', [binary, 'check', root/'tests/compile_errors/type/044_narrow_integer_literal_overflow.xr'],
            refusal='integer literal is outside its contextual type')
    print('Unary HIR: complete width/overflow/float CLI VM/C/MSVC and original type refusals PASS')


if __name__ == '__main__':
    main()
