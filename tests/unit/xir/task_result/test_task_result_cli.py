"""Run the complete four-state TaskResult Source bytes through CLI VM, C11 and native."""
from pathlib import Path
import argparse
import hashlib
import json
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'binary', 'source', 'expect'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.root.resolve() / 'scripts'))
    from xray_process import run_child
    binary = args.binary.resolve()
    source = args.source.read_bytes()
    oracle = json.loads(args.expect.read_text(encoding='utf-8'))
    assert len(source) == oracle['sourceBytes']
    assert hashlib.sha256(source).hexdigest() == oracle['sourceSHA256']
    expected = bytes.fromhex(oracle['stdoutHex'])
    assert len(expected) == oracle['stdoutBytes']
    with tempfile.TemporaryDirectory(prefix='xray-task-result-') as temporary:
        directory = Path(temporary)

        def run(command):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(command, cwd=directory, stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0)
                stderr.seek(0)
                return code, stdout.read(), stderr.read()

        path = directory / 'task-result.xr'
        path.write_bytes(source)
        for verb, wanted in [('check', b''), ('run', expected)]:
            code, actual, errors = run([str(binary), verb, str(path)])
            assert code == 0 and actual == wanted and not errors, (verb, code, actual, errors)
        generated = directory / 'task-result.c'
        code, actual, errors = run([str(binary), 'build', '-c', str(path), '-o', str(generated)])
        assert code == 0 and actual == ('Generated: ' + str(generated) + '\n').encode() and not errors
        assert b'xr_xir_instance_task_await_result_at(view,' in generated.read_bytes()
        native = directory / 'task-result.exe'
        code, actual, errors = run([str(binary), 'build', str(path), '-o', str(native)])
        assert code == 0 and actual == ('Built: ' + str(native) + '\n').encode() and not errors
        path.unlink()
        code, actual, errors = run([str(native)])
        assert code == 0 and actual == expected and not errors, (code, actual, errors)
        print(f'complete TaskResult: {len(source)} source bytes, {len(expected)} stdout bytes, source retired PASS')
        crlf = source.replace(b'\n', b'\r\n')
        assert crlf.replace(b'\r\n', b'\n') == source and crlf != source
        path.write_bytes(crlf)
        for verb, wanted in [('check', b''), ('run', expected)]:
            code, actual, errors = run([str(binary), verb, str(path)])
            assert code == 0 and actual == wanted and not errors, ('CRLF', verb, code, actual, errors)
        print(f'complete CRLF Source {len(crlf)} bytes, unchanged raw stdout PASS')
        cases = [
            ('never-error', 'enum A{Bad}\nconst r=TaskResult<i64,Never>.Error{error:A.Bad}', b'Never'),
            ('wrong-error-bound', 'enum A{Bad}\nenum B{Bad}\nconst r=TaskResult<i64,A>.Error{error:B.Bad}', b'failed checking'),
            ('old-arity', 'const r:TaskResult<i64>=TaskResult<i64,Never>.Cancelled', b'exact explicit arguments'),
            ('poll-argument', 'fn value()->i64{return 7}\nvar task=go value()\ntask.poll(1)', b'zero-argument recipe'),
            ('result-readonly', 'fn value()->i64{return 7}\nconst task=go value()\ntask.awaitResult()', b'mutable stored binding'),
            ('value-union', 'enum A{Bad}\nenum B{Bad}\nfn ordinary(v:A|B){}', b''),
            ('ordinary-unit', 'enum Ordinary{Value{value:null}}\nconst r=Ordinary.Value{value:()}', b'type declaration'),
            ('ctfe', 'fn value()->i64{return 7}\nfn answer()->i64{var t=go value();const r=t.poll();return 0}\nconst r=comptime answer()', b'')]
        for name, text, reason in cases:
            path.write_text(text, encoding='utf-8', newline='\n')
            code, actual, errors = run([str(binary), 'check', str(path)])
            assert code != 0 and not actual and reason in errors, (name, code, actual, errors)
        print('TaskResult Never/E bounds, old arity, ref receiver, ordinary union/Unit and CTFE refusal controls PASS')
        recursive_sources = ['enum Loop { Again { value: TaskResult<i64,Loop> } }\nexport fn answer()->i64{return 42}\n', 'enum Loop { Again { value: TaskResult<i64,Loop> } }\nfn work()->Loop { return Loop.Again { value: TaskResult<i64,Loop>.Success { value: 42 } } }\nexport fn answer()->i64 { var task = go work(); return 42 }\n', 'enum Other { Bad }\nenum Loop { Again { value: TaskResult<i64,Loop|Other> } }\nfn work()->Loop { return Loop.Again { value: TaskResult<i64,Loop|Other>.Success { value: 42 } } }\nexport fn answer()->i64 { var task=go work();return 42 }\n', 'class Box implements Sendable {\n items: Array<Box>\n init(items:Array<Box>){this.items=items}\n}\nenum E{Bad{value:Box}}\nfn work()->TaskResult<i64,E>{return TaskResult<i64,E>.Success{value:42}}\nexport fn answer()->i64{var task=go work();const result=task.awaitResult();return 42}\nprint(answer())\n']
        recursive_sources.append('enum A{Bad}\nenum B{Bad}\nfn work()->TaskResult<i64,A|B>{return TaskResult<i64,A|B>.Error{error:B.Bad}}\nexport fn answer()->i64{var task=go work();const result=task.awaitResult();return 42}\nprint(answer())\n')
        for index, text in enumerate(recursive_sources):
            path.write_text(text, encoding='utf-8', newline='\n')
            code, actual, errors = run([str(binary), 'check', str(path)])
            if index in (1, 2):
                assert code == 1 and not actual and b'failed checking' in errors, (index, code, actual, errors)
            else:
                assert code == 0 and not actual and not errors, (index, code, actual, errors)
                if index >= 3:
                    code, actual, errors = run([str(binary), 'run', str(path)])
                    assert code == 0 and actual == b'42\n' and not errors, (code, actual, errors)
        print('TaskResult recursive E/union Sendable refuse; legal readonly Class identity and declaration-only controls PASS')



if __name__ == '__main__':
    main()
