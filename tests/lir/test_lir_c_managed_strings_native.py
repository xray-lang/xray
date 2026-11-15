"""Execute the ordinary C emitter's real owned String workers and recovery."""
from pathlib import Path
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lib'))
from xraytest.toolchain import archive_link_arguments


def run(argv, work, success=True):
    print('$ ' + json.dumps([str(item) for item in argv]), flush=True)
    result = subprocess.run([str(item) for item in argv], cwd=work, capture_output=True, timeout=30)
    for data in (result.stdout, result.stderr):
        if data:
            print(data.decode('utf-8', errors='replace'), end='', flush=True)
    if success and result.returncode:
        raise RuntimeError(f'command exited {result.returncode}')
    return result


RECOVERY = r'''
#include "runtime/native/xnative_execution.h"
#include "xray_native_values.h"
#include <stdlib.h>
#include <stdio.h>
static size_t constructions;
static void watched_create(XrtManagedFrame *frame, size_t slot, const char *bytes, size_t length, uint32_t source) {
    xrt_native_string_create(frame, slot, bytes, length, source); ++constructions;
}
static void watched_concat(XrtManagedFrame *frame, size_t slot, const XrtManagedHeader *left,
    const XrtManagedHeader *right, uint32_t source) {
    xrt_native_string_concat_create(frame, slot, left, right, source); ++constructions;
}
#define xrt_native_string_create watched_create
#define xrt_native_string_concat_create watched_concat
#include "GENERATED"
#undef xrt_native_string_create
#undef xrt_native_string_concat_create
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
typedef struct Run { int64_t result; unsigned after; } Run;
static void execute(void *context) {
    Run *run = context; xrl_invoke(&run->result); ++run->after;
}
int main(void) {
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(xrt_standalone_source_map.locations,
        xrt_standalone_source_map.count, &locations) == XRT_LOCATION_OK);
    XrtValueRoot owner = {0}; XrtExecutionFailure failure = {0}; Run call = {0};
    unsigned failures = 0, late_failures = 0, successes = 0;
    (void)&watched_create; (void)&watched_concat;
    for (uint64_t limit = 1; limit <= UINT64_C(65536); limit *= 2) {
        owner.byte_limit = limit; call.after = 0; constructions = 0;
        XrtExecutionStatus status = xrt_run_embedded_owned(locations, &failure, execute, &call, &owner);
        CHECK(xrt_value_root_empty(&owner) && owner.byte_limit == limit);
        if (status == XRT_EXECUTION_FAILURE) {
            CHECK(!call.after);
            if (failure.kind == XRT_FAILURE_VALUE) {
                CHECK(failure.detail.value.status == XR_XIR_VALUE_MEMORY_LIMIT);
                ++failures; late_failures += constructions != 0;
            } else CHECK(PANIC && failure.kind == XRT_FAILURE_TYPED_PANIC && failure.detail.panic.code == 420);
        } else CHECK(!PANIC && status == XRT_EXECUTION_COMPLETED && call.after == 1 && ++successes);
    }
    CHECK(failures && (PANIC || (late_failures && successes)));
    owner.byte_limit = 0; call.after = 0;
    CHECK(xrt_run_embedded_owned(locations, &failure, execute, &call, &owner) ==
        (PANIC ? XRT_EXECUTION_FAILURE : XRT_EXECUTION_COMPLETED));
    CHECK(xrt_value_root_empty(&owner) && call.after == (PANIC ? 0u : 1u));
    if (PANIC) CHECK(failure.kind == XRT_FAILURE_TYPED_PANIC && failure.detail.panic.code == 420);
    xrt_locations_release(locations); return 0;
}
'''

