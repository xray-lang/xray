"""The real test command preserves the complete owned diagnostic report in JSON."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile


def parameter_cases(args, output, environment):
    source = output/'parameter-作用域.xr'
    text = ('const value:i64=99\r\n'
            '/*é😀*/ fn alpha(value:i64,value:i64)->i64{return value}\r\n'
            '/*é😀*/ fn beta(value:i64,value:i64)->i64{return value}\r\n'
            'print(alpha(4,5)+beta(5,6))\r\n').encode('utf-8')
    source.write_bytes(text)
    report = output/'parameters.json'
    commands = []

    def invoke(label, arguments):
        command = [str(args.xray.resolve()), *map(str, arguments)]
        child = subprocess.run(command, env=environment, capture_output=True, timeout=90)
        (output/('parameters-'+label+'.stdout')).write_bytes(child.stdout)
        (output/('parameters-'+label+'.stderr')).write_bytes(child.stderr)
        commands.append(dict(command=command, exit=child.returncode))
        return child

    child = invoke('test', ['test', source, '--report', report])
    assert child.returncode == 1, (child.returncode, child.stdout, child.stderr)
    data = json.loads(report.read_text(encoding='utf-8'))
    result = data['files'][0]
    assert len(data['files']) == 1 and result['sourceStatus'] == 'source-rejected'
    assert result['sourceStage'] == 4 and result['errors'] == 1
    rows = []
    for line_number, line in enumerate(text.splitlines(keepends=True), 1):
        if b'fn ' in line:
            first = line.index(b'value')
            second = line.index(b'value', first+5)
            rows.extend(((line_number, second+1, 'error', 'duplicate parameter name'),
                         (line_number, first+1, 'note', 'first declaration is here')))
    path = str(source).replace('\\', '/')
    expected = ''.join(f'{path}:{line}:{column}: {kind}: {message}\n'
                       for line, column, kind, message in rows)
    assert result['error'].replace('\\', '/') == expected, (result['error'], expected)
    child = invoke('check', ['check', source])
    assert child.returncode == 1 and child.stderr.count(b'duplicate parameter name') == 2
    source.unlink()
    assert json.loads(report.read_text(encoding='utf-8'))['files'][0]['error'] == result['error']
    corrected = text.replace(b'value:i64,value:i64', b'value:i64,other:i64')
    assert corrected != text and corrected.count(b'value:i64,other:i64') == 2
    source.write_bytes(corrected)
    child = invoke('run', ['run', source])
    assert child.returncode == 0 and child.stdout == b'9\n' and not child.stderr, (
        child.returncode, child.stdout, child.stderr)
    assert source.read_bytes() == corrected
    untouched = (
        ('method', 'struct S{pick(x:i64,x:i64)->i64{return x}}\nprint(0)\n', b'duplicate parameter name'),
        ('constructor', 'class Box{value:i64;init(value:i64,value:i64){this.value=value}}\nprint(0)\n', b'duplicate parameter name'),
        ('closure', 'const f=fn(x:i64,x:i64)->i64{return x}\nprint(0)\n', b'duplicate or empty declaration name'))
    for name, text_case, message in untouched:
        negative = output/('unmigrated-'+name+'.xr')
        negative.write_bytes(text_case.encode('utf-8'))
        child = invoke(name, ['check', negative])
        assert child.returncode == 1 and message in child.stderr, (name, child.returncode, child.stderr)
    return dict(roots=2, labels=2, unicode_crlf=True, corrected_stdout_hex='390a',
                parameter_shadows_module=True, equal_names_in_distinct_functions=True,
                parameter_scope_guards_rejected=[row[0] for row in untouched],
                source_sha256=hashlib.sha256(text).hexdigest(),
                corrected_sha256=hashlib.sha256(corrected).hexdigest(), commands=commands)


def method_parameter_cases(args, output, environment):
    source = output/'method-init-作用域.xr'
    text = ('const value:i64=99\r\nenum Fault{Bad}\r\nclass Box {\r\nvalue:i64\r\n'
            '/*é😀*/ init(value:i64,value:i64=0) throws(Fault){this.value=value}\r\n'
            '/*é😀*/ pick(value:i64,value:i64=0) throws(Fault)->i64{return this.value+value}\r\n'
            'other(value:i64,other:i64=0)->i64{return value+other}\r\n}\r\n'
            'var box=try Box(4)\r\nprint(try box.pick(5))\r\n').encode('utf-8')
    source.write_bytes(text)
    report = output/'method-parameters.json'
    commands = []

    def invoke(label, arguments):
        command = [str(args.xray.resolve()), *map(str, arguments)]
        child = subprocess.run(command, env=environment, capture_output=True, timeout=90)
        (output/('method-'+label+'.stdout')).write_bytes(child.stdout)
        (output/('method-'+label+'.stderr')).write_bytes(child.stderr)
        commands.append(dict(command=command, exit=child.returncode))
        return child

    child = invoke('test', ['test', source, '--report', report])
    assert child.returncode == 1, (child.returncode, child.stdout, child.stderr)
    result = json.loads(report.read_text(encoding='utf-8'))['files'][0]
    assert result['sourceStatus'] == 'source-rejected' and result['sourceStage'] == 4
    rows = []
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith(b'/*'):
            first = line.index(b'value')
            second = line.index(b'value', first+5)
            rows.extend(((number, second+1, 'error', 'duplicate parameter name'),
                         (number, first+1, 'note', 'first declaration is here')))
    path = str(source).replace('\\', '/')
    expected = ''.join(f'{path}:{line}:{column}: {kind}: {message}\n'
                       for line, column, kind, message in rows)
    assert len(rows) == 4 and result['error'].replace('\\', '/') == expected
    child = invoke('check', ['check', source])
    assert child.returncode == 1 and child.stderr.count(b'duplicate parameter name') == 2
    source.unlink()
    assert json.loads(report.read_text(encoding='utf-8'))['files'][0]['error'] == result['error']
    corrected = text.replace(b'value:i64,value:i64', b'value:i64,other:i64')
    assert corrected != text and corrected.count(b'value:i64,other:i64') == 3
    source.write_bytes(corrected)
    child = invoke('run', ['run', source])
    assert child.returncode == 0 and child.stdout == b'9\n' and not child.stderr
    generated, native = output/'method-init.c', output/'method-init.exe'
    child = invoke('c', ['build', '-c', source, '-o', generated])
    assert child.returncode == 0 and generated.is_file() and not child.stderr
    child = invoke('native-build', ['build', source, '-o', native])
    assert child.returncode == 0 and native.is_file() and not child.stderr
    assert source.read_bytes() == corrected
    source.unlink()
    child = subprocess.run([str(native)], env=environment, capture_output=True, timeout=90)
    assert child.returncode == 0 and child.stdout == b'9\n' and not child.stderr
    (output/'method-native.stdout').write_bytes(child.stdout)
    return dict(roots=2, labels=2, unicode_crlf=True, corrected_stdout_hex='390a',
                source_bytes=len(text), source_sha256=hashlib.sha256(text).hexdigest(),
                corrected_sha256=hashlib.sha256(corrected).hexdigest(),
                declared_errors=True, defaults=True, same_names_across_scopes=True,
                field_and_this=True, source_removed=True, commands=commands)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--xray', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    directory = output/'long'
    index = 0
    while len(str(directory/'two-independent-items.xr')) <= 512:
        directory = directory/(str(index)+'_'+'x'*78)
        index += 1
    directory.mkdir(parents=True)
    source = directory/'two-independent-items.xr'
    source.write_bytes(b'const alpha:i64=1\nconst alpha:i64=2\nconst beta:i64=3\nconst beta:i64=4\nprint(9)\n')
    assert len(str(source)) > 512
    report = output/'report.json'
    command = [str(args.xray.resolve()), 'test', str(source), '--report', str(report)]
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(args.root.resolve()/'stdlib'), NO_COLOR='1')
    child = subprocess.run(command, env=environment, capture_output=True, timeout=90)
    (output/'stdout').write_bytes(child.stdout)
    (output/'stderr').write_bytes(child.stderr)
    assert child.returncode == 1, (child.returncode, child.stdout, child.stderr)
    data = json.loads(report.read_text(encoding='utf-8'))
    assert len(data['files']) == 1
    result = data['files'][0]
    assert result['errors'] == 1 and result['sourceStatus'] == 'source-rejected'
    assert result['sourceStage'] == 4
    error = result['error'].replace('\\', '/')
    path = str(source).replace('\\', '/')
    expected = ''.join(f'{path}:{line}:7: {kind}: {message}\n' for line, kind, message in (
        (2, 'error', 'duplicate declaration name'), (1, 'note', 'first declaration is here'),
        (4, 'error', 'duplicate declaration name'), (3, 'note', 'first declaration is here')))
    assert len(error) > 512 and error == expected, (len(error), error, expected)
    assert source.resolve().is_relative_to(output)
    source.unlink()  # The producer process closed its source handle and owned report.
    assert json.loads(report.read_text(encoding='utf-8'))['files'][0]['error'] == result['error']
    summary = dict(status='PASS', command=command, executable_sha256=hashlib.sha256(args.xray.read_bytes()).hexdigest(),
                   path_length=len(str(source)), error_length=len(error), roots=2, labels=2,
                   source_removed=True, report_sha256=hashlib.sha256(report.read_bytes()).hexdigest(), output=str(output))
    summary['parameters'] = parameter_cases(args, output, environment)
    summary['method_parameters'] = method_parameter_cases(args, output, environment)
    (output/'result.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
