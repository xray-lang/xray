"""Run the complete typed-await Source bytes through CLI VM, C11 and native."""
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
    with tempfile.TemporaryDirectory(prefix='xray-typed-await-') as temporary:
        directory = Path(temporary)

        def run(command):
            with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
                code = run_child(command, cwd=directory, stdout=stdout, stderr=stderr, timeout=120)
                stdout.seek(0)
                stderr.seek(0)
                return code, stdout.read(), stderr.read()

        path = directory / 'typed-await.xr'
        path.write_bytes(source)
        for verb, wanted in [('check', b''), ('run', expected)]:
            code, actual, errors = run([str(binary), verb, str(path)])
            assert code == 0 and actual == wanted and not errors, (verb, code, actual, errors)
        generated = directory / 'typed-await.c'
        code, actual, errors = run([str(binary), 'build', '-c', str(path), '-o', str(generated)])
        assert code == 0 and actual == ('Generated: ' + str(generated) + '\n').encode() and not errors
        assert b'xr_xir_task_cancel(view,&task)' in generated.read_bytes()
        native = directory / 'typed-await.exe'
        code, actual, errors = run([str(binary), 'build', str(path), '-o', str(native)])
        assert code == 0 and actual == ('Built: ' + str(native) + '\n').encode() and not errors
        path.unlink()
        code, actual, errors = run([str(native)])
        assert code == 0 and actual == expected and not errors, (code, actual, errors)
        print(f'complete typed await: {len(source)} source bytes, {len(expected)} stdout bytes, source retired PASS')
        crlf = source.replace(b'\n', b'\r\n')
        assert crlf.replace(b'\r\n', b'\n') == source and crlf != source
        path.write_bytes(crlf)
        for verb, wanted in [('check', b''), ('run', expected)]:
            code, actual, errors = run([str(binary), verb, str(path)])
            assert code == 0 and actual == wanted and not errors, ('CRLF', verb, code, actual, errors)
        print(f'complete CRLF Source {len(crlf)} bytes, unchanged raw stdout PASS')
        cases = [
            ('never-bound', 'fn wait(t:Task<i64>) throws(Never)->i64{return try await t}', b'E0331'),
            ('one-bound', 'enum A{Bad}\nfn wait(t:Task<i64,A>) throws(A)->i64{return try await t}', b'E0331'),
            ('union-bound', 'enum A{Bad}\nenum B{Bad}\nfn wait(t:Task<i64,A|B>) throws(A|B)->i64{return try await t}', b'E0331'),
            ('fake-identity', 'enum Cancelled{Cancelled}\nfn wait(t:Task<i64>) throws(Cancelled)->i64{return try await t}', b'E0331'),
            ('readonly', 'fn value()->i64{return 7}\nconst task=go value()\ntask.cancel()', b'mutable stored binding'),
            ('argument', 'fn value()->i64{return 7}\nvar task=go value()\ntask.cancel(1)', b'cancellation recipe'),
            ('poll-unadmitted', 'fn value()->i64{return 7}\nvar task=go value()\ntask.poll()', b'cancellation recipe'),
            ('result-unadmitted', 'fn value()->i64{return 7}\nvar task=go value()\ntask.awaitResult()', b'cancellation recipe'),
            ('ctfe', 'fn value()->i64{return 7}\nfn answer()->i64{var task=go value();task.cancel();return 0}\nconst result=comptime answer()', b'')]
        for name, text, reason in cases:
            path.write_text(text, encoding='utf-8', newline='\n')
            code, actual, errors = run([str(binary), 'check', str(path)])
            assert code != 0 and not actual and reason in errors, (name, code, actual, errors)
        print('typed Error bounds, authentic identity, ref receiver and unadmitted Task/CTFE controls PASS')


if __name__ == '__main__':
    main()