ESCAPED = r'''
#include "GENERATED"
#include <string.h>
int main(void) {
    const unsigned char expected[] = {'A', 0, 0xc3, 0xa9, 'A', 0, 0xc3, 0xa9};
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        XrtManagedHeader *owner = xray_lir_entry();
        const XrtStringBacking *value = (const XrtStringBacking *)owner;
        if (!owner || atomic_load(&owner->references) != 1 || value->length != sizeof(expected) ||
            value->runes != 6 || memcmp(value->bytes, expected, sizeof(expected))) return 1;
        xrt_managed_drop(owner);
    }
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', required=True, type=Path)
    parser.add_argument('--faults', required=True, type=Path)
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=['msvc', 'gnu'])
    parser.add_argument('--include', required=True, type=Path)
    parser.add_argument('--library', required=True, nargs='+')
    parser.add_argument('--msvc-runtime', default='MultiThreadedDLL')
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--asan', action='store_true')
    parser.add_argument('--ubsan', action='store_true')
    parser.add_argument('--ubsan-runtime', type=Path)
    parser.add_argument('--lld-no-tail-merge', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='managed-strings-', dir=args.work)).resolve()
    programs = Path(__file__).resolve().parent / 'programs'
    crt = {'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd',
           'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd'}[args.msvc_runtime]
    flags = ['/nologo', '/std:c11', '/W4', '/WX', '/utf-8', crt]
    gnu_flags, link_flags = [], []
    if args.kind == 'msvc':
        if args.compiler.stem.lower() != 'clang-cl':
            flags.append('/experimental:c11atomics')
        if args.asan:
            flags += ['/fsanitize=address', '/Zi']
            link_flags.append('/INCREMENTAL:NO')
        if args.ubsan:
            assert args.compiler.stem.lower() == 'clang-cl'
            flags += ['/clang:-fsanitize=undefined', '/clang:-fno-sanitize=function']
        if args.lld_no_tail_merge:
            link_flags.append('/OPT:NOLLDTAILMERGE')
        if args.ubsan_runtime:
            link_flags.append(str(args.ubsan_runtime))
    else:
        enabled = [name for name, on in [('address', args.asan), ('undefined', args.ubsan)] if on]
        if enabled:
            gnu_flags += ['-fsanitize=' + ','.join(enabled), '-fno-omit-frame-pointer', '-g']
        if args.ubsan:
            gnu_flags.append('-fno-sanitize=function')

    def compile_run(name, text, private=False):
        source = root / (name + '.c'); source.write_text(text, encoding='ascii')
        product = root / (name + ('.exe' if os.name == 'nt' else ''))
        if args.kind == 'msvc':
            command = [args.compiler, *flags, '/I' + str(args.include)]
            if private:
                command += ['/I' + str(args.include.parent / 'src'), '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_WARNINGS']
            command += [source, '/Fe' + str(product), '/Fo' + str(root / (name + '.obj')),
                        '/link', *archive_link_arguments(args.library, driver=args.kind), *link_flags]
        else:
            command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                       *gnu_flags, '-I' + str(args.include)]
            if private:
                command += ['-I' + str(args.include.parent / 'src')]
            command += [source, *archive_link_arguments(args.library, driver=args.kind), '-pthread', '-o', product]
        run(command, root)
        return run([product], root)

    expected = 'value!\né\0big\nsmall\n\né\0big\né\0é\0\n3\n0\n16384\n'.encode()
    for name, panic in [('managed-string-values', False), ('managed-string-panic', True)]:
        generated = root / (name + '-generated.c')
        run([args.driver.resolve(), programs / (name + '.xr'), generated], root)
        emitted = generated.read_text(encoding='ascii')
        assert 'xrt_native_string_create(' in emitted and 'XrtString *' not in emitted
        assert 'xrt_string_retain(' not in emitted and 'xrt_string_root_take(' not in emitted
        if not panic:
            assert 'xrl_scalars_' in emitted and 'xrl_edge_' in emitted and 'xrl_return_' in emitted
            normal = compile_run('normal', '#include ' + json.dumps(generated.as_posix()) +
                                 '\nint main(void) { return xray_lir_entry() != 0; }\n')
            assert (normal.stdout, normal.stderr) == (expected, b'')
        recovery = re.sub(r'\bPANIC\b', 'is_panic', RECOVERY.replace('GENERATED', generated.name))
        recovery = recovery.replace('int main(void) {',
            'int main(void) {\n    volatile unsigned is_panic = ' + str(int(panic)) + ';')
        recovered = compile_run(name + '-recovery', recovery, private=True)
        assert not recovered.stderr
    generated = root / 'escaped-generated.c'
    run([args.faults.resolve(), '--managed-string', generated], root)
    assert not compile_run('escaped', ESCAPED.replace('GENERATED', generated.name)).stdout
    for name, text in [
        ('state', 'var text="kept"\nfn read()->string{return text}\nprint(read())\n'),
        ('format', 'print("n=${42}")\n'),
        ('cell', 'fn work(){var text="local"\nprint(len(text))}\nwork()\n'),
        ('array', 'var xs=Array<i64>(2,0)\nprint(len(xs))\n'),
    ]:
        source, generated = root / (name + '.xr'), root / (name + '.c')
        source.write_text(text, encoding='utf-8')
        rejected = run([args.driver.resolve(), source, generated], root, success=False)
        assert rejected.returncode == 1 and b'C emission:' in rejected.stderr and not generated.exists()
    print('ordinary C String workers: real sources, borrowed calls, reused landing, selected edges, quota/panic drain and retired producers PASS', flush=True)


if __name__ == '__main__':
    main()
