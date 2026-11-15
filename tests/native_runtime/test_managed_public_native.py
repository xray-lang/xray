"""Compile public-only C/C++ clients against fresh C implementation objects."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import tempfile


def run(argv, work):
    print('$ ' + json.dumps([str(item) for item in argv]), flush=True)
    result = subprocess.run([str(item) for item in argv], cwd=work, capture_output=True, timeout=30)
    for output in (result.stdout, result.stderr):
        if output:
            print(output.decode('utf-8', errors='replace'), end='', flush=True)
    if result.returncode:
        raise RuntimeError(f'command exited {result.returncode}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=['msvc', 'gnu'])
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='managed-public-', dir=args.work)).resolve()
    objects = []

    def compile_file(path, name, private=False, cpp=False):
        obj = work / (name + ('.obj' if args.kind == 'msvc' else '.o'))
        includes = [source / 'include'] + ([source / 'src'] if private else [])
        if args.kind == 'msvc':
            flags = ['/nologo', '/W4', '/WX', '/utf-8', '/TP', '/std:c++17'] if cpp else [
                '/nologo', '/W4', '/WX', '/utf-8', '/TC', '/std:c11']
            if not cpp and args.compiler.stem.lower() != 'clang-cl':
                flags.append('/experimental:c11atomics')
            if private:
                flags.extend(['/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_WARNINGS'])
            command = [args.compiler, *flags, *['/I' + str(p) for p in includes], '/c', path, '/Fo' + str(obj)]
        else:
            command = [args.compiler, '-x', 'c++' if cpp else 'c', '-std=c++17' if cpp else '-std=c11',
                       '-Wall', '-Wextra', '-Werror', '-pedantic', '-D_POSIX_C_SOURCE=200809L',
                       *['-I' + str(p) for p in includes], '-c', path, '-o', obj]
        run(command, work)
        return obj

    for name in ['xnative_managed', 'xnative_managed_root', 'xnative_place']:
        objects.append(compile_file(source / 'src/runtime/native' / (name + '.c'), name, private=True))
    for name, cpp in [('managed_public_client', False), ('managed_public_cpp', True)]:
        path = source / 'tests/native_runtime' / (name + ('.cpp' if cpp else '.c'))
        obj = compile_file(path, name, cpp=cpp)
        product = work / (name + ('.exe' if os.name == 'nt' else ''))
        command = [args.compiler, '/nologo', obj, *objects, '/Fe' + str(product)] if args.kind == 'msvc' else [
            args.compiler, obj, *objects, '-o', product]
        run(command, work)
        run([product], work)
    amalg = work / 'managed_amalg.c'
    amalg.write_text('#define xray_amalg_c 1\n' + ''.join(
        '#include ' + json.dumps((source / 'src/runtime/native' / (name + '.c')).as_posix()) + '\n'
        for name in ['xnative_managed', 'xnative_managed_root', 'xnative_place']), encoding='ascii')
    compile_file(amalg, 'managed_amalg', private=True)
    print('public-only C11 and opaque C++ clients plus amalgamated implementation passed', flush=True)


if __name__ == '__main__':
    main()
