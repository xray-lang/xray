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
        ('closure', 'const f=fn(x:i64,x:i64)->i64{return x}\nprint(0)\n', b'duplicate parameter name'))
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



def interface_parameter_cases(args, output, environment):
    source = output/'interface-参数.xr'
    text = (
        'const value:i64=99\r\n'
        'enum Fault{Bad}\r\n'
        'interface Reader {\r\n'
        '/*é😀*/ first(value:i64,value:i64) throws(Fault)->i64\r\n'
        '/*é😀*/ second(value:i64,value:i64) throws(Fault)->i64\r\n'
        '}\r\n'
        'struct Pair implements Reader {\r\n'
        'first(value:i64,other:i64) throws(Fault)->i64{return value+other}\r\n'
        'second(value:i64,other:i64) throws(Fault)->i64{return value+other}\r\n'
        '}\r\n'
        'fn answer<T:Reader>(reader:T) throws(Fault)->i64{return try reader.first(4,5)}\r\n'
        'print(try answer(Pair{}))\r\n').encode('utf-8')
    assert len(text) == 455 and hashlib.sha256(text).hexdigest() == 'd390c03a325d97a1c2e234092bebefd24aeb895e67e17cb26fed56907aec8251'
    source.write_bytes(text)
    report = output/'interface-parameters.json'
    commands = []

    def invoke(label, arguments):
        command = [str(args.xray.resolve()), *map(str, arguments)]
        child = subprocess.run(command, env=environment, capture_output=True, timeout=90)
        (output/('interface-'+label+'.stdout')).write_bytes(child.stdout)
        (output/('interface-'+label+'.stderr')).write_bytes(child.stderr)
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
    assert corrected != text and corrected.count(b'value:i64,other:i64') == 4
    source.write_bytes(corrected)
    child = invoke('run', ['run', source])
    assert child.returncode == 0 and child.stdout == b'9\n' and not child.stderr
    generated, native = output/'interface-parameters.c', output/'interface-parameters.exe'
    child = invoke('c', ['build', '-c', source, '-o', generated])
    assert child.returncode == 0 and generated.is_file() and not child.stderr
    child = invoke('native-build', ['build', source, '-o', native])
    assert child.returncode == 0 and native.is_file() and not child.stderr
    assert source.read_bytes() == corrected
    source.unlink()
    child = subprocess.run([str(native)], env=environment, capture_output=True, timeout=90)
    assert child.returncode == 0 and child.stdout == b'9\n' and not child.stderr
    (output/'interface-native.stdout').write_bytes(child.stdout)
    guards = {'member': ('interface I{f()->i64\nf()->i64}\nprint(9)\n', 'duplicate or empty declaration name'), 'default': ('interface I{f(value:i64=0)->i64}\n', "expected ')'"), 'type': ('interface I{f(value:Missing)->i64}\n', 'name does not resolve to an admitted nominal type'), 'error': ('interface I{f() throws(i64)->i64}\n', 'error type must satisfy Error'), 'generic': ('interface I{f<T,T>(value:T)->T}\n', "duplicate type parameter name 'T'"), 'closure': ('const f=fn(value:i64,value:i64)->i64{return value}\nprint(9)\n', 'duplicate parameter name')}
    for name, (guard, reason) in guards.items():
        path = output/("interface-guard-"+name+".xr")
        path.write_text(guard, encoding="utf-8")
        child = invoke("guard-"+name, ["check", path])
        assert child.returncode == 1
        if name == 'closure':
            assert child.stderr.count(b'duplicate parameter name') == 1 and child.stderr.count(b'first declaration is here') == 1
        else:
            assert child.stderr.endswith((reason+"\n").encode("utf-8"))
    return dict(roots=2, labels=2, unicode_crlf=True, corrected_stdout_hex='390a',
                source_bytes=len(text), source_sha256=hashlib.sha256(text).hexdigest(),
                corrected_sha256=hashlib.sha256(corrected).hexdigest(),
                declared_errors=True, implements_and_generic=True, same_names_across_scopes=True, source_removed=True, commands=commands)

