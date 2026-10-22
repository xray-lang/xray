"""Compare both real Source backends against fixed observable output bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--vm-driver', type=Path, required=True)
    parser.add_argument('--c-driver', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='skeleton-', dir=args.work)).resolve()
    identity = {'runtime': str(args.library),
                'runtimeSHA256': hashlib.sha256(args.library.read_bytes()).hexdigest(),
                'headerSHA256': hashlib.sha256((args.include / 'xray_native_runtime.h').read_bytes()).hexdigest(),
                'compiler': str(args.compiler), 'kind': args.kind, 'msvcRuntime': args.msvc_runtime,
                'asan': args.asan}
    (root / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')

    def invoke(folder, label, command):
        command = [str(item) for item in command]
        (folder / (label + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        try:
            result = subprocess.run(command, cwd=folder, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired as failure:
            (folder / (label + '.stdout')).write_bytes(failure.stdout or b'')
            (folder / (label + '.stderr')).write_bytes(failure.stderr or b'')
            raise
        (folder / (label + '.stdout')).write_bytes(result.stdout)
        (folder / (label + '.stderr')).write_bytes(result.stderr)
        (folder / (label + '.exit')).write_text(str(result.returncode))
        if result.returncode:
            raise AssertionError(f'{label} exited {result.returncode}; see {folder}')
        return result

    cases = [
        ('fib', None, b'55\n'),
        ('sum', None, b'0\n0\n190\n'),
        ('swap', 'fn swap(n:i64)->i64{var a=1\nvar b=2\nvar i=0\n'
         'while(i<n){const t=a\na=b\nb=t\ni=i+1}\nreturn a*10+b}\n'
         'print(swap(0))\nprint(swap(1))\nprint(swap(4))\n', b'12\n21\n12\n'),
        ('calls', 'fn zero()->i64{return 7}\nfn two(a:i64,b:i64)->i64{return a*10+b}\n'
         'fn work(n:i64)->i64{return two(n,n+1)+zero()}\nprint(work(2))\nprint(work(4))\n', b'30\n52\n'),
        ('ordinary-main', 'fn main()->i64{print(99)\nreturn 7}\nprint(42)\n', b'42\n'),
        ('branch', 'fn choose(flag:bool)->i64{return flag ? 3 : 7}\n'
         'print(choose(true))\nprint(choose(false))\n', b'3\n7\n'),
        ('extremes', 'print(-9223372036854775807-1)\nprint(0)\nprint(9223372036854775807)\n',
         b'-9223372036854775808\n0\n9223372036854775807\n'),
    ]
    for name, text, expected in cases:
        if not expected:
            raise AssertionError('Skeleton cases require nonempty expected output')
        folder = root / name
        folder.mkdir()
        if text is None:
            source = (args.fixtures / ('source-' + name + '.xr')).resolve()
        else:
            source = folder / (name + '.xr')
            source.write_text(text, encoding='utf-8', newline='\n')
        (folder / 'expected.stdout').write_bytes(expected)
        vm = invoke(folder, 'vm', [args.vm_driver, source])
        if vm.stdout != expected or vm.stderr:
            raise AssertionError(f'VM output differs from expected bytes; see {folder}')
        generated = folder / 'generated.c'
        invoke(folder, 'emit-c', [args.c_driver, source, generated])
        adapter = folder / 'adapter.c'
        # The synthetic Checked program entry returns i64. Include its actual
        # definition to check that C type; its value is not a process exit code.
        adapter.write_text('#include "generated.c"\nint main(void) {\n'
                           '    int64_t (*entry)(void) = xray_lir_entry;\n'
                           '    (void)entry();\n    return 0;\n}\n', encoding='ascii')
        executable = folder / ('native.exe' if args.kind == 'msvc' else 'native')
        if args.kind == 'msvc':
            crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                   'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
            command = [args.compiler, '/nologo', '/std:c11', '/W4', '/WX', '/O2', '/utf-8', crt,
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
        invoke(folder, 'compile-c', command)
        native = invoke(folder, 'native', [executable])
        if native.stdout != expected or native.stderr:
            raise AssertionError(f'Native output differs from expected bytes; see {folder}')
        if vm.stdout != native.stdout:
            raise AssertionError(f'Backend output differs; see {folder}')
    print(json.dumps({'passed': len(cases), 'cases': [case[0] for case in cases],
                      'logs': str(root), 'identity': identity}))


if __name__ == '__main__':
    main()
