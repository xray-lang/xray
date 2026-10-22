"""Observe integer, pointer and binary64 carriers across separate native TUs."""
import argparse
import hashlib
import json
import os
import re
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


_F64_MACROS = ('_MSC_VER', '_M_X64', '_M_ARM64EC', '_M_FP_FAST', '__FAST_MATH__', '__FINITE_MATH_ONLY__')
_ABI_ERROR = 'F64_TRANSPORT_REQUIRES_VALIDATED_MICROSOFT_X64_ABI'
_FP_ERROR = 'F64_TRANSPORT_REQUIRES_NONFAST_FLOATING_POINT_MODE'
_F64_INPUTS = ['0000000000000000', '8000000000000000', '3ff0000000000000', 'bff0000000000000',
               '0000000000000001', '8000000000000001', '000fffffffffffff', '0010000000000000',
               '7fefffffffffffff', '7ff0000000000000', 'fff0000000000000', '7ff8000000000000',
               '7ff8000000001234', 'fff8000000001234', '7ff0000000000001', 'fff0000000001234']


def _f64_profile(compiler, kind, options, work):
    """Read independent compiler facts; a failed query is never a target denial."""
    source = work / 'target-facts.c'
    text = '#define F64_STR1(x) #x\n#define F64_STR(x) F64_STR1(x)\n'
    for i, name in enumerate(_F64_MACROS):
        text += (f'#ifdef {name}\nconst int f64_d{i} = 1;\nconst char *f64_v{i} = F64_STR({name});\n'
                 f'#else\nconst int f64_d{i} = 0;\nconst char *f64_v{i} = "UNDEFINED";\n#endif\n')
    source.write_text(text, encoding='ascii')
    # These options only generate/debug/instrument code; retain target/FP/defines.
    ignored = {'/Zi', '/Z7', '/GL-', '/clang:-fno-lto', '-fno-lto', '-g'}
    preprocess = [x for x in options if str(x) not in ignored and not str(x).startswith(
        ('/fsanitize=', '/clang:-fsanitize=', '-fsanitize='))]
    argv = [compiler, *preprocess, *(['/EP'] if kind == 'msvc' else ['-E', '-P']), source]
    record = run(argv, work)
    output = bytes.fromhex(record['stdoutHex']).decode('ascii')
    facts = {}
    for i, name in enumerate(_F64_MACROS):
        defined = re.findall(rf'const int f64_d{i}\s*=\s*([01])\s*;', output)
        value = re.findall(rf'const char\s*\*f64_v{i}\s*=\s*"([^"]+)"\s*;', output)
        assert len(defined) == len(value) == 1, (name, output)
        present = defined[0] == '1'
        assert (value[0] != 'UNDEFINED') == present
        assert not present or value[0].isdigit(), (name, value)
        facts[name] = {'defined': present, 'value': int(value[0]) if present else None}
    abi = facts['_MSC_VER']['defined'] and facts['_M_X64']['defined'] and not facts['_M_ARM64EC']['defined']
    fast = facts['_M_FP_FAST']['defined'] or facts['__FAST_MATH__']['defined'] or (
        facts['__FINITE_MATH_ONLY__']['defined'] and facts['__FINITE_MATH_ONLY__']['value'] > 0)
    return {'facts': facts, 'query': record, 'abiSupported': abi, 'fastMode': bool(fast),
            'supported': abi and not fast, 'expectedGuard': _ABI_ERROR if not abi else _FP_ERROR if fast else None}


def _f64_reject(command, work, marker, product):
    result = subprocess.run([str(x) for x in command], cwd=work, capture_output=True, timeout=90)
    output = result.stdout + result.stderr
    errors = [line for line in output.decode('utf-8', errors='replace').splitlines()
              if re.search(r'\b(?:fatal )?error(?: C[0-9]+)?:', line)]
    assert result.returncode and marker.encode('ascii') in output and errors, (command, output)
    assert all(_ABI_ERROR in line or _FP_ERROR in line for line in errors), errors
    assert not product.exists(), product
    return {'argv': [str(x) for x in command], 'exit': result.returncode,
            'stdoutHex': result.stdout.hex(), 'stderrHex': result.stderr.hex(),
            'status': 'UNSUPPORTED_TARGET_REJECTED', 'payloadVectorsExecuted': 0}


