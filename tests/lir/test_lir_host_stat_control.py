"""Compare real hosted io.fileSize execution through RBC and emitted C."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_lir_pipeline_eval import compile_command


def invoke(work, command, expected=0, *, env=None):
    process = subprocess.run(command, cwd=work, env=env, capture_output=True, timeout=30)
    assert process.returncode == expected, (command, process)
    return process.stdout, process.stderr


def compile_native(args, tests, work, source, stdlib):
    generated = work / 'generated.c'
    env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR='1')
    invoke(work, [args.binary, 'build', '--c-only', source, '-o', generated], env=env)
    native = work / ('host-native.exe' if os.name == 'nt' else 'host-native')
    command = compile_command(args, tests / 'lir_host_stat_control.c', native)
    # Match the runtime's UBSan settings, including UBSan-only builds.
    if args.kind == 'msvc':
        options = ['/DXR_TEST_NATIVE_STAT', '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(work)]
        if args.ubsan:
            if not args.asan:
                options.append('/clang:-fsanitize=undefined')
            options.append('/clang:-fno-sanitize=function')
        if args.ubsan_runtime:
            command.append(args.ubsan_runtime)
        if args.lld_no_tail_merge:
            command.append('/OPT:NOLLDTAILMERGE')
    else:
        options = ['-DXR_TEST_NATIVE_STAT', '-I' + str(work)]
        if args.ubsan:
            if not args.asan:
                options.append('-fsanitize=undefined')
            options.append('-fno-sanitize=function')
    command[1:1] = options
    invoke(work, command)

    return native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'driver', 'compiler', 'include', 'source-root', 'work'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=('msvc', 'gnu'))
    parser.add_argument('--library', required=True, nargs='+')
    parser.add_argument('--msvc-runtime', default='MultiThreadedDLL')
    parser.add_argument('--asan', action='store_true')
    parser.add_argument('--ubsan', action='store_true')
    parser.add_argument('--ubsan-runtime', type=Path)
    parser.add_argument('--lld-no-tail-merge', action='store_true')
    args = parser.parse_args()

    tests = Path(__file__).resolve().parent
    source = tests / 'programs' / 'host-stat-control.xr'
    stdlib = args.source_root.resolve() / 'stdlib'
    args.work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='stat-', dir=args.work) as directory:
        work = Path(directory).resolve()
        (work / 'file.txt').write_bytes(b'measure')
        native = compile_native(args, tests, work, source, stdlib)

        for driver in (args.driver, native):
            ordinary = invoke(work, [driver, source, stdlib, 'run'])
            assert ordinary == (b'11\n7\n22\n', b''), (driver, ordinary)
            cancelled = invoke(work, [driver, source, stdlib, 'cancel'], expected=1)
            assert cancelled == (b'11\n', b'[Uncaught Cancellation] at unknown source\n'), (driver, cancelled)
        print('RBC and emitted C agree on file size and cancellation before class publication')

    array_cases = (
        ('managed-array-cleanup.xr', b'7\n0\n71\n22\n', b'7\n71\n', b'6:9-6:34'),
        ('managed-inline-array-cleanup.xr', b'7\n69\n0\n23\n77\n22\n',
         b'7\n69\n23\n77\n', b'19:11-19:37'),
    )
    for name, expected_normal, expected_failure, location in array_cases:
        array_source = tests / 'programs' / name
        with tempfile.TemporaryDirectory(prefix='array-cleanup-', dir=args.work) as directory:
            work = Path(directory).resolve()
            (work / 'file.txt').write_bytes(b'measure')
            native = compile_native(args, tests, work, array_source, stdlib)
            failures = []
            for driver in (args.driver, native):
                ordinary = invoke(work, [driver, array_source, stdlib, 'run'])
                assert ordinary == (expected_normal, b''), (name, driver, ordinary)
                failures.append(invoke(work, [driver, array_source, stdlib, 'fail-allocation'], expected=1))
            assert failures[0] == failures[1], (name, failures)
            output, diagnostic = failures[0]
            assert output == expected_failure, (name, failures)
            assert diagnostic.startswith(b'XR_RUN_6002: value operation rejected (status 3) at '), (name, failures)
            assert diagnostic.endswith(name.encode() + b':' + location + b'\n'), (name, failures)
        print(f'RBC and emitted C agree on managed allocation cleanup for {name}')


if __name__ == '__main__':
    main()
