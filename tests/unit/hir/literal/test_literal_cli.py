"""Exercise the complete installed consumer's numeric sources on actual CLI products."""
from pathlib import Path
import argparse
import ast
import json
import os
import re
import sys
import tempfile


def consumer_sources(root):
    source = (root/'tests/unit/xir/source_sdk_consumer.c').read_text(encoding='utf-8')
    block = source[source.index('static void compiler_constants('):]
    block = block[:block.index('\nstatic ', 10)]
    values = {}
    for name in ('first', 'second', 'expected_first', 'expected_second'):
        match = re.search(r'\b'+name+r'\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', block, re.S)
        assert match, name
        values[name] = b''.join(ast.literal_eval(token).encode('latin1')
                               for token in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
    assert len(values['first']) == 739 and len(values['second']) == 145
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    values = consumer_sources(root)
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    records = []
    with tempfile.TemporaryDirectory(prefix='xray-hir-literals-') as temporary:
        directory = Path(temporary)

        def run(label, command, expected=b'', refusal=None):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(list(map(str, command)), cwd=directory, env=environment,
                                 stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0)
                actual = stdout.read()
                stderr.seek(0)
                errors = stderr.read()
            records.append(dict(label=label, command=list(map(str, command)), exit=code))
            if evidence:
                (evidence/(label+'.stdout')).write_bytes(actual)
                (evidence/(label+'.stderr')).write_bytes(errors)
                (evidence/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
            assert (code, actual) == (1 if refusal else 0, expected), (label, code, actual, errors)
            assert refusal.encode() in errors if refusal else not errors, (label, errors)

        for name in ('first', 'second'):
            source = directory/(name+'.xr')
            source.write_bytes(values[name])
            expected = values['expected_'+name]
            run(name+'-check', [binary, 'check', source])
            run(name+'-vm', [binary, 'run', source], expected)
            generated = directory/(name+'.c')
            run(name+'-c', [binary, 'build', source, '-c', '-o', generated],
                ('Generated: '+str(generated)+'\n').encode())
            assert generated.is_file() and b'({' not in generated.read_bytes()
            native = directory/(name+'.exe')
            run(name+'-build', [binary, 'build', source, '--toolchain', 'msvc', '-o', native],
                ('Built: '+str(native)+'\n').encode())
            run(name+'-native', [native], expected)
            if evidence:
                (evidence/(name+'.xr')).write_bytes(values[name])
                (evidence/(name+'.c')).write_bytes(generated.read_bytes())
        for name, declaration in (('f64-inexact', 'f64 = 9007199254740993'),
                                  ('f32-inexact', 'f32 = 16777217'),
                                  ('nullable-f32-inexact', 'f32? = 16777217')):
            source = directory/(name+'.xr')
            source.write_text('const value: '+declaration+'\nprint(value)\n')
            run(name, [binary, 'check', source], refusal='integer literal is not exactly representable in its floating context')
        for name, reason in (('044_narrow_integer_literal_overflow', 'integer literal is outside its contextual type'),
                             ('integer_literal_exceeds_uint64', 'integer literal exceeds u64 range')):
            run(name, [binary, 'check', root/'tests/compile_errors/type'/(name+'.xr')], refusal=reason)
        for name, text, reason in (
                ('pattern-f32-inexact', 'enum E{A{x:f32}}\nfn f(v:E)->i64{return match(v){E.A{x:16777217}->1,E.A{}->2}}\n',
                 'integer literal is not exactly representable in its floating context'),
                ('pattern-u8-overflow', 'enum E{A{x:u8}}\nfn f(v:E)->i64{return match(v){E.A{x:0..256}->1,E.A{}->2}}\n',
                 'integer literal is outside its contextual type')):
            source = directory/(name+'.xr')
            source.write_text(text)
            run(name, [binary, 'check', source], refusal=reason)
    print('Numeric HIR complete original SDK sources: CLI VM/C/MSVC and original precision/range refusals PASS')


if __name__ == '__main__':
    main()
