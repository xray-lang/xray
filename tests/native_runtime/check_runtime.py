"""Exercise real static-library clients; retain bytes and negative link logs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def header_clients(build, logs):
    cache = dict(re.findall(r'^([^#/:\n]+):[^=\n]+=(.*)$',
                           (build / 'CMakeCache.txt').read_text(encoding='utf-8'), re.M))
    graph = subprocess.run([cache['CMAKE_MAKE_PROGRAM'], '-C', build, '-t', 'commands',
                            'test_native_runtime_0'], capture_output=True, check=True, timeout=15)
    recipe = next(line for line in graph.stdout.decode('utf-8').splitlines()
                  if 'MAP_KIND=0' in line and 'client.c' in line)
    source = Path(cache['CMAKE_HOME_DIRECTORY'])
    compiler = cache['CMAKE_C_COMPILER']
    msvc = os.name == 'nt' and Path(compiler).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    libraries = list(build.rglob('xray_native_runtime.lib' if msvc else 'libxray_native_runtime.a'))
    assert len(libraries) == 1, libraries
    library = libraries[0]
    sanitizer = re.findall(r'(?<!\S)[/-](?:clang:-)?fsanitize=[^\s]+', recipe)
    if msvc:
        crt = re.findall(r'(?<!\S)[/-]M[TD]d?(?=\s)', recipe)
        assert len(crt) == 1, recipe
        flags = ['/nologo', '/W4', '/WX', '/O2', '/utf-8', crt[0], *sanitizer]
        link = ['/link', '/INCREMENTAL:NO'] if sanitizer else []
    else:
        flags = ['-Wall', '-Wextra', '-Werror', '-pedantic', '-O2', *sanitizer]
        link = ['-pthread']
    text = ('#include "xray_native_runtime.h"\n'
            '#ifdef XRT_NORETURN\n#error header macro leaked\n#endif\n'
            'const XrtSourceMap xrt_standalone_source_map = {NULL, 0};\n'
            'static void (*const leaf)(uint32_t, uint32_t) = xrt_arithmetic_panic;\n'
            'static void (*const text)(const char *, size_t, uint32_t) = xrt_print_string;\n'
            'static void invoke(void *context) { text("header", 6, 0); leaf(*(uint32_t *)context, 0); }\n'
            'int main(void) { uint32_t code = 421;\n'
            'xrt_run_standalone(&xrt_standalone_source_map, invoke, &code); return 77; }\n')
    records = []
    for language in ('c11', 'c++17'):
        client = logs / ('header-client.' + ('c' if language == 'c11' else 'cpp'))
        client.write_text(text, encoding='ascii')
        executable = logs / ('header-client-' + language.replace('+', 'x') + ('.exe' if msvc else ''))
        if msvc:
            command = [compiler, *flags, '/std:' + language,
                       '/I' + str(source / 'include'), ('/Tc' if language == 'c11' else '/Tp') + str(client),
                       library, '/Fe:' + str(executable), *link]
        else:
            command = [compiler, *flags, '-std=' + language, '-x', 'c' if language == 'c11' else 'c++',
                       '-I' + str(source / 'include'), client, '-x', 'none', library, '-o', executable, *link]
        compiled = subprocess.run(command, cwd=logs, capture_output=True, timeout=30)
        compile_log = logs / (language.replace('+', 'x') + '-compile.log')
        compile_log.write_bytes(compiled.stdout + compiled.stderr)
        assert compiled.returncode == 0, (command, str(compile_log))
        ran = subprocess.run([executable], capture_output=True, timeout=15)
        assert (ran.returncode, ran.stdout, ran.stderr) == (1, b'header\n', b'[Uncaught Panic] E0421: modulo by zero\n')
        records.append({'language': language, 'command': [str(x) for x in command],
                        'actualClientRecipe': recipe,
                        'compileExit': compiled.returncode, 'exit': ran.returncode,
                        'stderrHex': ran.stderr.hex(), 'library': str(library),
                        'librarySHA256': hashlib.sha256(library.read_bytes()).hexdigest(),
                        'headerSHA256': hashlib.sha256((source / 'include/xray_native_runtime.h').read_bytes()).hexdigest()})
    (logs / 'header-clients.json').write_text(json.dumps(records, indent=2), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--clients', nargs='+', required=True)
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)

    def run(label, client=0, mode='write-error', location=1, pipe=False):
        command = [args.clients[client], mode, str(location)]
        if pipe:
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                process = subprocess.run(command, stdout=write_fd, stderr=subprocess.PIPE, timeout=15)
            finally:
                os.close(write_fd)
            output = b''
        else:
            process = subprocess.run(command, capture_output=True, timeout=15)
            output = process.stdout
        (args.logs / (label + '.stdout')).write_bytes(output)
        (args.logs / (label + '.stderr')).write_bytes(process.stderr)
        (args.logs / (label + '.exit')).write_text(str(process.returncode))
        assert b'AFTER_CALL' not in process.stderr and b'EXIT_CALLBACK' not in process.stderr, label
        return process.returncode, output, process.stderr

    if args.suite == 'output':
        code, output, error = run('raw-values', mode='values')
        assert (code, output, error) == (0, b'-9223372036854775808\n0\n9223372036854775807\n', b'')
        assert run('prefix-order', mode='prefix') == (0, b'prefix:12\n', b'')
        assert run('string-bytes', mode='string-values') == (0, b'A\0\xe4\xb8\xad\r\nZ\n\n', b'')
        assert run('string-prefix', mode='string-prefix') == (0, b'prefix:text\n', b'')
    elif args.suite == 'failures':
        for mode, expected in [('panic-div', b'[Uncaught Panic] E0420: division by zero\n'),
                               ('panic-rem', b'[Uncaught Panic] E0421: modulo by zero\n'),
                               ('panic-nested', b'[Uncaught Panic] E0420: division by zero\n')]:
            for location in (0, 1):
                assert run(mode + '-' + str(location), mode=mode, location=location) == (1, b'', expected)
        assert run('panic-empty', 1, mode='panic-div', location=0) == (
            1, b'', b'[Uncaught Panic] E0420: division by zero\n')
        assert run('panic-sink-error', mode='panic-sink-error', location=0) == (1, b'', b'')
        for label, mode, client, location in [('panic-bad-id', 'panic-div', 0, 2),
                                               ('panic-empty-id', 'panic-div', 1, 1),
                                               ('panic-bad-code', 'panic-bad-code', 0, 0),
                                               ('panic-unbound', 'unbound-panic', 0, 0)]:
            code, output, error = run(label, client, mode=mode, location=location)
            assert code not in (0, 1, 74) and not output, (label, code, output, error)
            assert b'Arithmetic panic requires an active execution, valid code and source id' in error
            assert b'[Uncaught Panic]' not in error, (label, error)
        cases = [
            ('location', 0, 1, b'source\\x0a.xrc:3:5-7:9'),
            ('unknown', 0, 0, b'unknown source'),
            ('empty', 1, 0, b'unknown source'),
            ('empty-id', 1, 1, b'invalid source id 1'),
            ('range', 0, 4294967295, b'invalid source id 4294967295'),
            ('start-only', 7, 1, b'start.xrc:3:5 ['),
        ]
        for label, client, location, expected in cases:
            code, output, error = run(label, client, location=location)
            assert code == 74 and not output, (label, code, output, error)
            assert error.startswith(b'xray: output write failed at ') and expected in error, (label, error)
            assert error.count(b'\n') == 1 and b'!' not in error, (label, error)
            assert re.search(rb'\[(?:win32|errno)=[1-9][0-9]*\]\n$', error), (label, error)
        for client in [2, 3, 4, 5, 6, 8, 9]:
            code, output, error = run('invalid-root-' + str(client), client, mode='values')
            assert code != 0 and not output, (client, code, output, error)
            assert b'valid prepared source map' in error, (client, error)
        code, output, error = run('real-flush', mode='flush-error')
        assert code == 74 and not output and b'output flush failed at ' in error, (code, output, error)
        assert re.search(rb'\[(?:win32|errno)=[1-9][0-9]*\]\n$', error), error
        code, output, error = run('stale-codes', mode='stale-flush-error')
        assert code == 74 and not output and b'output flush failed at ' in error, (code, output, error)
        assert re.search(rb'\[(?:win32|errno)=[1-9][0-9]*\]\n$', error), error
        code, output, error = run('real-broken-pipe', mode='pipe', pipe=True)
        assert (code, output, error) == (74, b'', b''), (code, output, error)
        code, output, error = run('real-buffered-broken-pipe', mode='pipe-flush', pipe=True)
        assert (code, output, error) == (74, b'', b''), (code, output, error)
        for mode in ('write-error', 'empty-error', 'flush-error'):
            code, output, error = run('string-' + mode, mode='string-' + mode)
            operation = b'flush' if mode == 'flush-error' else b'write'
            assert code == 74 and not output, (mode, code, output, error)
            assert error.startswith(b'xray: output ' + operation + b' failed at '), error
            assert b'source\\x0a.xrc:3:5-7:9' in error and error.count(b'\n') == 1, error
        for mode in ('pipe', 'pipe-flush', 'empty-pipe'):
            assert run('string-' + mode, mode='string-' + mode, pipe=True) == (74, b'', b'')
        code, output, error = run('string-bad-span', mode='string-bad-span')
        assert code not in (0, 1, 74) and not output, (code, output, error)
        assert b'String output requires bytes for a nonempty span' in error
    elif args.suite == 'link_contract':
        header_clients(Path(args.build).resolve(), args.logs.resolve())
        for name, diagnostic in [('missing', rb'LNK20(?:01|19)|undefined (?:reference|symbol)'),
                                 ('duplicate', rb'LNK2005|multiple definition|duplicate symbol')]:
            result = subprocess.run(['cmake', '--build', args.build, '--target',
                                     'test_native_runtime_' + name, '-j1'],
                                    capture_output=True, timeout=60)
            combined = result.stdout + result.stderr
            (args.logs / (name + '.log')).write_bytes(combined)
            assert result.returncode and b'xrt_standalone_source_map' in combined, combined
            assert re.search(diagnostic, combined, re.IGNORECASE), combined
    else:
        raise AssertionError(args.suite)
    print('PASS:', args.suite)


if __name__ == '__main__':
    main()