def _f64_adapter():
    return r'''#include "carrier.h"
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fenv.h>
int main(int argc, char **argv) {
    double one = 1.0, negative_zero = -0.0;
    uint64_t first, second; memcpy(&first, &one, 8); memcpy(&second, &negative_zero, 8);
    if (first != UINT64_C(0x3ff0000000000000) || second != UINT64_C(0x8000000000000000)) return 2;
    fenv_t original;
    if (fegetenv(&original)) return 3;
    Carrier call = carrier_get(); Root root = root_get();
    for (int i = 1; i < argc; ++i) {
        char *end = NULL; errno = 0;
        uint64_t expected = strtoull(argv[i], &end, 16);
        if (errno || end == argv[i] || *end) return 4;
        for (int mode = 0; mode < 2; ++mode) {
            int rounding = mode ? FE_UPWARD : FE_TONEAREST;
            if (fesetround(rounding) || feclearexcept(FE_ALL_EXCEPT)) return 5;
            if (mode && feraiseexcept(FE_INEXACT)) return 6;
            int flags = fetestexcept(FE_ALL_EXCEPT), round = fegetround();
            double input; memcpy(&input, &expected, 8);
            double result = call(input); uint64_t actual; memcpy(&actual, &result, 8);
            int after_flags = fetestexcept(FE_ALL_EXCEPT), after_round = fegetround();
            if (actual != expected || after_flags != flags || after_round != round) return 7;
            printf("%016" PRIx64 "\n", actual);
        }
    }
    if (fesetenv(&original) || fflush(stdout)) return 8;
    double value = root(); uint64_t bits; memcpy(&bits, &value, 8);
    printf("%016" PRIx64 "\n", bits); return 0;
}
'''


