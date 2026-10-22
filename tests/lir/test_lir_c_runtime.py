"""Link real source-generated C to the production standalone output library."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import tempfile
from test_lir_c_native import run
from test_lir_carriers_native import _f64_profile, _f64_reject


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--fixture-driver', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--kind', choices=['msvc', 'gnu'], required=True)
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='lir-runtime-', dir=args.work))
    header = args.include / 'xray_native_runtime.h'
    identity = {'library': str(args.library), 'librarySHA256': hashlib.sha256(args.library.read_bytes()).hexdigest(),
                'header': str(header), 'headerSHA256': hashlib.sha256(header.read_bytes()).hexdigest()}
    print(json.dumps(identity), flush=True)
    cases = {
        'fib': ('fn fib(n:i64)->i64{if(n<2){return n}\nreturn fib(n-1)+fib(n-2)}\nprint(fib(32))\n', b'2178309\n'),
        'sum': ('fn sum(n:i64)->i64{var i=0\nvar total=0\nwhile(i<n){total=total+i\ni=i+1}\nreturn total}\nprint(sum(100000000))\n', b'4999999950000000\n'),
        'calls': ('fn main()->i64{return 42}\nfn show(x:i64){print(x)}\nshow(main())\nshow(-9223372036854775807-1)\nshow(9223372036854775807)\n',
                  b'42\n-9223372036854775808\n9223372036854775807\n'),
        'branch': ('fn choose(flag:bool)->i64{return flag ? 3 : 7}\nprint(choose(true))\nprint(choose(false))\n', b'3\n7\n'),
        'output-unknown': (None, b'5\n'),
        'output-point': (None, b'5\n'),
        'output-range': (None, b'5\n'),
        'typed-i64-min': (None, b'5\n'),
        'typed-u64': (None, b'5\n'),
        'typed-bool': (None, b'5\n'),
        'typed-unit': (None, b'5\n'),
        'typed-root-call': (None, b'5\n'),
    }
    cases['fixed-divrem'] = (
        'fn q(n:i64)->i64{return n/3}\n'
        'fn r(n:i64)->i64{return n%97}\n'
        'print(q(-9223372036854775807-1))\n'
        'print(r(-9223372036854775807-1))\n'
        'print(q(-7))\n'
        'print(r(-98))\n'
        'print(q(9223372036854775807))\n'
        'print(r(9223372036854775807))\n',
        b'-3074457345618258602\n-79\n-2\n-1\n3074457345618258602\n78\n')
    cases['minus-one-divrem'] = (
        'fn q(n:i64)->i64{return n/(-1)}\n'
        'fn r(n:i64)->i64{return n%(-1)}\n'
        'print(q(-9223372036854775807-1))\nprint(r(-9223372036854775807-1))\n'
        'print(q(-7))\nprint(r(-7))\nprint(q(7))\nprint(r(7))\n'
        'print(q(9223372036854775807))\nprint(r(9223372036854775807))\n'
        'print(q(0))\nprint(r(0))\n',
        b'-9223372036854775808\n0\n7\n0\n-7\n0\n-9223372036854775807\n0\n0\n0\n')
    cases['f64-source'] = ('fn carry(x:f64)->f64{print(7)\nreturn x}\ncarry(1.25)\nprint(2)\n', b'7\n2\n')
    cases['f64-fneg-source'] = ('fn negative(x:f64)->f64{\nprint(13)\nreturn -x\n}\n'
                                'negative(1.25)\nprint(2)\n', b'13\n2\n')
    refused_source, refused_c = root / 'refused-fadd.xr', root / 'refused-fadd.c'
    refused_source.write_text('print(1)\nfn add(x:f64)->f64{return x+1.0}\nadd(2.0)\nprint(2)\n', encoding='utf-8')
    refused = subprocess.run([str(args.driver), str(refused_source), str(refused_c)], capture_output=True, timeout=30)
    (root / 'refused-fadd.stderr').write_bytes(refused.stderr)
    assert refused.returncode == 1 and refused.stdout == b'' and not refused_c.exists()
    assert b'C emission:' in refused.stderr and b'operation has no admitted portable C implementation' in refused.stderr
    identity['selectedFadd'] = {'exit': refused.returncode, 'stdoutHex': refused.stdout.hex(),
                                'stderrHex': refused.stderr.hex(), 'publishedC': False}
    typed_results = {'typed-i64-min': ('int64_t', 'INT64_MIN'),
                     'typed-u64': ('uint64_t', 'UINT64_MAX'),
                     'typed-bool': ('_Bool', '1'), 'typed-unit': ('void', None),
                     'typed-root-call': ('int64_t', 'INT64_C(9)')}
    executables = {}
    for name, (program, expected) in cases.items():
        source = root / (name + '.xr')
        generated = root / (name + '.c')
        adapter = root / (name + '-adapter.c')
        executable = root / (name + ('.exe' if os.name == 'nt' else ''))
        if program is None:
            run([args.fixture_driver, name, generated], cwd=root)
        else:
            source.write_text(program, encoding='utf-8')
            run([args.driver, source, generated], cwd=root)
        text = generated.read_text(encoding='ascii')
        assert text.count('const XrtSourceMap xrt_standalone_source_map =') == 1
        assert text.count('xrt_run_standalone(') == 1
        assert '#include "xray_native_runtime.h"' in text and '({' not in text
        assert re.search(r'xrt_print_i64\((?:v|p)[0-9]+, UINT32_C\([0-9]+\)\);', text)
        assert 'invoke_io' not in text and 'fail_io' not in text and 'switch' not in text
        if name == 'fib':
            functions = re.findall(r'static int64_t (xrl_f[0-9]+)\(int64_t p0\) \{(.*?)\n\}', text, re.S)
            assert any(body.count(symbol + '(') == 2 and 'xrt_' not in body for symbol, body in functions)
        if name == 'typed-root-call':
            body = re.search(r'static int64_t xrl_f1\(void\) \{(.*?)\n\}', text, re.S)
            assert body and body.group(1).count('xrl_f1(') == 1
            assert 'xray_lir_entry' not in body.group(1) and 'xrt_' not in body.group(1)
        result_type, expected_result = typed_results.get(name, ('int64_t', 'INT64_C(0)'))
        invoke = ('    entry();\n' if result_type == 'void'
                  else f'    {result_type} result = entry();\n')
        check = '0' if expected_result is None else f'result == {expected_result} ? 0 : 1'
        # Distinct failure-mode adapters must not execute this post-entry marker.
        # Include the real definition once so the typed pointer checks its ABI.
        adapter.write_text('#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n'
            f'#include "{generated.name}"\n'
            'static void on_exit(void) { fputs("EXIT_CALLBACK", stderr); }\n'
            'int main(int argc, char **argv) {\n    (void)argv;\n'
            '    if (argc > 1 && atexit(on_exit) != 0) return 2;\n'
            '    if (argc > 2) {\n        static char buffer[128];\n'
            '        if (setvbuf(stdout, buffer, _IOFBF, sizeof(buffer)) != 0) return 2;\n'
            '        if (fputs("buffered", stdout) == EOF) return 2;\n    }\n'
            f'    {result_type} (*entry)(void) = xray_lir_entry;\n' + invoke +
            '    if (argc > 1) fputs("AFTER_ENTRY", stderr);\n'
            f'    return {check};\n}}\n', encoding='ascii')
        if name in ('f64-source', 'f64-fneg-source'):
            workers = re.findall(r'static double (xrl_f[0-9]+)\(double p0\) \{(.*?)\n\}', text, re.S)
            assert len(workers) == 1, workers
            worker, body = workers[0]
            assert re.search(r'\b' + worker + r'\((?:p|v)[0-9]+\)', text)
            if name == 'f64-source':
                assert 'return p0;' in body
            else:
                assert 'memcpy' in body and '0x7ff8000000000000' in body
                assert '#pragma STDC FENV_ACCESS ON' in text and re.search(r'return v[0-9]+;', body)
            assertion = ('\n_Static_assert(_Generic(&' + worker +
                         ', double (*)(double): 1, default: 0), "source worker type");\n')
            adapter.write_text(adapter.read_text(encoding='ascii') + assertion, encoding='ascii')
            key = 'f64SourceWorker' if name == 'f64-source' else 'f64NegationSourceWorker'
            identity[key] = {'name': worker, 'type': 'double(double)', 'returnsParameter': name == 'f64-source'}
        if args.kind == 'msvc':
            crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                   'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
            command = [args.compiler, '/nologo', '/std:c11', '/W4', '/WX', '/O2', crt,
                       '/I' + str(args.include), adapter, args.library, '/Fe:' + str(executable)]
            if args.asan:
                command += ['/fsanitize=address', '/Zi']
                if 'clang' in args.compiler.name.lower():
                    command += ['/clang:-fsanitize=undefined']
                command += ['/link', '/INCREMENTAL:NO']
        else:
            command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
                       '-I' + str(args.include), adapter, args.library, '-pthread', '-o', executable]
            if args.asan:
                command += ['-fsanitize=address,undefined', '-g']
        if name in ('f64-source', 'f64-fneg-source'):
            options = [str(x) for x in command[1:] if isinstance(x, str) and x.startswith(('/', '-'))
                       and not x.startswith(('/Fe:', '/INCREMENTAL:')) and x not in ('/link', '-o', '-pthread')]
            profile = _f64_profile(args.compiler, args.kind, options, root)
            key = 'f64Source' if name == 'f64-source' else 'f64NegationSource'
            identity[key + 'Profile'] = profile
            if not profile['supported']:
                identity[key] = _f64_reject(command, root, profile['expectedGuard'], executable)
                continue
        run(command, cwd=root)
        completed = run([executable], cwd=root)
        assert completed.stdout == expected and completed.stderr == b'', (name, completed)
        executables[name] = executable
        if name in ('f64-source', 'f64-fneg-source'):
            key = 'f64Source' if name == 'f64-source' else 'f64NegationSource'
            identity[key] = {'status': 'PASS_SOURCE_ADMISSION_AND_EFFECTS',
                             'stdoutHex': completed.stdout.hex(), 'payloadQualification': False}
    # A read-only stdout handle exercises a real write failure, without a stub.
    read_only = root / 'read-only-output'
    read_only.write_bytes(b'')
    locations = {'branch': b'branch.xr:2:1-2:1 ', 'output-unknown': b'unknown source',
                 'output-point': b'odd"\\x5c\xc3\xa9\\x0a.xr:4:3-4:3 ',
                 'output-range': b'odd"\\x5c\xc3\xa9\\x0a.xr:4:3-5:8 ',
                 'typed-unit': b'typed-root.xr:2:1-2:1 '}
    for name, location in locations.items():
        with read_only.open('rb') as stream:
            failure = subprocess.run([str(executables[name]), 'fail'], stdout=stream,
                                     stderr=subprocess.PIPE, cwd=root)
        (root / (name + '-failure.stderr')).write_bytes(failure.stderr)
        assert failure.returncode == 74, failure
        assert b'output write failed at ' in failure.stderr and location in failure.stderr, failure
        assert b'AFTER_ENTRY' not in failure.stderr and b'EXIT_CALLBACK' not in failure.stderr
        assert failure.stderr.count(b'\n') == 1
        assert b'\r' not in failure.stderr
        domain = b'win32' if os.name == 'nt' else b'errno'
        assert re.search(rb'\[' + domain + rb'=[1-9][0-9]*\]\n$', failure.stderr), failure
    # Close the read side before launch, so pipe closure cannot race the guest.
    read_end, write_end = os.pipe()
    os.close(read_end)
    with os.fdopen(write_end, 'wb', buffering=0) as stream:
        pipe = subprocess.run([str(executables['branch']), 'fail'], stdout=stream,
                              stderr=subprocess.PIPE, cwd=root)
    assert pipe.returncode == 74 and pipe.stderr == b'', pipe
    with read_only.open('rb') as stream:
        flush = subprocess.run([str(executables['branch']), 'fail', 'buffered'], stdout=stream,
                               stderr=subprocess.PIPE, cwd=root)
    (root / 'buffered-failure.stderr').write_bytes(flush.stderr)
    assert flush.returncode == 74 and b'output flush failed at ' in flush.stderr, flush
    assert b'branch.xr:2:1-2:1 ' in flush.stderr and flush.stderr.count(b'\n') == 1, flush
    assert b'AFTER_ENTRY' not in flush.stderr and b'EXIT_CALLBACK' not in flush.stderr
    read_end, write_end = os.pipe()
    os.close(read_end)
    with os.fdopen(write_end, 'wb', buffering=0) as stream:
        buffered_pipe = subprocess.run([str(executables['branch']), 'fail', 'buffered'], stdout=stream,
                                       stderr=subprocess.PIPE, cwd=root)
    assert buffered_pipe.returncode == 74 and buffered_pipe.stderr == b'', buffered_pipe
    identity['result'] = 'PASS'
    identity['cases'] = list(cases)
    identity['write-failure'] = 'exit 74, owned source position, no return or exit callbacks'
    identity['broken-pipe'] = 'exit 74, empty diagnostic'
    identity['buffered-flush'] = 'exit 74 at the real generated call, no post-entry/exit callbacks'
    identity['buffered-broken-pipe'] = 'exit 74, empty diagnostic'
    (root / 'result.json').write_text(json.dumps(identity, indent=2) + '\n', encoding='utf-8')
    print('real generated-C/runtime checks PASS: ' + str(root), flush=True)


if __name__ == '__main__':
    main()
