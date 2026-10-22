"""Compile real LIR-generated C after its producer has been destroyed."""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import tempfile


def run(arguments, **kwargs):
    print('$ ' + json.dumps([str(value) for value in arguments]), flush=True)
    completed = subprocess.run([str(value) for value in arguments], capture_output=True, **kwargs)
    if completed.stdout:
        print(completed.stdout.decode('utf-8', errors='replace'), end='', flush=True)
    if completed.stderr:
        print(completed.stderr.decode('utf-8', errors='replace'), end='', flush=True)
    if completed.returncode:
        raise RuntimeError(f'command failed with exit {completed.returncode}')
    return completed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', required=True, type=Path)
    parser.add_argument('--faults', required=True, type=Path)
    parser.add_argument('--source-driver', required=True, type=Path)
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=['msvc', 'gnu'])
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='lir-c-', dir=args.work))
    results = {}
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    expected = {'arithmetic': 17, 'extremes': -1, 'overflow': 9223372036854775806,
                'fib': 2178309, 'swap': 1234, 'self': 1212,
                'same-true': 1234, 'same-false': 3412, 'unused': 42, 'unit': None, 'bool': 1,
                'source-entry': 0, 'source-call': 0, 'divrem': 7}
    sources = {
        'source-entry': 'fn main()->string{return "ordinary"}\nfn work()->i64{var i=0\nwhile(i<5){i=i+1}\nreturn i}\nwork()\n',
        'source-call': 'fn sum(n:i64)->i64{var i=0\nvar total=0\nwhile(i<n){total=total+i\ni=i+1}\nreturn total}\nsum(7)\n',
    }
    for name, answer in expected.items():
        source = root / (name + '.c')
        adapter = root / (name + '-adapter.c')
        executable = root / (name + ('.exe' if os.name == 'nt' else ''))
        if name in sources:
            source_file = root / (name + '.xr')
            source_file.write_text(sources[name], encoding='utf-8')
            run([args.source_driver, source_file, source], cwd=root)
        else:
            run([args.driver, name, source], cwd=root)
        text = source.read_text(encoding='ascii')
        assert '({' not in text and 'switch' not in text and 'xrt_' not in text
        assert '#include <stdint.h>' in text and 'xray_lir_entry(void)' in text
        if name == 'unused':
            assert 'double' not in text and 'float' not in text and 'main' not in text
        if name == 'fib':
            # A separate fixture has exactly one recursive implementation.
            match = re.search(r'static int64_t xrl_f1\(int64_t p0\) \{(.*?)\n\}', text, re.S)
            assert match and match.group(1).count('xrl_f1(') == 2
            assert 'xray_lir_entry' not in match.group(1)
        constant = f'INT64_C({answer})' if answer is not None and answer >= 0 else f'(-INT64_C({-answer}))' if answer is not None else '0'
        result_type = 'void' if name == 'unit' else '_Bool' if name == 'bool' else 'int64_t'
        adapter_text = ('#include <stdint.h>\n#include <stdio.h>\n'
            f'extern {result_type} xray_lir_entry(void);\n'
            'int main(void) {\n    int64_t actual = xray_lir_entry();\n'
            f'    if (actual != {constant}) {{\n'
            '        fputs("wrong scalar result\\n", stderr);\n        return 1;\n    }\n'
            '    return 0;\n}\n')
        if name == 'unit':
            adapter_text = adapter_text.replace('int64_t actual = xray_lir_entry();',
                                                'xray_lir_entry();\n    int64_t actual = 0;')
        if name == 'divrem':
            # Check the actual definitions; a separate adapter supplies runtime numerators.
            gateway = root / 'divrem-gateway.c'
            gateway.write_text('#include "divrem.c"\n' + ''.join(
                f'_Static_assert(_Generic(&xrl_f{i}, int64_t (*)(int64_t): 1, default: 0), "division ABI");\n'
                f'int64_t (*get_f{i}(void))(int64_t) {{ return xrl_f{i}; }}\n'
                for i in range(2, 6)), encoding='ascii')
            adapter_text = ('#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n' + ''.join(
                f'extern int64_t (*get_f{i}(void))(int64_t);\n' for i in range(2, 6)) +
                'int main(int argc, char **argv) {\n    if (argc != 2) return 2;\n'
                '    char *end; int64_t n = (int64_t)strtoll(argv[1], &end, 10);\n'
                '    if (*end) return 2;\n' + ''.join(
                    f'    if (printf("%lld\\n", (long long)get_f{i}()(n)) < 0) return 3;\n'
                    for i in range(2, 6)) + '    return 0;\n}\n')
            source = gateway
        adapter.write_text(adapter_text, encoding='ascii')
        if args.kind == 'msvc':
            command = [args.compiler, '/nologo', '/std:c11', '/W4', '/WX', '/O2',
                       source, adapter, '/Fe:' + str(executable)]
            if args.asan:
                command += ['/fsanitize=address', '/Zi']
                if 'clang' in args.compiler.name.lower():
                    command += ['/MD', '/clang:-fsanitize=undefined']
                command += ['/link', '/INCREMENTAL:NO']
        else:
            command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                       '-O2', source, adapter, '-o', executable]
            if args.asan:
                command += ['-fsanitize=address,undefined', '-g']
        if name == 'divrem':
            no_lto = '/clang:-fno-lto' if 'clang' in args.compiler.name.lower() else '/GL-'
            command.insert(1, no_lto if args.kind == 'msvc' else '-fno-lto')
        run(command, cwd=root)
        if name == 'divrem':
            vectors = [
                (-9223372036854775808, -3074457345618258602, -2, 95086309658296657, -79),
                (-98, -32, -2, 1, -1), (-7, -2, -1, 0, -7), (-1, 0, -1, 0, -1),
                (0, 0, 0, 0, 0), (1, 0, 1, 0, 1), (7, 2, 1, 0, 7),
                (98, 32, 2, -1, 1),
                (9223372036854775807, 3074457345618258602, 1, -95086309658296657, 78),
            ]
            observations = []
            for n, *answers in vectors:
                completed = run([executable, str(n)], cwd=root)
                fixed = ''.join(str(a) + ('\r\n' if os.name == 'nt' else '\n') for a in answers).encode('ascii')
                assert completed.stdout == fixed and completed.stderr == b'', (n, completed)
                observations.append({'numerator': n, 'expectedHex': fixed.hex(),
                                     'stdoutHex': completed.stdout.hex(),
                                     'stderrHex': completed.stderr.hex(), 'exit': completed.returncode})
            (root / 'divrem-vectors.json').write_text(json.dumps(observations, indent=2) + '\n')
        else:
            completed = run([executable], cwd=root)
            assert completed.stdout == b'' and completed.stderr == b''
        results[name] = 'PASS'
    output_source = root / 'ordinary-output.xr'
    output_text = root / 'ordinary-output.c'
    output_source.write_text('print(1)\n', encoding='utf-8')
    run([args.source_driver, output_source, output_text], cwd=root)
    text = output_text.read_text(encoding='ascii')
    assert '#include "xray_native_runtime.h"' in text
    assert text.count('const XrtSourceMap xrt_standalone_source_map =') == 1
    assert text.count('xrt_run_standalone(') == 1
    assert re.search(r'xrt_print_i64\(v[0-9]+, UINT32_C\(1\)\);', text)
    results['ordinary-output-owned-text'] = 'PASS'
    environment = dict(os.environ, XRAY_CGEN_ICE_DIR=str(root))
    child = subprocess.run([str(args.faults), '--ice'], cwd=root, env=environment, capture_output=True)
    print(child.stderr.decode('utf-8', errors='replace'), end='', flush=True)
    assert child.returncode != 0 and b'UNEXPECTED_PUBLICATION' not in child.stdout
    assert b'[ICE]' in child.stderr and b'W4_FORWARD_REF' in child.stderr
    dumps = [path for path in root.iterdir() if path.suffix == '.c' and 'v9' in path.read_text(encoding='utf-8')]
    assert any(path.read_bytes() == b'int xray_lir_entry(void) {\n    return v9;\n}\n' for path in dumps)
    # Compilation is attempted only for successfully published outputs above.
    results['malformed-output-no-host-compile'] = 'PASS'
    (root / 'result.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print('real generated-C compilation/execution and ICE checks PASS: ' + str(root), flush=True)


if __name__ == '__main__':
    main()
