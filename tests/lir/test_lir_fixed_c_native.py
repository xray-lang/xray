"""Exercise physical byte structs with the production execution boundary."""
from pathlib import Path
import hashlib
import json
import os
import platform
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.toolchain import archive_link_arguments


def fixed_value_type(text, count, aggregate):
    """Check the natural byte shape and its real public entry contract."""
    assert '({' not in text and 'XrtArray' not in text
    definitions = re.findall(
        r'typedef\s+struct\s+([A-Za-z_]\w*)\s*\{\s*uint8_t\s+bytes\[(\d+)\];\s*\}\s+\1\s*;', text)
    assert len(definitions) == 1 and int(definitions[0][1]) == count, definitions
    value_type = definitions[0][0]
    entry = re.search(r'^([A-Za-z_]\w*)\s+xray_lir_entry\(void\)\s*\{', text, re.M)
    assert entry and entry.group(1) == (value_type if aggregate else 'int64_t'), entry
    return value_type


def fixed_c_cases(args, root):
    root = Path(root) / 'fixed-values'
    root.mkdir()
    cases = {'fixed-1': 12, 'fixed-11': 32, 'fixed-16': 42, 'fixed-17': 44,
             'fixed-256': 10, 'fixed-state': 12, 'fixed-return': 'aggregate',
             'fixed-state-return': 'aggregate', 'fixed-negative': 'bounds',
             'fixed-bounds': 'bounds', 'fixed-unpublished': 'unpublished'}
    results = {}

    def execute(name, command, expected=0):
        command = [str(value) for value in command]
        result = subprocess.run(command, cwd=root, capture_output=True, timeout=30)
        (root / (name + '.stdout')).write_bytes(result.stdout)
        (root / (name + '.stderr')).write_bytes(result.stderr)
        results[name] = {'command': command, 'exit': result.returncode,
                         'stdoutHex': result.stdout.hex(), 'stderrHex': result.stderr.hex()}
        assert result.returncode == expected, results[name]
        return result

    for name, answer in cases.items():
        generated, client = root / (name + '.c'), root / (name + '-client.c')
        binary = root / (name + ('.exe' if os.name == 'nt' else ''))
        execute(name + '-emit', [args.fixture_driver, name, generated])
        text = generated.read_text(encoding='ascii')
        count = (int(name.removeprefix('fixed-')) if name.removeprefix('fixed-').isdigit() else
                 11 if name in ('fixed-negative', 'fixed-bounds') else 17)
        value_type = fixed_value_type(text, count, answer == 'aggregate')
        layout = (f'_Static_assert(sizeof({value_type}) == {count}, "fixed value size");\n'
                  f'_Static_assert(_Alignof({value_type}) == _Alignof(uint8_t), "byte alignment");\n'
                  f'_Static_assert(offsetof({value_type}, bytes) == 0, "byte storage offset");\n'
                  f'_Static_assert(sizeof((({value_type} *)0)->bytes) == {count}, "byte storage extent");\n')
        if answer == 'aggregate':
            body = (f'    {value_type} value = xray_lir_entry();\n'
                    '    _Static_assert(sizeof(value) == 17, "aggregate return size");\n'
                    '    for (uint32_t i = 0; i < 17; ++i)\n'
                    '        if (value.bytes[i] != (uint8_t)(i + 3)) return 1;\n')
        else:
            body = '    int64_t value = xray_lir_entry();\n'
            body += ('    (void)value;\n' if isinstance(answer, str) else
                     f'    if (value != INT64_C({answer})) return 1;\n')
        client.write_text(f'#include "{generated.name}"\n' + layout + 'int main(void) {\n' + body +
                          '    return 0;\n}\n', encoding='ascii')
        if args.kind == 'msvc':
            crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                   'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
            command = [args.compiler, '/nologo', '/std:c11', '/utf-8', '/O2', '/W4', '/WX', crt,
                       '/I' + str(args.include), client, *archive_link_arguments(args.library, driver=args.kind), '/Fe:' + str(binary)]
            if args.asan:
                command += ['/fsanitize=address', '/Zi', '/clang:-fsanitize=undefined',
                            '/link', '/INCREMENTAL:NO']
        else:
            command = [args.compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                       '-I' + str(args.include), client, *archive_link_arguments(args.library, driver=args.kind), '-lm', '-pthread', '-o', binary]
            command += ['-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections']
            if args.asan: command += ['-fsanitize=address,undefined', '-g']
        execute(name + '-compile', command)
        failed = answer in ('bounds', 'unpublished')
        result = execute(name + '-run', [binary], 1 if failed else 0)
        assert not result.stdout
        if failed:
            position = b'fixed-state.xr:45:6' if answer == 'unpublished' else b'fixed-get.xr:37:4'
            assert position in result.stderr, results[name + '-run']
        else:
            assert not result.stderr, results[name + '-run']
        results[name + '-source'] = {'sha256': hashlib.sha256(generated.read_bytes()).hexdigest()}
    (root / 'result.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    return results
