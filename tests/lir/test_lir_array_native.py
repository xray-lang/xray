"""Compile and execute real Array C emitted after its LIR owner retires."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.toolchain import archive_link_arguments


def run(command, cwd, expected_exit=0):
    command = [str(value) for value in command]
    print('$ ' + json.dumps(command), flush=True)
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=30)
    if result.returncode != expected_exit:
        raise RuntimeError((command, result.returncode, result.stdout, result.stderr))
    return result


def adapter(name, generated):
    prefix = '#include <inttypes.h>\n#include <stdio.h>\n#include <string.h>\n' + f'#include "{generated.name}"\n'
    if not name.startswith('return-'):
        return prefix + 'int main(void) { printf("%" PRId64 "\\n", xray_lir_entry()); return 0; }\n'
    string = name.endswith('string')
    observe = ('XrtString *s = xrt_array_string_get(p->array, 0, 0);\n'
               '    p->value = xrt_string_length(s, 0); xrt_string_drop(&s);\n' if string else
               'p->value = xrt_array_i64_get(p->array, 0, 0);\n')
    if name.endswith('u8'):
        observe = 'p->value = xrt_array_u8_get(p->array, 0, 0);\n'
    if name.endswith('u64'):
        observe = 'uint64_t bits = xrt_array_u64_get(p->array, 0, 0); memcpy(&p->value, &bits, sizeof(bits));\n'
    return prefix + '''typedef struct Probe { XrtArray *array; int64_t value; } Probe;
static void probe(void *context) {
    Probe *p = context;
    ''' + observe + '''    p->value += xrt_array_length(p->array, 0);
}
int main(void) {
    Probe p = {xray_lir_entry(), 0};
    XrtSourceMap map = {NULL, 0};
    xrt_run_standalone(&map, probe, &p);
    printf("%" PRId64 "\\n", p.value);
    xrt_array_owned_drop(&p.array);
    return 0;
}
'''


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
    work = Path(tempfile.mkdtemp(prefix='array-native-', dir=args.work))
    expected = {'i64': 1202, 'string': 511, 'large': 1500, 'state-i64': 1202,
                'state-string': 511, 'state-layout': 9, 'return-i64': 13, 'return-string': 7,
                'array-spills': 44850, 'array-spills-string': 1500,
                'phi-i64': 1202, 'phi-string': 511,
                'state-uninitialized-read': None, 'state-uninitialized-write': None,
                'repeat-i64': 19, 'repeat-string': 10, 'repeat-empty-i64': 0, 'repeat-empty-string': 0,
                'repeat-negative-i64': None, 'repeat-negative-string': None,
                'u8': 264, 'u64': 8, 'phi-u8': 262, 'phi-u64': 6, 'state-u8': 264, 'state-u64': 8,
                'return-u8': 258, 'return-u64': 2, 'unsigned-spills-u8': 33586, 'unsigned-spills-u64': -45150}
    record = {'compiler': str(args.compiler), 'cases': {}, 'libraries': {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in args.library if p.is_file()}}
    for name, answer in expected.items():
        generated, client = work / (name + '.c'), work / (name + '-client.c')
        binary = work / (name + ('.exe' if os.name == 'nt' else ''))
        run([args.driver, name, generated], work)
        text = generated.read_text(encoding='ascii')
        assert '({' not in text
        if name == 'large':
            assert 'UINT32_C(300)' in text and '(const int64_t[]){' in text
        if name == 'state-layout':
            assert 'XRT_MODULE_ARRAY_I64' in text and 'XRT_MODULE_ARRAY_STRING' in text
        client.write_text(adapter(name, generated), encoding='ascii')
        if args.kind == 'msvc':
            crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                   'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
            command = [args.compiler, '/nologo', '/std:c11', '/utf-8', '/O2', '/W4', '/WX', crt,
                       '/I' + str(args.include), client, *archive_link_arguments(args.library, driver=args.kind), '/Fe:' + str(binary)]
            if args.asan:
                command += ['/fsanitize=address', '/Zi']
                if 'clang' in args.compiler.name.lower(): command += ['/clang:-fsanitize=undefined']
                command += ['/link', '/INCREMENTAL:NO']
        else:
            command = [args.compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                       '-I' + str(args.include), client, *archive_link_arguments(args.library, driver=args.kind), '-lm', '-pthread', '-o', binary]
            command += ['-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections']
            if args.asan: command += ['-fsanitize=address,undefined', '-g']
        run(command, work)
        result = run([binary], work, 1 if answer is None else 0)
        if answer is None:
            position = (b'array-c.xr:3:1' if name.startswith('repeat-negative') else
                        b'array-c.xr:37:9' if name.endswith('read') else b'array-c.xr:41:7')
            assert not result.stdout and position in result.stderr, (name, result.stdout, result.stderr)
        else:
            expected_stdout = (str(answer) + os.linesep).encode()
            assert result.stdout == expected_stdout and not result.stderr, (name, result.stdout, result.stderr)
        record['cases'][name] = {'stdout': result.stdout.decode(), 'exit': result.returncode,
                                 'stderr': result.stderr.decode(),
                                 'sourceSHA256': hashlib.sha256(generated.read_bytes()).hexdigest()}
    (work / 'result.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(json.dumps(record), flush=True)


if __name__ == '__main__':
    main()
