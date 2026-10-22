"""Compare fixed outputs before and after propagation in real C and VM."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(command, cwd):
    command = [str(x) for x in command]
    print('$ ' + json.dumps(command), flush=True)
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=90)
    if result.stdout:
        print(result.stdout.decode('utf-8', errors='replace'), end='', flush=True)
    if result.stderr:
        print(result.stderr.decode('utf-8', errors='replace'), end='', flush=True)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return {'argv': command, 'exit': result.returncode,
            'stdoutHex': result.stdout.hex(), 'stderrHex': result.stderr.hex()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--kind', choices=['msvc', 'gnu'], required=True)
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='lir-sccp-', dir=args.work))
    host_lf = b'\r\n' if os.name == 'nt' else b'\n'
    expected = {name: str(value).encode('ascii') + host_lf for name, value in
                [('fold', 42), ('true', 42), ('false', 43), ('unknown-true', 42),
                 ('unknown-false', 43), ('loop', 190), ('wrap', 9223372036854775808)]}
    expected['effects'] = b'1\n2\n3\n'
    results = []
    for case, answer in expected.items():
        for mode in ('before', 'after'):
            name = case + '-' + mode
            generated, harness = root / (name + '.c'), root / (name + '-host.c')
            executable = root / (name + ('.exe' if os.name == 'nt' else ''))
            run([args.driver, case, mode, generated], root)
            text = generated.read_text(encoding='ascii')
            assert '({' not in text
            if case != 'effects':
                assert 'xrt_' not in text and 'xray_native_runtime.h' not in text
            action = '(void)entry();' if case == 'effects' else 'printf("%" PRIu64 "\\n", (uint64_t)entry());'
            harness.write_text('#include <stdint.h>\n#include <inttypes.h>\n#include <stdio.h>\n'
                f'#include "{generated.name}"\n'
                '_Static_assert(_Generic(&xray_lir_entry, int64_t (*)(void): 1, default: 0), "entry type");\n'
                f'int main(void) {{ int64_t (*entry)(void) = xray_lir_entry; {action} return 0; }}\n', encoding='ascii')
            if args.kind == 'msvc':
                crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                       'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
                command = [args.compiler, '/nologo', '/std:c11', '/W4', '/WX', '/O2', '/utf-8', crt,
                           '/I' + str(args.include), harness, args.library, '/Fe:' + str(executable)]
                if args.asan:
                    command += ['/fsanitize=address', '/Zi']
                    if 'clang' in args.compiler.name.lower():
                        command += ['/clang:-fsanitize=undefined']
                    command += ['/link', '/INCREMENTAL:NO']
            else:
                command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
                           '-I' + str(args.include), harness, args.library, '-pthread', '-o', executable]
                if args.asan:
                    command += ['-fsanitize=address,undefined', '-g']
            compiled = run(command, root)
            native = run([executable], root)
            vm = run([args.driver, case, mode], root)
            for result in (native, vm):
                assert result['stdoutHex'] == answer.hex() and result['stderrHex'] == ''
            assert native['stdoutHex'] == vm['stdoutHex']
            results.append({'case': case, 'mode': mode, 'expectedHex': answer.hex(),
                            'generated': str(generated), 'compile': compiled, 'native': native, 'vm': vm})
            (root / 'case-results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('Integer propagation: eight cases before/after in real C/VM passed', flush=True)


if __name__ == '__main__':
    main()
