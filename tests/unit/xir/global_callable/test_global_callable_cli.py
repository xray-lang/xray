"""Run the complete original global callable input and independent refusal controls."""
from pathlib import Path
import argparse
import ast
import hashlib
import json
import os
import re
import sys
import tempfile


def global_source(root):
    header = (root/'tests/unit/xir/global_callable_fixture.h').read_text(encoding='utf-8')
    values = []
    for name in ('global_callable_source', 'global_callable_expected'):
        match = re.search(r'\b'+name+r'\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', header, re.S)
        assert match, name
        values.append(b''.join(ast.literal_eval(token).encode('latin1')
                               for token in re.findall(r'"(?:\\.|[^"\\])*"', match[1])))
    assert len(values[0]) == 704 and hashlib.sha256(values[0]).hexdigest() == 'dce3c1c73444939bce471d91c0e953f63e0b1e6fac273f9922eff8407dfdec23'
    assert values[1] == b'42 42\ncv\0\xe4\xb8\xad\n'
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
    source_bytes, expected = global_source(root)
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:evidence.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1');records = []
    with tempfile.TemporaryDirectory(prefix='xray-global-callable-') as temporary:
        directory = Path(temporary)

        def run(label, command, stdout_expected=b'', refusal=None):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(list(map(str, command)), cwd=directory, env=environment,
                                 stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0);actual = stdout.read();stderr.seek(0);errors = stderr.read()
            records.append(dict(label=label, command=list(map(str, command)), exit=code))
            if evidence:
                (evidence/(label+'.stdout')).write_bytes(actual);(evidence/(label+'.stderr')).write_bytes(errors)
                (evidence/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
            assert (code, actual) == (1 if refusal else 0, stdout_expected), (label, code, actual, errors)
            assert refusal.encode() in errors if refusal else not errors, (label, errors)

        source = directory/'original.xr';source.write_bytes(source_bytes)
        run('original-check', [binary, 'check', source]);run('original-vm', [binary, 'run', source], expected)
        generated, native = directory/'original.c', directory/'original.exe'
        run('original-c', [binary, 'build', source, '-c', '-o', generated], ('Generated: '+str(generated)+'\n').encode())
        assert b'({' not in generated.read_bytes()
        run('original-build', [binary, 'build', source, '--toolchain', 'msvc', '-o', native], ('Built: '+str(native)+'\n').encode())
        run('original-native', [native], expected)
        if evidence:
            for path in (source, generated, native):(evidence/path.name).write_bytes(path.read_bytes())
        controls = {
            'inline-control': source_bytes.replace(b'apply(inferred)', b'apply(fn(value:i64)->i64{return value+1})'),
            'unconsumed-control': source_bytes.replace(b'apply(inferred)', b'42'),
            'ordinary-unused': b'fn hold(callback:fn(i64)->i64)->i64{return 42}\nfn value(x:i64)->i64{return x+1}\nprint(hold(value))\n',
            'rooted-callback': b'var state:i64=1\nfn apply(callback:fn(i64)->i64)->i64{return callback(41)}\n'
                b'const callback=fn(value:i64){return value+state}\nprint(apply(callback))\n',
        }
        for name, text in controls.items():
            path = directory/(name+'.xr');path.write_bytes(text)
            run(name+'-check', [binary, 'check', path]);run(name+'-vm', [binary, 'run', path], b'42\n' if name in ('ordinary-unused','rooted-callback') else expected)
        refusals = (
            ('root-required', 'var state:i64=1\nfn rooted()->i64{return state}\nconst task=go rooted()\nprint(try await task)\n',
             'GO target requires the current instance root execution'),
            ('nosuspend-reference', 'fn use(callback:nosuspend fn(i64)->i64)->i64{return callback(41)}\n'
             'fn value(x:i64)->i64{return x+1}\nprint(use(value))\n', 'qualified reference requires an explicit target promise'),
            ('nosuspend-lambda', 'import time as Clock\nfn use(callback:nosuspend fn()->i64)->i64{return callback()}\n'
             'print(use(fn()->i64{Clock.sleep(1);return 42}))\n', 'declared no_suspend function may suspend or call an unqualified callable'),
            ('declared-errors', 'enum E{Bad}\nfn use(callback:fn()->i64)->i64{return callback()}\n'
             'print(use(fn() throws(E)->i64{throw E.Bad}))\n', 'callback argument requires exact shape and an admitted advertised bound'),
        )
        for name, text, reason in refusals:
            path = directory/(name+'.xr');path.write_text(text, encoding='utf-8');run(name, [binary, 'check', path], refusal=reason)
    print('Global callable original704: CLI VM/C/MSVC, separate730/unconsumed/ordinary controls and NoSuspend/E refusals PASS')


if __name__ == '__main__':main()