def _f64_cases(args, root, results):
    roots = {'f64': '8000000000000000', 'f64-zero': '0000000000000000',
             'f64-finite': '3ff0000000000000', 'f64-subnormal': '0000000000000001',
             'f64-inf': '7ff0000000000000', 'f64-nan': '7ff8000000000000'}
    newline = b'\r\n' if os.name == 'nt' else b'\n'
    profiles = {}
    for base, answer in roots.items():
        for output in (False, True):
            case = base + ('-output' if output else '')
            for mode in ('before', 'after'):
                for fp in (('default', 'precise', 'strict') if args.kind == 'msvc' else ('default',)):
                    work = root / (case + '-' + mode + '-' + fp); work.mkdir()
                    generated, gateway, host = [work / x for x in ('generated.c', 'gateway.c', 'adapter.c')]
                    producer = run([args.driver, case, mode, generated], work)
                    observation = json.loads(bytes.fromhex(producer['stdoutHex']))
                    assert observation['carrier'] == 'f64'
                    a, b, _ = observation['parameters']
                    assert observation['backedgeArguments'][:2] == [b, a] and observation['returned'] == b
                    text = generated.read_text(encoding='ascii')
                    assert 'double' in text and 'memcpy' in text and _ABI_ERROR in text and _FP_ERROR in text
                    if output:
                        assert '*(double *)context = xrl_f1();' in text and 'xrt_run_standalone(' in text
                    else:
                        assert 'xrt_' not in text and 'xray_native_runtime.h' not in text
                    (work / 'carrier.h').write_text('typedef double Scalar;\ntypedef Scalar (*Carrier)(Scalar);\n'
                        'typedef double (*Root)(void);\nCarrier carrier_get(void);\nRoot root_get(void);\n', encoding='ascii')
                    gateway.write_text('#include "generated.c"\n#include "carrier.h"\n'
                        '_Static_assert(_Generic(&xrl_f2, Carrier: 1, default: 0), "helper type");\n'
                        '_Static_assert(_Generic(&xray_lir_entry, Root: 1, default: 0), "root type");\n'
                        'Carrier carrier_get(void) { return &xrl_f2; }\nRoot root_get(void) { return &xray_lir_entry; }\n', encoding='ascii')
                    if output:
                        with gateway.open('a', encoding='ascii') as stream:
                            stream.write('_Static_assert(_Generic(&xrl_f1, Root: 1, default: 0), "worker type");\n')
                    host.write_text(_f64_adapter(), encoding='ascii')
                    if args.kind == 'msvc':
                        crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                               'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
                        flags = ['/nologo', '/std:c11', '/W4', '/WX', '/O2', '/utf-8', crt]
                        flags += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
                        if fp != 'default': flags += ['/fp:' + fp]
                        if args.asan:
                            flags += ['/fsanitize=address', '/Zi']
                            if 'clang' in args.compiler.name.lower(): flags += ['/clang:-fsanitize=undefined']
                        flags += ['/I' + str(args.include)] if output else []
                        objects = [work / 'gateway.obj', work / 'adapter.obj']
                        commands = [[args.compiler, *flags, '/c', path, '/Fo' + str(obj)]
                                    for path, obj in zip((gateway, host), objects)]
                        link = [args.compiler, *flags, *objects, *([args.library] if output else []),
                                '/Fe:' + str(work / 'run.exe'), '/link', '/INCREMENTAL:NO']
                        executable = work / 'run.exe'
                    else:
                        flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2', '-fno-lto']
                        if args.asan: flags += ['-fsanitize=address,undefined', '-g']
                        flags += ['-I' + str(args.include)] if output else []
                        objects = [work / 'gateway.o', work / 'adapter.o']
                        commands = [[args.compiler, *flags, '-c', path, '-o', obj]
                                    for path, obj in zip((gateway, host), objects)]
                        executable = work / 'run'
                        link = [args.compiler, *flags, *objects, *([args.library] if output else []), '-lm', '-o', executable]
                    if fp not in profiles:
                        profiles[fp] = _f64_profile(args.compiler, args.kind, flags, work)
                    profile = profiles[fp]
                    record = {'case': case, 'mode': mode, 'fpMode': fp, 'producer': producer,
                              'observation': observation, 'profile': profile, 'generatedSHA256': digest(generated)}
                    if not profile['supported']:
                        record['negative'] = _f64_reject(commands[0], work, profile['expectedGuard'], objects[0])
                    else:
                        record['compiles'] = [run(command, work) for command in commands]
                        record['link'] = run(link, work)
                        inputs = _F64_INPUTS if base == 'f64' else []
                        native = run([executable, *inputs], work)
                        expected = b''.join(x.encode('ascii') + newline for x in inputs for _ in range(2))
                        if output: expected += b'42\n'
                        expected += answer.encode('ascii') + newline
                        assert native['stdoutHex'] == expected.hex() and native['stderrHex'] == ''
                        record.update(native=native, expectedHex=expected.hex(), status='PASS',
                                      payloadVectorsExecuted=len(inputs) * 2,
                                      objects=[{'path': str(obj), 'sha256': digest(obj)} for obj in objects])
                    if base == 'f64' and not output and mode == 'before' and fp == 'default':
                        negative_flags = [x for x in flags if not str(x).startswith(('/fsanitize=', '/clang:-fsanitize=', '-fsanitize=')) and x != '/Zi']
                        fast = ['/fp:fast'] if args.kind == 'msvc' else ['-ffast-math']
                        variants = [('fast', args.compiler, fast)]
                        if 'clang-cl' in args.compiler.name.lower():
                            variants += [(arch, args.compiler, ['/clang:--target=' + arch + '-pc-windows-msvc'])
                                         for arch in ('aarch64', 'arm64ec')]
                        if args.kind == 'msvc' and args.compiler.name.lower() == 'cl.exe':
                            x86 = args.compiler.parent.parent / 'x86' / args.compiler.name
                            record['x86Control'] = {'compiler': str(x86), 'available': x86.is_file(),
                                                    'qualification': 'compile-only negative, no execution'}
                            if x86.is_file():
                                variants += [('x86-default', x86, []), ('x86-fast', x86, fast)]
                        record['guardRejections'] = []
                        for label, compiler, options in variants:
                            control = work / ('control-' + label); control.mkdir()
                            facts = _f64_profile(compiler, args.kind, [*negative_flags, *options], control)
                            marker = facts['expectedGuard']
                            assert marker in (_ABI_ERROR, _FP_ERROR)
                            if label.endswith('fast'): assert facts['fastMode']
                            if label != 'fast': assert not facts['abiSupported'] and marker == _ABI_ERROR
                            obj = control / ('rejected.obj' if args.kind == 'msvc' else 'rejected.o')
                            command = ([compiler, *negative_flags, *options, '/c', generated, '/Fo' + str(obj)]
                                       if args.kind == 'msvc' else [compiler, *negative_flags, *options, '-c', generated, '-o', obj])
                            rejected = _f64_reject(command, control, marker, obj)
                            rejected.update(control=label, profile=facts, expectedGuard=marker,
                                            fpOnlyQualification=marker == _FP_ERROR)
                            record['guardRejections'].append(rejected)
                    if output:
                        record['runtime'] = {'library': str(args.library), 'librarySHA256': digest(args.library),
                                             'header': str(args.include / 'xray_native_runtime.h'),
                                             'headerSHA256': digest(args.include / 'xray_native_runtime.h'),
                                             'msvcRuntime': args.msvc_runtime}
                    results.append(record)
                    (root / 'case-results.json').write_text(json.dumps(results, indent=2) + '\n')


