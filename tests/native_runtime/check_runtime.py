"""Exercise real static-library clients; retain bytes and negative link logs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.toolchain import archive_link_arguments, install_native_runtime, native_c11_options


def header_clients(build, logs, libraries):
    build, logs = build.resolve(), logs.resolve()
    cache = dict(re.findall(r'^([^#/:\n]+):[^=\n]+=(.*)$',
                           (build / 'CMakeCache.txt').read_text(encoding='utf-8'), re.M))
    graph = subprocess.run([cache['CMAKE_MAKE_PROGRAM'], '-C', build, '-t', 'commands',
                            'test_native_runtime_0'], capture_output=True, check=True, timeout=15)
    recipe = next(line for line in graph.stdout.decode('utf-8').splitlines()
                  if 'MAP_KIND=0' in line and 'client.c' in line)
    compiler = cache['CMAKE_C_COMPILER']
    msvc = os.name == 'nt' and Path(compiler).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    system_libraries = []
    if os.name == 'nt':
        assert tuple(str(p) for p in libraries[-2:]) == ('advapi32.lib', 'userenv.lib'), libraries
        system_libraries = [str(p) for p in libraries[-2:]]
        libraries = libraries[:-2]
    assert len(libraries) == 1 and all(p.is_absolute() and p.is_file() and p.name in ("xray_native_runtime.lib", "libxray_native_runtime.a") for p in libraries), libraries
    prefix = Path(tempfile.mkdtemp(prefix='native install with spaces-', dir=logs))
    runtime = install_native_runtime(build, prefix, logs)
    original = libraries[0]
    installed = runtime / original.name
    assert installed.is_file() and hashlib.sha256(installed.read_bytes()).digest() == hashlib.sha256(original.read_bytes()).digest(), installed
    moved = prefix.with_name(prefix.name + '-moved'); prefix.rename(moved)
    runtime = moved / runtime.relative_to(prefix)
    libraries = [runtime / original.name]
    include = runtime / 'include'
    sanitizer = re.findall(r'(?<!\S)[/-](?:clang:-)?fsanitize=[^\s]+', recipe)
    if msvc:
        crt = re.findall(r'(?<!\S)[/-]M[TD]d?(?=\s)', recipe)
        assert len(crt) == 1, recipe
        flags = ['/nologo', '/W4', '/WX', '/O2', '/utf-8', crt[0], *sanitizer]
        link = ['/link', '/INCREMENTAL:NO'] if sanitizer else []
    else:
        flags = ['-Wall', '-Wextra', '-Werror', '-pedantic', '-O2', *sanitizer]
        link = [
            ('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'),
            '-pthread']
    witness = logs / 'header-values.c'
    witness.write_text('''#include "xray_native_runtime.h"
#include "xray_native_values.h"
#include "xray_native_map.h"
#include "xray_native_place.h"
#include <string.h>
#define REQUIRE(call) do { if ((call) != XRT_MANAGED_OK) return 0; } while (0)
int xray_header_values(void) {
    XrtManagedRoot *root = xrt_native_managed_root(0);
    XrtManagedFrame *frame = NULL;
    REQUIRE(xrt_managed_frame_enter(root, 8, &frame));
    xrt_native_string_create(frame, 0, "head", 4, 0);
    xrt_native_string_i64_create(frame, 1, 7, 0);
    XrtManagedHeader *head = NULL, *number = NULL, *joined = NULL, *empty = NULL;
    REQUIRE(xrt_managed_slot_borrow(frame, 0, &head));
    REQUIRE(xrt_managed_slot_borrow(frame, 1, &number));
    xrt_native_string_concat_create(frame, 2, head, number, 0);
    XrtPlace source = {frame, 2, NULL, XRT_PLACE_NONNULL};
    XrtPlace copied = {frame, 3, NULL, XRT_PLACE_NULLABLE};
    XrtPlace moved = {frame, 4, NULL, XRT_PLACE_NULLABLE};
    REQUIRE(xrt_place_copy_replace(copied, source, NULL));
    REQUIRE(xrt_place_move_replace(moved, copied, NULL));
    REQUIRE(xrt_managed_slot_borrow(frame, 3, &empty));
    REQUIRE(xrt_managed_slot_borrow(frame, 4, &joined));
    const XrtStringBacking *text = (const XrtStringBacking *)joined;
    if (empty || !text || text->length != 5 || memcmp(text->bytes, "head7", 5)) return 0;
    REQUIRE(xrt_managed_slot_drop(frame, 4));
    REQUIRE(xrt_managed_slot_drop(frame, 3));
    const XrtMapInput input = {NULL, NULL, 0};
    xrt_native_map_create(frame, 5, &input, 0);
    xrt_native_string_create(frame, 6, "key", 3, 0);
    XrtManagedHeader *key = NULL, *map = NULL;
    REQUIRE(xrt_managed_slot_borrow(frame, 6, &key));
    const XrtStringBacking *key_value = (const XrtStringBacking *)key;
    XrtPlace binding = {frame, 5, NULL, XRT_PLACE_NONNULL};
    const XrtMapUpdate update = {XRT_MAP_UPDATE_SET, key_value, 42};
    bool removed = false;
    xrt_native_map_update_place(binding, frame, 7, &update, &removed, 0);
    REQUIRE(xrt_managed_slot_borrow(frame, 5, &map));
    int64_t value = 0;
    if (removed || ((const XrtMapBacking *)map)->length != 1 ||
        !xrt_native_map_get(map, key_value, &value, 0) || value != 42 ||
        xrt_native_map_index(map, key_value, 0) != 42) return 0;
    REQUIRE(xrt_managed_frame_leave(root, frame));
    return 1;
}
''', encoding='ascii')
    object_file = logs / ('header-values.obj' if msvc else 'header-values.o')
    if msvc:
        witness_command = [compiler, *flags, *native_c11_options(compiler, driver='msvc'),
                           '/I' + str(include), '/c', '/Tc' + str(witness), '/Fo' + str(object_file)]
    else:
        witness_command = [compiler, *flags, *native_c11_options(compiler, driver='gnu'),
                           '-I' + str(include), '-c', witness, '-o', object_file]
    compiled = subprocess.run(witness_command, cwd=logs, capture_output=True, timeout=30)
    (logs / 'header-values.compile.json').write_text(json.dumps([str(x) for x in witness_command], indent=2))
    (logs / 'header-values.compile.log').write_bytes(compiled.stdout + compiled.stderr)
    assert compiled.returncode == 0, (witness_command, compiled.stdout, compiled.stderr)
    text = ('#include "xray_native_runtime.h"\n'
            '#include "xray_native_values.h"\n'
            '#include "xray_native_array.h"\n'
            '#include "xray_native_map.h"\n'
            '#include "xray_native_state.h"\n'
            '#ifdef XRT_NORETURN\n#error header macro leaked\n#endif\n'
            '#ifdef __cplusplus\nextern "C" int xray_header_values(void);\n'
            '#else\nint xray_header_values(void);\n#endif\n'
            'const XrtSourceMap xrt_standalone_source_map = {NULL, 0};\n'
            'static void (*const leaf)(uint32_t, uint32_t) = xrt_arithmetic_panic;\n'
            'static void (*const text)(const char *, size_t, uint32_t) = xrt_print_string;\n'
            'static int64_t (*const realtime)(uint32_t) = xrt_clock_realtime_nanos;\n'
            'static int64_t (*const cpu)(uint32_t) = xrt_clock_process_cpu_nanos;\n'
            'static int64_t (*const monotonic)(uint32_t) = xrt_clock_monotonic_nanos;\n'
            'static int64_t (*const offset)(int64_t, uint32_t) = xrt_clock_utc_offset_at;\n'
            'static void invoke(void *context) { int64_t a = monotonic(0);\n'
            ' if (a <= 0 || realtime(0) <= 0 || cpu(0) < 0 || monotonic(0) < a ||\n'
            ' offset(INT64_C(1700000000), 0) != offset(INT64_C(1700000000), 0)) return;\n'
            ' if (!xray_header_values()) return;\n'
            ' text("header", 6, 0); leaf(*(uint32_t *)context, 0); }\n'
            'int main(void) { uint32_t code = 421;\n'
            'xrt_run_standalone(&xrt_standalone_source_map, invoke, &code); return 77; }\n')
    records = []
    for language in ('c11', 'c++17'):
        client = logs / ('header-client.' + ('c' if language == 'c11' else 'cpp'))
        client.write_text(text, encoding='ascii')
        executable = logs / ('header-client-' + language.replace('+', 'x') + ('.exe' if msvc else ''))
        if msvc:
            mode = native_c11_options(compiler, driver='msvc') if language == 'c11' else ['/std:c++17']
            command = [compiler, *flags, *mode,
                       '/I' + str(include), ('/Tc' if language == 'c11' else '/Tp') + str(client),
                       object_file,
                       *archive_link_arguments(libraries, driver="msvc" if msvc else "gnu"), *system_libraries, '/Fe:' + str(executable), *link]
        else:
            # The C++ driver supplies its ABI and sanitizer runtime at link time.
            client_compiler = compiler if language == 'c11' else cache['CMAKE_CXX_COMPILER']
            command = [client_compiler, *flags, '-std=' + language, '-x', 'c' if language == 'c11' else 'c++',
                       '-I' + str(include), client, '-x', 'none', object_file, *archive_link_arguments(libraries, driver="msvc" if msvc else "gnu"), *system_libraries, '-o', executable, *link]
        compiled = subprocess.run(command, cwd=logs, capture_output=True, timeout=30)
        compile_log = logs / (language.replace('+', 'x') + '-compile.log')
        compile_log.write_bytes(compiled.stdout + compiled.stderr)
        assert compiled.returncode == 0, (command, str(compile_log))
        ran = subprocess.run([executable], capture_output=True, timeout=15)
        label = language.replace('+', 'x')
        (logs / (label + '.stdout')).write_bytes(ran.stdout)
        (logs / (label + '.stderr')).write_bytes(ran.stderr)
        (logs / (label + '.exit')).write_text(str(ran.returncode), encoding='ascii')
        assert (ran.returncode, ran.stdout, ran.stderr) == (1, b'header\n', b'[Uncaught Panic] E0421: modulo by zero\n')
        records.append({'language': language, 'command': [str(x) for x in command],
                        'actualClientRecipe': recipe,
                        'compileExit': compiled.returncode, 'exit': ran.returncode,
                        'stdoutHex': ran.stdout.hex(),
                        'stderrHex': ran.stderr.hex(),
                        'libraries': [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in libraries],
                        'systemLibraries': system_libraries,
                        'headerSHA256': hashlib.sha256((include / 'xray_native_runtime.h').read_bytes()).hexdigest(),
                        'witnessSHA256': hashlib.sha256(witness.read_bytes()).hexdigest(),
                        'witnessObjectSHA256': hashlib.sha256(object_file.read_bytes()).hexdigest()})
    (logs / 'header-clients.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--library', type=Path, nargs='+', required=True)
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

    if args.suite == 'clock':
        for mode in ('failed', 'range', 'unknown', 'bad-source',
                     'offset-range', 'offset-unknown', 'offset-bad-source'):
            command = [args.clients[0], mode]
            process = subprocess.run(command, capture_output=True, timeout=15)
            (args.logs / (mode + '.stdout')).write_bytes(process.stdout)
            (args.logs / (mode + '.stderr')).write_bytes(process.stderr)
            (args.logs / (mode + '.exit')).write_text(str(process.returncode))
            assert process.returncode != 0 and not process.stdout
            assert b'AFTER_CALL' not in process.stderr and b'EXIT_CALLBACK' not in process.stderr
            if mode not in ('bad-source', 'offset-bad-source'):
                offset = mode.startswith('offset-')
                reason = b'out of range' if offset or mode == 'range' else b'read failed'
                source = b'unknown source' if mode in ('unknown', 'offset-unknown') else b'clock.xr:7:9-7:12'
                kind = b'utc-offset' if offset else b'monotonic'
                assert process.returncode == 1
                assert process.stderr == b'xray: host clock ' + kind + b' ' + reason + b' at ' + source + b'\n'
        # The original complete Source call discards the result; all four
        # optimization orders must retain its execution-ending range failure.
        clients = header_clients(Path(args.build), args.logs, args.library)
        template = next(row['command'] for row in clients if row['language'] == 'c11')
        for mode in range(4):
            generated = args.logs / ('offset-range-' + str(mode) + '.c')
            emitted = subprocess.run([args.clients[0], 'emit-offset', str(generated), str(mode)],
                                     capture_output=True, timeout=30)
            assert (emitted.returncode, emitted.stdout, emitted.stderr) == (0, b'', b''), emitted.stderr
            adapter = generated.with_name(generated.stem + '-client.c')
            adapter.write_text('#include <stdio.h>\n#include <stdlib.h>\n#include "' + generated.name +
                '"\nstatic void late(void){fputs("EXIT_CALLBACK",stderr); }\n'
                'int main(void){if(atexit(late))return 2;(void)xray_lir_entry();'
                'fputs("AFTER_CALL",stderr);return 77;}\n', encoding='ascii')
            exe = adapter.with_suffix('.exe' if os.name == 'nt' else '')
            command = [('/Tc' + str(adapter) if x.startswith('/Tc') else
                        '/Fe:' + str(exe) if x.startswith('/Fe:') else
                        str(adapter) if x.endswith('header-client.c') else x) for x in template]
            if os.name != 'nt':
                command[command.index('-o') + 1] = str(exe)
            (args.logs / (generated.stem + '.compile.json')).write_text(json.dumps(command, indent=2))
            compiled = subprocess.run(command, cwd=args.logs, capture_output=True, timeout=30)
            (args.logs / (generated.stem + '.compile.log')).write_bytes(compiled.stdout + compiled.stderr)
            assert compiled.returncode == 0, compiled.stderr
            ran = subprocess.run([exe], capture_output=True, timeout=15)
            (args.logs / (generated.stem + '.stdout')).write_bytes(ran.stdout)
            (args.logs / (generated.stem + '.stderr')).write_bytes(ran.stderr)
            (args.logs / (generated.stem + '.exit')).write_text(str(ran.returncode))
            assert ran.returncode == 1 and not ran.stdout
            assert ran.stderr == (b'xray: host clock utc-offset out of range at '
                b'<embedded stdlib>/time/time.xr:45:5-45:5\n'), ran.stderr
        return
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
        typed_guard = b'Typed panic requires an active execution, admitted code and source id'
        arithmetic_guard = b'arithmetic operations use their admitted panic codes'
        for label, mode, client, location, guard in [
                ('panic-bad-id', 'panic-div', 0, 2, typed_guard),
                ('panic-empty-id', 'panic-div', 1, 1, typed_guard),
                ('panic-bad-code', 'panic-bad-code', 0, 0, arithmetic_guard),
                ('panic-unbound', 'unbound-panic', 0, 0, typed_guard)]:
            code, output, error = run(label, client, mode=mode, location=location)
            assert code not in (0, 1, 74) and not output, (label, code, output, error)
            assert guard in error, (label, error)
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
        header_clients(Path(args.build).resolve(), args.logs.resolve(), args.library)
        for name, diagnostic in [('missing', rb'LNK20(?:01|19)|undefined (?:reference|symbol)'),
                                 ('duplicate', rb'LNK2005|multiple definition|duplicate symbol')]:
            result = subprocess.run(['cmake', '--build', args.build, '--target',
                                     'test_native_runtime_' + name, '-j1'],
                                    capture_output=True, timeout=30)
            combined = result.stdout + result.stderr
            (args.logs / (name + '.log')).write_bytes(combined)
            assert result.returncode and b'xrt_standalone_source_map' in combined, combined
            assert re.search(diagnostic, combined, re.IGNORECASE), combined
    else:
        raise AssertionError(args.suite)
    print('PASS:', args.suite)


if __name__ == '__main__':
    main()
