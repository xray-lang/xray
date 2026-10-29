"""Consume complete binary-operator Source cases through actual CLI products."""
from pathlib import Path
import argparse
import ast
import json
import os
import re
import sys
import tempfile


def binary_sources(root):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'conversion'))
    from test_conversion_cli import conversion_sources
    values = conversion_sources(root)
    header = (root/'tests/unit/xir/binary_fixture.h').read_text(encoding='utf-8')
    for name, key in (('binary_source', 'control-flow'), ('binary_expected', 'expected_control-flow')):
        match = re.search(r'\b'+name+r'\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', header, re.S)
        assert match, name
        values[key] = b''.join(ast.literal_eval(token).encode('latin1')
                               for token in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
    assert values['control-flow'] == (root/'demos/01-basics/control_flow.xr').read_bytes()
    assert values['expected_control-flow'] == (root/'tests/smoke/expected/control_flow.txt').read_bytes()
    header = (root/'tests/unit/xir/global_callable_fixture.h').read_text(encoding='utf-8')
    for name, key in (('global_callable_source', 'original704'), ('global_callable_expected', 'expected_original704')):
        match = re.search(r'\b'+name+r'\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', header, re.S)
        assert match, name
        values[key] = b''.join(ast.literal_eval(token).encode('latin1')
                               for token in re.findall(r'"(?:\\.|[^"\\])*"', match[1]))
    assert len(values['original704']) == 704 and len(values['contextual']) == 730
    return values


def binary_refusals():
    return (
        ('mixed-sign', 'fn f(a:i8,b:u8)->i8{return a+b}\n', 'integer signedness requires an explicit cast'),
        ('equality-types', 'fn f(a:i8,b:i64)->bool{return a==b}\n', 'equality requires the same declared operand type'),
        ('string-sub', 'fn f(a:string,b:string)->string{return a-b}\n', 'operator is not defined for string operands'),
        ('float-rem', 'fn f(a:f32,b:f64)->f64{return a%b}\n', 'operator is not defined for floating operands'),
        ('integer-float', 'fn f(a:i64,b:f64)->f64{return a+b}\n', 'operator requires a declared concrete operand contract'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    values = binary_sources(root)
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    records = []
    with tempfile.TemporaryDirectory(prefix='xray-hir-binary-') as temporary:
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

        for name in ('control-flow', 'first', 'second', 'original704', 'contextual'):
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
        for name, text, reason in binary_refusals():
            source = directory/(name+'.xr');source.write_text(text, encoding='utf-8')
            run(name, [binary, 'check', source], refusal=reason)
        for name, reason in (
                ('044_narrow_integer_literal_overflow', 'integer literal is outside its contextual type'),
                ('integer_literal_exceeds_uint64', 'integer literal exceeds u64 range')):
            run(name, [binary, 'check', root/'tests/compile_errors/type'/(name+'.xr')], refusal=reason)
    print('Binary HIR: full control flow/constants/contextual CLI VM/C/MSVC and declared operator refusals PASS')


if __name__ == '__main__':
    main()
