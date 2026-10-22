"""Observe integer and opaque pointer carriers across separate native TUs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(command, cwd):
    command = [str(x) for x in command]
    print('$ ' + json.dumps(command), flush=True)
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=90)
    for stream in (result.stdout, result.stderr):
        if stream:
            print(stream.decode('utf-8', errors='replace'), end='', flush=True)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return {'argv': command, 'exit': result.returncode,
            'stdoutHex': result.stdout.hex(), 'stderrHex': result.stderr.hex()}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def adapter(case):
    includes = '#include "carrier.h"\n#include <inttypes.h>\n#include <stdio.h>\n'
    if case == 'u64':
        return includes + ('int main(void) { Root root = root_get(); uint64_t value = root();\n'
                           'printf("%" PRIu64 "\\n", value); return 0; }\n')
    if case.startswith('ptr'):
        return includes + '#include <errno.h>\n#include <stdlib.h>\n' + '''
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    unsigned char left = 0, right = 0;
    void *objects[] = {NULL, &left, &right};
    Carrier call = carrier_get(); Root root = root_get();
    Comparer equal = equal_get(), different = different_get();
    for (int i = 1; i < argc; ++i) {
        char *end = NULL; errno = 0;
        intmax_t selector = strtoimax(argv[i], &end, 10);
        if (errno || end == argv[i] || *end || selector < 0 || selector > 2) return 3;
        void *input = objects[(size_t)selector];
        printf("%d\\n", call(input) == input);
    }
    const unsigned pairs[][2] = {{0,0}, {1,1}, {1,2}, {0,1}, {1,0}};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(*pairs); ++i) {
        void *a = objects[pairs[i][0]], *b = objects[pairs[i][1]];
        printf("%d\\n", 2 * (int)equal(a, b) + (int)different(a, b));
    }
    if (fflush(stdout)) return 4;
    void *result = root();
    printf("%d\\n", result == NULL); return 0;
}
'''
    minimum, maximum = ('INT8_MIN', 'INT8_MAX') if case == 'i8' else ('0', 'UINT16_MAX')
    return includes + '#include <errno.h>\n#include <stdlib.h>\n' + f'''
int main(int argc, char **argv) {{
    if (argc != 4) return 2;
    Carrier call = carrier_get(); Root root = root_get();
    for (int i = 1; i < argc; ++i) {{
        char *end = NULL; errno = 0;
        intmax_t parsed = strtoimax(argv[i], &end, 10);
        if (errno || end == argv[i] || *end || parsed < {minimum} || parsed > {maximum}) return 3;
        Scalar result = call((Scalar)parsed);
        printf("%" PRId64 "\\n", (int64_t)result);
    }}
    printf("%" PRId64 "\\n", root()); return 0;
}}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--kind', choices=['msvc', 'gnu'], required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='lir-carriers-', dir=args.work))
    inputs = {'i8': ['-128', '-1', '127'], 'u16': ['32768', '65535', '0'], 'u64': [],
              'ptr': ['0', '1', '2'], 'ptr-output': ['0', '1', '2']}
    answers = {'i8': [-128, -1, 127, -128], 'u16': [32768, 65535, 0, 65535],
               'u64': [18446744073709551615]}
    newline = b'\r\n' if os.name == 'nt' else b'\n'
    results = []
    for case in inputs:
        for mode in ('before', 'after'):
            pointer = case.startswith('ptr')
            output = case == 'ptr-output'
            work = root / (case + '-' + mode)
            work.mkdir()
            generated, gateway, host = work / 'generated.c', work / 'gateway.c', work / 'adapter.c'
            producer = run([args.driver, case, mode, generated], work)
            assert producer['stderrHex'] == ''
            observation = json.loads(bytes.fromhex(producer['stdoutHex']))
            assert observation['carrier'] == ('ptr' if pointer else case)
            if case != 'u64':
                a, b, _ = observation['parameters']
                assert observation['backedgeArguments'][:2] == [b, a]
                assert observation['returned'] == b and observation['iterations'] == 1
            text = generated.read_text(encoding='ascii')
            assert '({' not in text
            if output:
                assert '#include "xray_native_runtime.h"' in text and 'xrt_run_standalone' in text
            else:
                assert 'xrt_' not in text and 'xray_native_runtime.h' not in text
            scalar = 'void *' if pointer else {'i8': 'int8_t', 'u16': 'uint16_t', 'u64': 'uint64_t'}[case]
            result_type = 'void *' if pointer else 'uint64_t' if case == 'u64' else 'int64_t'
            header = '#ifndef CARRIER_H\n#define CARRIER_H\n#include <stdint.h>\n'
            header += f'typedef {result_type} (*Root)(void);\nRoot root_get(void);\n'
            helpers = ''
            if case != 'u64':
                header += f'typedef {scalar} Scalar;\ntypedef Scalar (*Carrier)(Scalar);\nCarrier carrier_get(void);\n'
                helpers = ('_Static_assert(_Generic(&xrl_f2, Carrier: 1, default: 0), "helper type");\n'
                           'Carrier carrier_get(void) { return &xrl_f2; }\n')
                if pointer:
                    header += ('typedef _Bool (*Comparer)(Scalar, Scalar);\n'
                               'Comparer equal_get(void);\nComparer different_get(void);\n')
                    helpers += ('_Static_assert(_Generic(&xrl_f3, Comparer: 1, default: 0), "EQ type");\n'
                                '_Static_assert(_Generic(&xrl_f4, Comparer: 1, default: 0), "NE type");\n'
                                'Comparer equal_get(void) { return &xrl_f3; }\n'
                                'Comparer different_get(void) { return &xrl_f4; }\n')
            (work / 'carrier.h').write_text(header + '#endif\n', encoding='ascii')
            gateway.write_text('#include "generated.c"\n#include "carrier.h"\n'
                '_Static_assert(_Generic(&xray_lir_entry, Root: 1, default: 0), "root type");\n'
                'Root root_get(void) { return &xray_lir_entry; }\n' + helpers, encoding='ascii')
            host.write_text(adapter(case), encoding='ascii')
            executable = work / ('run.exe' if os.name == 'nt' else 'run')
            objects = [work / ('gateway.obj' if args.kind == 'msvc' else 'gateway.o'),
                       work / ('adapter.obj' if args.kind == 'msvc' else 'adapter.o')]
            if args.kind == 'msvc':
                crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                       'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
                flags = ['/nologo', '/std:c11', '/W4', '/WX', '/O2', '/utf-8', crt]
                if output:
                    flags += ['/I' + str(args.include)]
                flags += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
                if args.asan:
                    flags += ['/fsanitize=address', '/Zi']
                    if 'clang' in args.compiler.name.lower():
                        flags += ['/clang:-fsanitize=undefined']
                compiles = [[args.compiler, *flags, '/c', source, '/Fo' + str(obj)]
                            for source, obj in zip((gateway, host), objects)]
                link = [args.compiler, *flags, *objects]
                if output:
                    link += [args.library]
                link += ['/Fe:' + str(executable), '/link', '/INCREMENTAL:NO']
            else:
                flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2', '-fno-lto']
                if output:
                    flags += ['-I' + str(args.include)]
                if args.asan:
                    flags += ['-fsanitize=address,undefined', '-g']
                compiles = [[args.compiler, *flags, '-c', source, '-o', obj]
                            for source, obj in zip((gateway, host), objects)]
                link = [args.compiler, *flags, *objects]
                if output:
                    link += [args.library, '-pthread']
                link += ['-o', executable]
            compiled = [run(command, work) for command in compiles]
            linked = run(link, work)
            native = run([executable, *inputs[case]], work)
            if pointer:
                expected = b''.join(str(value).encode('ascii') + newline for value in [1, 1, 1, 2, 2, 1, 1, 1])
                if output:
                    expected += b'42\n'
                expected += b'1' + newline
            else:
                expected = b''.join(str(value).encode('ascii') + newline for value in answers[case])
            assert native['stdoutHex'] == expected.hex() and native['stderrHex'] == ''
            runtime = None
            if output:
                runtime = {'library': str(args.library), 'librarySHA256': digest(args.library),
                           'header': str(args.include / 'xray_native_runtime.h'),
                           'headerSHA256': digest(args.include / 'xray_native_runtime.h'), 'msvcRuntime': args.msvc_runtime}
            results.append({'case': case, 'mode': mode, 'expectedHex': expected.hex(), 'producer': producer,
                'observation': observation, 'generatedSHA256': digest(generated), 'compiles': compiled,
                'objects': [{'path': str(obj), 'sha256': digest(obj)} for obj in objects],
                'link': linked, 'native': native, 'runtime': runtime})
            (root / 'case-results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('Integer and pointer carriers before/after SCCP passed across separate native TUs', flush=True)


if __name__ == '__main__':
    main()