_FNEG_VECTORS = [
    ('0000000000000000', '8000000000000000'), ('8000000000000000', '0000000000000000'),
    ('3ff0000000000000', 'bff0000000000000'), ('bff0000000000000', '3ff0000000000000'),
    ('0000000000000001', '8000000000000001'), ('8000000000000001', '0000000000000001'),
    ('000fffffffffffff', '800fffffffffffff'), ('800fffffffffffff', '000fffffffffffff'),
    ('0010000000000000', '8010000000000000'), ('8010000000000000', '0010000000000000'),
    ('7fefffffffffffff', 'ffefffffffffffff'), ('ffefffffffffffff', '7fefffffffffffff'),
    ('7ff0000000000000', 'fff0000000000000'), ('fff0000000000000', '7ff0000000000000'),
    ('7ff8000000000000', '7ff8000000000000'), ('7ff8000000001234', '7ff8000000000000'),
    ('fff8000000001234', '7ff8000000000000'), ('7ff0000000000001', '7ff8000000000000'),
    ('fff0000000001234', '7ff8000000000000'), ('7fffffffffffffff', '7ff8000000000000'),
]


def _fneg_adapter():
    return r'''#include "carrier.h"
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fenv.h>
#if defined(_MSC_VER) && defined(_M_X64) && !defined(_M_ARM64EC) && !defined(_M_FP_FAST) && !defined(__FAST_MATH__) && (!defined(__FINITE_MATH_ONLY__) || __FINITE_MATH_ONLY__ <= 0)
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#elif defined(_MSC_VER)
#pragma fenv_access(on)
#endif
#endif
int main(int argc, char **argv) {
    double one = 1.0, negative_zero = -0.0;
    uint64_t first, second; memcpy(&first, &one, 8); memcpy(&second, &negative_zero, 8);
    if (first != UINT64_C(0x3ff0000000000000) || second != UINT64_C(0x8000000000000000)) return 2;
    fenv_t original; if (fegetenv(&original)) return 3;
    Carrier call = carrier_get(); Root root = root_get();
    for (int i = 1; i < argc; ++i) {
        char *end = NULL; errno = 0;
        uint64_t input = strtoull(argv[i], &end, 16);
        if (errno || end == argv[i] || *end != ':') return 4;
        const char *next = end + 1; errno = 0;
        uint64_t expected = strtoull(next, &end, 16);
        if (errno || end == next || *end) return 5;
        for (int mode = 0; mode < 2; ++mode) {
            if (fesetround(mode ? FE_UPWARD : FE_TONEAREST) || feclearexcept(FE_ALL_EXCEPT)) return 6;
            if (mode && feraiseexcept(FE_INEXACT)) return 7;
            int flags = fetestexcept(FE_ALL_EXCEPT), rounding = fegetround();
            double value; memcpy(&value, &input, 8);
            double answer = call(value);
            int after_flags = fetestexcept(FE_ALL_EXCEPT), after_rounding = fegetround();
            uint64_t actual; memcpy(&actual, &answer, 8);
            if (actual != expected || flags != after_flags || rounding != after_rounding) return 8;
            if (fesetenv(&original)) return 9;
            printf("%016" PRIx64 "\n", actual);
        }
    }
    if (fflush(stdout)) return 10;
    double value = root(); uint64_t bits; memcpy(&bits, &value, 8);
    printf("%016" PRIx64 "\n", bits); return fflush(stdout) ? 11 : 0;
}
'''