def closure_parameter_cases(args, output, environment):
    source = output/'closure-参数.xr'
    text = ('enum Fault{Bad}\r\n'
            'fn answer<T>(tag:T) throws(Fault)->i64{\r\n'
            'var offset=2\r\n'
            '/*é😀*/ const a=fn(value:i64,value:i64) throws(Fault)->i64{return value+offset}\r\n'
            '/*é😀*/ const b=(value:i64,value:i64)->value+offset\r\n'
            '/*é😀*/ fn local(value:i64,value:i64) throws(Fault)->i64{return value+offset}\r\n'
            'return try a(1,2)+b(1,2)+try local(1,2)\r\n'
            '}\r\n'
            'print(try answer<i64>(0))\r\n').encode('utf-8')
    assert len(text) == 365 and hashlib.sha256(text).hexdigest() == '49606ae78aec27c81129b9d1de6f6cd40243b775328c1a270668002443662b05'
    source.write_bytes(text)
    report = output/'closure-parameters.json'
    commands = []

    def invoke(label, arguments):
        command = [str(args.xray.resolve()), *map(str, arguments)]
        child = subprocess.run(command, env=environment, capture_output=True, timeout=90)
        (output/('closure-'+label+'.stdout')).write_bytes(child.stdout)
        (output/('closure-'+label+'.stderr')).write_bytes(child.stderr)
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
    assert len(rows) == 6 and result['error'].replace('\\', '/') == expected
    child = invoke('check', ['check', source])
    assert child.returncode == 1 and child.stderr.count(b'duplicate parameter name') == 3
    source.unlink()
    assert json.loads(report.read_text(encoding='utf-8'))['files'][0]['error'] == result['error']
    corrected = text.replace(b'value:i64,value:i64', b'value:i64,other:i64')
    assert corrected != text and corrected.count(b'value:i64,other:i64') == 3
    source.write_bytes(corrected)
    child = invoke('run', ['run', source])
    assert child.returncode == 0 and child.stdout == b'9\n' and not child.stderr
    generated, native = output/'closure-parameters.c', output/'closure-parameters.exe'
    child = invoke('c', ['build', '-c', source, '-o', generated])
    assert child.returncode == 0 and generated.is_file() and not child.stderr
    child = invoke('native-build', ['build', source, '-o', native])
    assert child.returncode == 0 and native.is_file() and not child.stderr
    assert source.read_bytes() == corrected
    source.unlink()
    child = subprocess.run([str(native)], env=environment, capture_output=True, timeout=90)
    assert child.returncode == 0 and child.stdout == b'9\n' and not child.stderr
    (output/'closure-native.stdout').write_bytes(child.stdout)
    positives = {
        'module-expression': 'print(((value:i64)->value+1)(8))\n',
        'nested-shadow': 'fn outer(value:i64)->i64{const a=(value:i64)->{const b=(value:i64)->value;return b(value)};const b=(value:i64)->value;return a(value)+b(value)-value}\nprint(outer(9))\n',
        'generic-context': 'fn apply<T>(seed:T)->T{const f:fn(T)->T=fn(value){return value};return f(seed)}\nprint(apply<i64>(9))\n',
        'contextual-ref': 'var value=4\nconst f:fn(ref i64)->i64=fn(value)->i64{value+=1;return value}\nprint(f(ref value)+value-1)\n'}
    positives.update({'const-shadow': 'fn outer()->i64{const value=2;const captured=3;const f=fn(value:i64)->i64{return value+captured};return f(value+4)}\nprint(outer())\n', 'cell-shadow': 'fn outer()->i64{var value=100;var captured=2;const f=fn(value:i64)->i64{captured+=1;return value+captured};const result=f(6);return result+value-100}\nprint(outer())\n', 'self-positive': 'fn outer()->i64{fn local(value:i64)->i64{if(value==0){return 0};return local(value-1)+1};return local(9)}\nprint(outer())\n', 'captured-this': 'class Box{value:i64;init(value:i64){this.value=value};read()->i64{const f=fn(value:i64)->i64{return this.value+value};return f(4)}}\nprint(Box(5).read())\n'})
    for name, program in positives.items():
        path = output/('closure-positive-'+name+'.xr')
        path.write_bytes(program.encode('utf-8'))
        child = invoke('positive-'+name, ['run', path])
        assert child.returncode == 0 and child.stdout == b'9\n' and not child.stderr
    guards = {
        'own-generic': ('const f=fn<T>(value:T)->T{return value}\n', 'closure contract is not implemented in XIR'),
        'const-capture': ('fn outer(){const value=1;const f=fn(){value=2}}\n', 'assignment requires a mutable binding'),
        'local-name': ('fn outer(){fn local(){};fn local(){}}\n', 'duplicate local name'),
        'declared-error': ('enum Fault{Bad}\nfn fail() throws(Fault)->i64{throw Fault.Bad}\nconst f:fn() throws(Fault)->i64=fn() throws(Never)->i64{return try fail()}\n', 'E0331: escaping error exceeds the declared throws type; catch it or declare throws'),
        'context-type': ('const f:fn(i64)->i64=fn(value:string)->i64{return 9}\n', 'callable conversion may only weaken its outer execution contract')}
    guards.update({'self-conflict': ('fn outer(){fn local(local:i64)->i64{return local(0)}}\n', 'indirect call requires its declared signature'), 'this-keyword': ('const f=fn(this:i64)->i64{return this}\n', "'this' is a keyword and cannot be used as an identifier")})
    for name, (program, reason) in guards.items():
        path = output/('closure-guard-'+name+'.xr')
        path.write_bytes(program.encode('utf-8'))
        child = invoke('guard-'+name, ['check', path])
        assert child.returncode == 1 and child.stderr.endswith((reason+'\n').encode('utf-8'))
    ordered = ('class Box{\nfirst(value:i64)->i64{const f=fn(x:i64,x:i64)->i64{return x};return f(value,0)}\n'
               'second(value:i64,value:i64)->i64{return value}\n}\n')
    path = output/'closure-method-order.xr'
    path.write_bytes(ordered.encode('utf-8'))
    child = invoke('method-order', ['check', path])
    errors = [line for line in child.stderr.splitlines() if b'error: duplicate parameter name' in line]
    assert child.returncode == 1 and len(errors) == 2 and b':2:' in errors[0] and b':3:' in errors[1]
    return dict(roots=3, labels=3, unicode_crlf=True, corrected_stdout_hex='390a',
                source_bytes=len(text), source_sha256=hashlib.sha256(text).hexdigest(),
                corrected_sha256=hashlib.sha256(corrected).hexdigest(), declared_errors=True,
                fn_arrow_local=True, captured_cell=True, same_names_across_scopes=True,
                nested_scope_and_shadow=True, generic_context=True, contextual_ref=True,
                source_removed=True, ordered_method_body_diagnostics=True, commands=commands)



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
    summary['interface_parameters'] = interface_parameter_cases(args, output, environment)
    summary['closure_parameters'] = closure_parameter_cases(args, output, environment)
    (output/'result.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
