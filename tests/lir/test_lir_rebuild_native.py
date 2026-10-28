"""Execute copied modules in both backends after retiring every producer."""
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
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--kind', choices=['msvc', 'gnu'], required=True)
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--library', type=Path, nargs='+', required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='lir-rebuild-', dir=args.work))
    # The guest leaf writes LF; scalar-return adapters use the host CRT stream.
    host_lf = b'\r\n' if os.name == 'nt' else b'\n'
    expected = {'fib': b'2178309\n', 'true': b'42' + host_lf, 'false': b'43' + host_lf,
                'loop': b'190' + host_lf, 'unit': b'0' + host_lf}
    for case, answer in expected.items():
        generated = root / (case + '.c')
        harness = root / (case + '-host.c')
        executable = root / (case + ('.exe' if os.name == 'nt' else ''))
        run([args.driver, case, generated], root)
        text = generated.read_text(encoding='ascii')
        assert '({' not in text
        if case != 'fib':
            assert 'xrt_' not in text and 'xray_native_runtime.h' not in text
        result_type = 'void' if case in ('fib', 'unit') else 'int64_t'
        action = 'entry();' if case == 'fib' else 'entry(); puts("0");' if case == 'unit' else 'printf("%" PRId64 "\\n", entry());'
        harness.write_text('#include <stdint.h>\n#include <inttypes.h>\n#include <stdio.h>\n'
            f'#include "{generated.name}"\nint main(void) {{\n'
            f'    {result_type} (*entry)(void) = xray_lir_entry;\n    {action}\n    return 0;\n}}\n', encoding='ascii')
        if args.kind == 'msvc':
            crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                   'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
            command = [args.compiler, '/nologo', '/std:c11', '/W4', '/WX', '/O2', '/utf-8', crt,
                       '/I' + str(args.include), harness, *args.library, '/Fe:' + str(executable)]
            if args.asan:
                command += ['/fsanitize=address', '/Zi']
                if 'clang' in args.compiler.name.lower():
                    command += ['/clang:-fsanitize=undefined']
                command += ['/link', '/INCREMENTAL:NO']
        else:
            command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
                       '-I' + str(args.include), harness, *args.library, '-pthread', '-o', executable]
            if args.asan:
                command += ['-fsanitize=address,undefined', '-g']
        run(command, root)
        native = run([executable], root)
        vm = run([args.driver, case], root)
        assert native.stdout == vm.stdout == answer, (case, native.stdout, vm.stdout, answer)
        assert native.stderr == vm.stderr == b''
    print('Owned reconstruction: five real C/VM consumers passed', flush=True)


if __name__ == '__main__':
    main()