def _fneg_commands(args, work, output, fp):
    sources = [work / name for name in ('gateway.c', 'adapter.c')]
    if args.kind == 'msvc':
        crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
               'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
        flags = ['/nologo', '/std:c11', '/W4', '/WX', '/O2', '/utf-8', crt]
        flags += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
        if fp != 'default': flags += ['/fp:' + fp]
        if args.asan:
            flags += ['/fsanitize=address', '/Zi']
            if 'clang' in args.compiler.name.lower(): flags += ['/clang:-fsanitize=undefined']
        if output: flags += ['/I' + str(args.include)]
        objects = [work / 'gateway.obj', work / 'adapter.obj']
        compiles = [[args.compiler, *flags, '/c', source, '/Fo' + str(obj)]
                    for source, obj in zip(sources, objects)]
        executable = work / 'run.exe'
        link = [args.compiler, *flags, *objects, *([args.library] if output else []),
                '/Fe:' + str(executable), '/link', '/INCREMENTAL:NO']
    else:
        flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2', '-fno-lto']
        if args.asan: flags += ['-fsanitize=address,undefined', '-g']
        if output: flags += ['-I' + str(args.include)]
        objects = [work / 'gateway.o', work / 'adapter.o']
        compiles = [[args.compiler, *flags, '-c', source, '-o', obj] for source, obj in zip(sources, objects)]
        executable = work / 'run'
        link = [args.compiler, *flags, *objects, *([args.library] if output else []), '-lm', '-o', executable]
    return flags, objects, compiles, link, executable


def _fneg_controls(args, flags, generated, work):
    flags = [x for x in flags if not str(x).startswith(('/fsanitize=', '/clang:-fsanitize=', '-fsanitize='))
             and x != '/Zi']
    fast = ['/fp:fast'] if args.kind == 'msvc' else ['-ffast-math']
    variants = [('fast', args.compiler, fast)]
    if 'clang-cl' in args.compiler.name.lower():
        variants += [(arch, args.compiler, ['/clang:--target=' + arch + '-pc-windows-msvc'])
                     for arch in ('aarch64', 'arm64ec')]
    if args.kind == 'msvc' and args.compiler.name.lower() == 'cl.exe':
        x86 = args.compiler.parent.parent / 'x86' / args.compiler.name
        if x86.is_file(): variants += [('x86-default', x86, []), ('x86-fast', x86, fast)]
    rejected = []
    for label, compiler, options in variants:
        control = work / ('control-' + label); control.mkdir()
        facts = _f64_profile(compiler, args.kind, [*flags, *options], control)
        marker = facts['expectedGuard']; assert marker in (_ABI_ERROR, _FP_ERROR)
        if label.endswith('fast'): assert facts['fastMode']
        if label != 'fast': assert not facts['abiSupported'] and marker == _ABI_ERROR
        obj = control / ('rejected.obj' if args.kind == 'msvc' else 'rejected.o')
        command = ([compiler, *flags, *options, '/c', generated, '/Fo' + str(obj)] if args.kind == 'msvc'
                   else [compiler, *flags, *options, '-c', generated, '-o', obj])
        result = _f64_reject(command, control, marker, obj)
        result.update(control=label, profile=facts, expectedGuard=marker)
        rejected.append(result)
    return rejected


