"""Consume complete Source conversion cases through the actual CLI products."""
from pathlib import Path
import argparse
import ast
import json
import os
import re
import sys
import tempfile


def conversion_sources(root):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'literal'))
    from test_literal_cli import consumer_sources
    values = consumer_sources(root)
    header = (root/'tests/unit/xir/conversion_fixture.h').read_text(encoding='utf-8')
    for name, key in (('conversion_source', 'contextual'), ('conversion_expected', 'expected_contextual')):
        match = re.search(r'\b'+name+r'\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', header, re.S)
        assert match, name
        values[key] = b''.join(ast.literal_eval(token).encode('latin1')
                               for token in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    parser.add_argument('--refusals-only', action='store_true')
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    values = conversion_sources(root)
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    records = []
    with tempfile.TemporaryDirectory(prefix='xray-hir-conversions-') as temporary:
        directory = Path(temporary)

        def run(label, command, expected=b'', refusal=None):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(list(map(str, command)), cwd=directory, env=environment,
                                 stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0);actual = stdout.read();stderr.seek(0);errors = stderr.read()
            records.append(dict(label=label, command=list(map(str, command)), exit=code))
            if evidence:
                (evidence/(label+'.stdout')).write_bytes(actual)
                (evidence/(label+'.stderr')).write_bytes(errors)
                (evidence/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
            assert (code, actual) == (1 if refusal else 0, expected), (label, code, actual, errors)
            assert refusal.encode() in errors if refusal else not errors, (label, errors)

        for name in (() if args.refusals_only else ('first', 'second', 'contextual')):
            source = directory/(name+'.xr');source.write_bytes(values[name]);expected = values['expected_'+name]
            run(name+'-check', [binary, 'check', source])
            run(name+'-vm', [binary, 'run', source], expected)
            generated = directory/(name+'.c')
            run(name+'-c', [binary, 'build', source, '-c', '-o', generated], ('Generated: '+str(generated)+'\n').encode())
            assert generated.is_file() and b'({' not in generated.read_bytes()
            native = directory/(name+'.exe')
            run(name+'-build', [binary, 'build', source, '--toolchain', 'msvc', '-o', native], ('Built: '+str(native)+'\n').encode())
            run(name+'-native', [native], expected)
            if evidence:
                for path in (source, generated, native):
                    (evidence/path.name).write_bytes(path.read_bytes())
        run('original-conversion-refused', [binary, 'check', root/'tests/compile_errors/type/133_implicit_numeric_conversion_rejected.xr'],
            refusal='usize type is not yet supported by the compiler')
        for name, text, reason in (
                ('Error-proof', 'const value:Error=42\n', 'Error conversion requires an enum proof'),
                ('narrowing-refused', 'fn narrow(value:i64)->i8{return value}\n', 'expression cannot satisfy its declared type'),
                ('nullable-refused', 'fn unwrap(value:i64?)->i64{return value}\n', 'expression cannot satisfy its declared type'),
                ('nosuspend-refused', 'fn use(callback:nosuspend fn(i64)->i64)->i64{return callback(41)}\n'
                 'fn value(x:i64)->i64{return x+1}\nprint(use(value))\n', 'qualified reference requires an explicit target promise')):
            source = directory/(name+'.xr');source.write_text(text, encoding='utf-8')
            run(name, [binary, 'check', source], refusal=reason)
    print('Contextual conversion HIR: complete constants/nullable/callable/Error CLI VM/C/MSVC and original refusals PASS')


if __name__ == '__main__':
    main()
