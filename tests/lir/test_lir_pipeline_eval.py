"""Compare independent Source pipelines and preserve unqualified stage receipts."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time

MODES = ('raw', 'sccp', 'inline', 'sccp-inline-inline-sccp')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invoke(folder, label, command, expected=0, stdout=None):
    command = [str(x) for x in command]
    (folder / (label + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
    start = time.perf_counter_ns()
    p = subprocess.run(command, cwd=folder, stdout=stdout or subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=900)
    record = {'exit': p.returncode, 'child_wall_ns': time.perf_counter_ns() - start,
              'stdoutHex': (p.stdout or b'').hex(), 'stderrHex': p.stderr.hex(),
              'formal_performance': False}
    for suffix, data in (('stdout', p.stdout or b''), ('stderr', p.stderr)):
        (folder / (label + '.' + suffix)).write_bytes(data)
    (folder / (label + '.exit')).write_text(str(p.returncode))
    (folder / (label + '.result.json')).write_text(json.dumps(record, indent=2) + '\n')
    assert p.returncode == expected, (label, record, folder)
    return p, record


def adapter(path, generated, count):
    # Only this test adapter needs the platform macro header. The emitted
    # program itself still depends on its public runtime header alone.
    source = r'''#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "base/xplatform.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#else
#include <time.h>
#endif
#include "GENERATED"
_Static_assert(_Generic(&xray_lir_entry, int64_t (*)(void): 1, default: 0), "complete i64 root");
static uint64_t frequency(void) {
#if defined(XR_OS_WINDOWS)
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) exit(2);
    return (uint64_t)f.QuadPart;
#else
    return UINT64_C(1000000000);
#endif
}
static uint64_t ticks(void) {
#if defined(XR_OS_WINDOWS)
    LARGE_INTEGER t;
    if (!QueryPerformanceCounter(&t) || t.QuadPart < 0) exit(2);
    return (uint64_t)t.QuadPart;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 || t.tv_nsec < 0 ||
        (uint64_t)t.tv_sec > (UINT64_MAX - (uint64_t)t.tv_nsec) / UINT64_C(1000000000)) exit(2);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
#endif
}
static void on_exit(void) { fputs("EXIT_CALLBACK", stderr); }
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1 && atexit(on_exit)) return 2;
    uint64_t durations[COUNT], freq = frequency();
    int64_t results[COUNT];
    for (unsigned i = 0; i < COUNT; ++i) {
        uint64_t before = ticks();
        results[i] = xray_lir_entry();
        uint64_t after = ticks();
        if (after < before || results[i] != INT64_C(0)) return 3;
        durations[i] = after - before;
    }
    if (argc > 1) fputs("AFTER_ENTRY", stderr);
    FILE *file = NULL;
#if defined(XR_OS_WINDOWS)
    if (fopen_s(&file, "native-ticks.json", "wb")) return 2;
#else
    file = fopen("native-ticks.json", "wb");
#endif
    if (!file) return 2;
    fprintf(file, "{\"clock_frequency\":%" PRIu64 ",\"formal_performance\":false,\"samples\":[", freq);
    for (unsigned i = 0; i < COUNT; ++i)
        fprintf(file, "%s{\"completed_entry\":true,\"ticks\":%" PRIu64 ",\"result\":%" PRId64 "}",
            i ? "," : "", durations[i], results[i]);
    fputs("]}\n", file);
    int failed = ferror(file);
    return fclose(file) || failed ? 2 : 0;
}
'''
    path.write_text(source.replace('GENERATED', generated.name).replace('COUNT', str(count)), encoding='ascii')


def compile_command(args, source, executable):
    if args.kind == 'msvc':
        crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
               'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
        command = [args.compiler, '/nologo', '/std:c11', '/W4', '/WX', '/O2', '/utf-8', crt,
                   '/I' + str(args.include), '/I' + str(args.source_root / 'src'),
                   source, args.library, '/Fe:' + str(executable)]
        command += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
        if args.asan:
            command += ['/fsanitize=address', '/Zi']
            if 'clang' in args.compiler.name.lower():
                command += ['/clang:-fsanitize=undefined']
            command += ['/link', '/INCREMENTAL:NO']
    else:
        command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-O2', '-fno-lto', '-I' + str(args.include),
                   '-I' + str(args.source_root / 'src'), source, args.library, '-pthread', '-o', executable]
        if args.asan:
            command += ['-fsanitize=address,undefined', '-g']
    return command


def check_receipt(receipt, mode, expected_runs, failure=False):
    passes = {'raw': 0, 'sccp': 1, 'inline': 1, 'sccp-inline-inline-sccp': 4}[mode]
    assert receipt['comparisons'] == passes and len(receipt['snapshots']) == passes + 1
    assert receipt['clock_frequency'] > 0 and receipt['root_result_type'] == 'i64'
    assert receipt['formal_performance'] is False
    assert len(receipt['samples']) == expected_runs
    assert len({s['root_ordinal'] for s in receipt['snapshots']}) == 1
    module_counts = {s['MODULE']['functions'] + s['MODULE']['external'] for s in receipt['snapshots']}
    assert len(module_counts) == 1
    for snapshot in receipt['snapshots']:
        for label in ('MODULE', 'ROOT_CFG_CLOSURE', 'C_SELECTED_FUNCTION_ALL_BLOCKS'):
            assert snapshot[label]['functions'] > 0 and snapshot[label]['instructions'] > 0
    for sample in receipt['samples']:
        assert sample['prepare_ticks'] >= 0 and sample['run_ticks'] >= 0
        assert sample['completed_entry'] is not failure
        assert sample['bits'] is None if failure else sample['bits'] == 0
    if failure:
        assert receipt['status'] == 'XR_RBC_OUTPUT_FAILURE' and receipt['host_continued']
        assert receipt['owner_path_matches_original'] and receipt['failure_location'] == [4, 5, 4, 5]
        assert receipt['failure_facts']['domain'] and receipt['failure_facts']['code']
        assert not receipt['failure_facts']['broken_pipe']
    else:
        assert receipt['status'] == 'XR_RBC_OK' and not receipt['host_continued']


def failure_rows(args, folder, source, mode, executable):
    rows = []
    marker = b'HOST_ALIVE\r\n' if os.name == 'nt' else b'HOST_ALIVE\n'
    for order in ('program-first', 'outcome-first'):
        child = folder / order
        child.mkdir()
        sink = child / 'read-only-output'
        sink.write_bytes(b'')
        receipt = child / 'receipt.json'
        with sink.open('rb') as stream:
            p, record = invoke(child, 'embedded-output-failure',
                               [args.driver, source, mode, child / 'generated.c', receipt, 0, 1, order],
                               stdout=stream)
        assert p.stdout is None and p.stderr == marker and sink.read_bytes() == b''
        result = json.loads(receipt.read_bytes())
        check_receipt(result, mode, 1, failure=True)
        record.update(kind='owned-output-failure', release_order=order, performance_sample=False)
        rows.append(record)
    child = folder / 'native-output-failure'
    child.mkdir()
    sink = child / 'read-only-output'
    sink.write_bytes(b'')
    with sink.open('rb') as stream:
        p, record = invoke(child, 'native-output-failure', [executable, 'expect-failure'],
                           expected=74, stdout=stream)
    assert sink.read_bytes() == b'' and b'output write failed at ' in p.stderr
    assert b'source-eval-helper.xr:4:5-4:5 ' in p.stderr
    assert b'AFTER_ENTRY' not in p.stderr and b'EXIT_CALLBACK' not in p.stderr
    assert not (child / 'native-ticks.json').exists()
    record.update(kind='standalone-output-failure', completed_entry=False, performance_sample=False)
    rows.append(record)
    return rows


def main():
    parser = argparse.ArgumentParser()
    for name in ('driver', 'compiler', 'include', 'library', 'source-root', 'helper', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='lir-pipeline-', dir=args.work)).resolve()
    inputs = root / 'inputs'
    inputs.mkdir()
    usage = []
    for label, warmups, runs, order in (
            ('negative-warmups', '-1', '1', 'normal'),
            ('negative-runs', '0', '-1', 'normal'),
            ('empty-warmups', '', '1', 'normal'),
            ('empty-runs', '0', '', 'normal'),
            ('whitespace', ' 1', '1', 'normal'),
            ('nondecimal', '0', '1.0', 'normal'),
            ('u32-overflow', '0', '4294967296', 'normal'),
            ('ull-overflow', '0', '18446744073709551616', 'normal'),
            ('zero-runs', '0', '0', 'normal'),
            ('failure-warmup', '1', '1', 'program-first'),
            ('failure-repeats', '0', '2', 'outcome-first')):
        child = root / 'usage' / label
        child.mkdir(parents=True)
        generated, receipt = child / 'generated.c', child / 'receipt.json'
        process, record = invoke(child, 'invalid-counts',
                                 [args.driver, args.helper, 'raw', generated, receipt, warmups, runs, order],
                                 expected=2)
        assert not process.stdout and not process.stderr
        assert not generated.exists() and not receipt.exists()
        usage.append(dict(case=label, **record))
    cases, sources = [], {}
    for name, needle, variants in (
            ('fib', b'const n = 32', ((10, b'55\n'),)),
            ('integer_loop', b'const n = 100000000', ((0, b'0\n'), (1000, b'46995\n')))):
        original = args.source_root / 'bench/native' / (name + '.xr')
        raw = original.read_bytes()
        assert raw.count(needle) == 1
        sources[name] = {'path': str(original), 'rawSHA256': digest(original)}
        for n, expected in variants:
            path = inputs / (name + '-' + str(n) + '.xr')
            path.write_bytes(raw.replace(needle, b'const n = ' + str(n).encode('ascii')))
            cases.append((path.stem, path, expected))
    cases.append(('helper', args.helper.resolve(), b'10\n10\n-10\n-10\n'))
    time_library = args.source_root / 'stdlib/time/time.xr'
    time_bytes = time_library.read_bytes()
    time_entry = inputs / 'time.xr'
    time_entry.write_bytes(b'import time\nprint(1)\n')
    cases.append(('time', time_entry, b'1\n'))
    sources['time'] = {'path': str(time_library), 'rawSHA256': digest(time_library)}
    (inputs / 'base.xr').write_bytes(b'const n=10\nexport fn read()->i64{return n}\n')
    (inputs / 'left.xr').write_bytes(b'import "./base" as base\nconst own=1\n'
        b'export fn read()->i64{return base.read()+own}\n')
    (inputs / 'right.xr').write_bytes(b'import "./base" as base\nconst own=2\n'
        b'export fn read()->i64{return base.read()+own}\n')
    diamond = inputs / 'diamond.xr'
    diamond.write_bytes(b'import "./left" as left\nimport "./right" as right\n'
        b'print(left.read()+right.read())\n')
    cases.append(('diamond', diamond, b'23\n'))
    compiler = Path(shutil.which(str(args.compiler)) or args.compiler).resolve()
    assert compiler.is_file()
    identity = {'compiler': str(compiler), 'compilerSHA256': digest(compiler),
                'driverSHA256': digest(args.driver), 'library': str(args.library),
                'librarySHA256': digest(args.library),
                'headerSHA256': digest(args.include / 'xray_native_runtime.h'),
                'asan': args.asan, 'msvcRuntime': args.msvc_runtime,
                'sourceInputs': sources, 'formal_performance': False}
    records = []
    for name, source, expected in cases:
        for mode in MODES:
            folder = root / name / mode
            folder.mkdir(parents=True)
            generated, metrics = folder / 'generated.c', folder / 'receipt.json'
            p, vm_record = invoke(folder, 'source-pipeline',
                                  [args.driver, source, mode, generated, metrics, 0, 2, 'normal', args.source_root / 'stdlib'])
            assert p.stdout == expected * 2 and not p.stderr
            receipt = json.loads(metrics.read_bytes())
            check_receipt(receipt, mode, 2)
            selected = receipt['snapshots'][-1]['ROOT_CFG_CLOSURE']
            if name.startswith('fib'):
                assert selected['self_calls'] > 0
            if name == 'helper':
                if mode == 'inline':
                    assert selected['ordinary_calls'] > 0
                if mode == 'sccp-inline-inline-sccp':
                    assert selected['ordinary_calls'] == 0
            text = generated.read_text(encoding='ascii')
            assert '({' not in text and '#include "xray_native_runtime.h"' in text
            actual_functions = re.findall(r'^static [^\n]+ xrl_f[0-9]+\([^;\n]*\) \{', text, re.M)
            assert len(actual_functions) == receipt['snapshots'][-1]['C_SELECTED_FUNCTION_ALL_BLOCKS']['functions']
            native_adapter = folder / 'adapter.c'
            adapter(native_adapter, generated, 2)
            executable = folder / ('native.exe' if os.name == 'nt' else 'native')
            _, host_compile = invoke(folder, 'host-compile-link', compile_command(args, native_adapter, executable))
            native, native_record = invoke(folder, 'native-complete-entry', [executable])
            assert native.stdout == expected * 2 and not native.stderr
            native_receipt = json.loads((folder / 'native-ticks.json').read_bytes())
            assert native_receipt['clock_frequency'] > 0 and native_receipt['formal_performance'] is False
            assert len(native_receipt['samples']) == 2
            assert all(s['completed_entry'] and s['result'] == 0 for s in native_receipt['samples'])
            row = {'case': name, 'mode': mode, 'expectedHex': expected.hex(),
                   'sourceSHA256': digest(source), 'generatedSHA256': digest(generated),
                   'executableSHA256': digest(executable),
                   'vm': vm_record, 'native': native_record, 'compile_link': host_compile,
                   'actualEmittedCFunctionDefinitions': len(actual_functions),
                   'root': selected, 'formal_performance': False}
            if name == 'helper':
                row['outputFailures'] = failure_rows(args, folder, source, mode, executable)
            records.append(row)
    assert time_library.read_bytes() == time_bytes
    assert time_entry.read_bytes() == b'import time\nprint(1)\n'
    for source in sources.values():
        assert digest(Path(source['path'])) == source['rawSHA256']
    identity.update(status='PASS_READINESS_NOT_FORMAL_PERFORMANCE', cases=len(records),
                    modes=list(MODES), results=records, usageCases=len(usage), usageResults=usage,
                    fullOriginalWorkloads='NOT_RUN_QUICK_GATE')
    (root / 'result.json').write_text(json.dumps(identity, indent=2) + '\n')
    print('source pipeline readiness PASS: ' + str(root), flush=True)


if __name__ == '__main__':
    main()