def _fneg_cases(args, root, results):
    roots = {'fneg-zero': '8000000000000000', 'fneg-nan': '7ff8000000000000'}
    newline = b'\r\n' if os.name == 'nt' else b'\n'
    vectors = [source + ':' + expected for source, expected in _FNEG_VECTORS]
    profiles = {}
    for base, answer in roots.items():
        for output in (False, True):
            case = base + ('-output' if output else '')
            for mode in ('before', 'after'):
                for fp in (('default', 'precise', 'strict') if args.kind == 'msvc' else ('default',)):
                    work = root / (case + '-' + mode + '-' + fp); work.mkdir()
                    generated = work / 'generated.c'
                    producer = run([args.driver, case, mode, generated], work)
                    assert not producer['stderrHex']
                    observation = json.loads(bytes.fromhex(producer['stdoutHex']))
                    assert observation['carrier'] == 'f64-negation' and observation['helper'] == 2
                    assert observation['parameter'] == observation['operand']
                    assert observation['line'] == 7 and observation['column'] == 5
                    text = generated.read_text(encoding='ascii')
                    policy = text.index('#pragma STDC FENV_ACCESS ON')
                    assert text.rfind('#include ') < policy < text.index('double ')
                    assert '#pragma fenv_access(on)' in text and '0x7ff8000000000000' in text
                    assert _ABI_ERROR in text and _FP_ERROR in text
                    if not output: assert 'xrt_' not in text and 'xray_native_runtime.h' not in text
                    (work / 'carrier.h').write_text('typedef double Scalar;\ntypedef Scalar (*Carrier)(Scalar);\n'
                        'typedef double (*Root)(void);\nCarrier carrier_get(void);\nRoot root_get(void);\n', encoding='ascii')
                    (work / 'gateway.c').write_text('#include "carrier.h"\n#include "generated.c"\n'
                        '_Static_assert(_Generic(&xrl_f2, Carrier: 1, default: 0), "negation type");\n'
                        '_Static_assert(_Generic(&xray_lir_entry, Root: 1, default: 0), "root type");\n'
                        'Carrier carrier_get(void) { return &xrl_f2; }\n'
                        'Root root_get(void) { return &xray_lir_entry; }\n', encoding='ascii')
                    (work / 'adapter.c').write_text(_fneg_adapter(), encoding='ascii')
                    flags, objects, compiles, link, executable = _fneg_commands(args, work, output, fp)
                    if fp not in profiles: profiles[fp] = _f64_profile(args.compiler, args.kind, flags, work)
                    profile = profiles[fp]
                    record = {'case': case, 'mode': mode, 'fpMode': fp, 'producer': producer,
                              'observation': observation, 'profile': profile, 'generatedSHA256': digest(generated)}
                    if not profile['supported']:
                        record['negative'] = _f64_reject(compiles[0], work, profile['expectedGuard'], objects[0])
                    else:
                        record['compiles'] = [run(command, work) for command in compiles]
                        record['link'] = run(link, work)
                        native = run([executable, *vectors], work)
                        expected = b''.join(x.encode('ascii') + newline for _, x in _FNEG_VECTORS for _ in range(2))
                        if output: expected += b'42\n'
                        expected += answer.encode('ascii') + newline
                        assert native['stdoutHex'] == expected.hex() and not native['stderrHex']
                        record.update(native=native, expectedHex=expected.hex(), status='PASS',
                            payloadVectorsExecuted=len(_FNEG_VECTORS) * 2, observerDeclaresFenv=True,
                            immediateFenvCapture=True, rootProbeDefaultEnv=True,
                            objects=[{'path': str(obj), 'sha256': digest(obj)} for obj in objects])
                    if base == 'fneg-zero' and not output and mode == 'before' and fp == 'default':
                        record['guardRejections'] = _fneg_controls(args, flags, generated, work)
                    if output:
                        record['runtime'] = {'library': str(args.library), 'librarySHA256': digest(args.library),
                            'header': str(args.include / 'xray_native_runtime.h'),
                            'headerSHA256': digest(args.include / 'xray_native_runtime.h'), 'msvcRuntime': args.msvc_runtime}
                    results.append(record)
                    (root / 'case-results.json').write_text(json.dumps(results, indent=2) + '\n')


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
    _f64_cases(args, root, results)
    _fneg_cases(args, root, results)
    print('Typed carriers and explicit target frontiers passed across separate native TUs', flush=True)


if __name__ == '__main__':
    main()
