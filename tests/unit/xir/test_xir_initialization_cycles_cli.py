"""Check complete generated-contract fixtures through real CLI/VM/C/MSVC products."""
from pathlib import Path
import argparse
import ast
import json
import os
import re
import sys
import tempfile


def sources(root):
    text = (root/'tests/unit/xir/initialization_cycle_fixture.h').read_text(encoding='utf-8')
    result = {}
    for name in ('pure', 'error'):
        match = re.search(r'initialization_'+name+r'_source\[\]\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', text)
        assert match, name
        result[name] = b''.join(ast.literal_eval(token).encode('utf-8')
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
    evidence = args.evidence.resolve() if args.evidence else None
    if evidence:
        evidence.mkdir(exist_ok=False)
    environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
    records = []
    with tempfile.TemporaryDirectory(prefix='xray-initializers-') as temporary:
        directory = Path(temporary)

        def run(label, command, expected=b''):
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
                (evidence/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
            assert code == 0 and actual == expected and not errors, (label, code, actual, errors)

        fixtures = sources(root)
        for name, source in fixtures.items():
            path = directory/(name+'.xr')
            path.write_bytes(source)
            if evidence:
                (evidence/path.name).write_bytes(source)
            run(name+'-check', [binary, 'check', path])
            if name == 'pure':
                continue
            run('error-vm', [binary, 'run', path], b'42\n')
            generated = directory/'error.c'
            run('error-c', [binary, 'build', path, '-c', '-o', generated],
                ('Generated: '+str(generated)+'\n').encode())
            assert generated.is_file() and b'({' not in generated.read_bytes()
            native = directory/'error.exe'
            run('error-build', [binary, 'build', path, '--toolchain', 'msvc', '-o', native],
                ('Built: '+str(native)+'\n').encode())
            run('error-native', [native], b'42\n')
            if evidence:
                (evidence/'error.c').write_bytes(generated.read_bytes())
    print('Generated contracts: pure compile only; complete ReadErr ring CLI/VM/C/MSVC42 PASS')


if __name__ == '__main__':
    main()
