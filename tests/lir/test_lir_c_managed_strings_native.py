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

STATE_RECOVERY = r'''
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include <stdio.h>
#include <stdlib.h>
#include "GENERATED"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
typedef struct StateRun { XrtModuleState *state; int64_t result; unsigned after; } StateRun;
static volatile unsigned scalar_mode = SCALAR_MODE, panic_mode = PANIC_MODE;
static void execute(void *context) {
    StateRun *call = context;
    xrl_invoke(&call->result, call->state);
    if (scalar_mode) CHECK(xrt_native_values_prepared(xrt_value_root_active()));
    ++call->after;
}
int main(void) {
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(xrt_standalone_source_map.locations,
        xrt_standalone_source_map.count, &locations) == XRT_LOCATION_OK);
    unsigned limits = 0, ooms = 0, successes = 0, panics = 0;
    for (unsigned mode = 0; mode < 2; ++mode) for (unsigned point = 0; point < (mode ? 40u : 17u); ++point) {
        XrtValueRoot owner = {0}; XrtExecutionFailure failure = {0}; StateRun call = {0};
        XrtModuleLayout *layout = NULL; XrtBillingOwner *billing = NULL;
        owner.byte_limit = mode ? 0 : UINT64_C(1) << point;
        XrtManagedStatus status = xrt_native_values_prepare(&owner, &billing);
        bool kept_billing = status == XRT_MANAGED_OK;
        if (kept_billing) CHECK(xrt_billing_owner_retain(billing));
        if (status == XRT_MANAGED_OK)
            status = xrt_module_layout_new(billing, &xrl_state_spec, (XrtManagedLease){0}, &layout);
        if (status == XRT_MANAGED_OK) status = xrt_module_state_new(layout, billing, &call.state);
        if (status == XRT_MANAGED_OK) {
            if (mode) xrt_billing_owner_fail_after(billing, point);
            XrtExecutionStatus result = xrt_run_embedded_owned(locations, &failure, execute, &call, &owner);
            CHECK(xrt_value_root_empty(&owner));
            if (result == XRT_EXECUTION_FAILURE) {
                CHECK(!call.after);
                if (failure.kind == XRT_FAILURE_VALUE) {
                    CHECK(failure.detail.value.status == (mode ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_MEMORY_LIMIT));
                    if (mode) ++ooms; else ++limits;
                } else {
                    CHECK(panic_mode && failure.kind == XRT_FAILURE_TYPED_PANIC && failure.detail.panic.code == 420);
                    ++panics;
                }
            } else CHECK(!panic_mode && result == XRT_EXECUTION_COMPLETED && call.after == 1 && ++successes);
            if (scalar_mode && mode) CHECK(result == XRT_EXECUTION_COMPLETED && call.after == 1);
        } else {
            CHECK(!mode && status == XRT_MANAGED_LIMIT); ++limits;
        }
        xrt_module_state_drop(&call.state); xrt_module_layout_drop(&layout);
        xrt_value_root_drain(&owner); CHECK(xrt_value_root_empty(&owner));
        if (kept_billing) {
            CHECK(xrt_billing_owner_stats(billing).blocks == 1);
            xrt_billing_owner_drop(billing);
        }
    }
    CHECK(limits && (scalar_mode || ooms) && (panic_mode ? panics : successes));
    xrt_locations_release(locations); return 0;
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
    flags, gnu_flags, link_flags = [], [], []
    if args.kind == 'msvc':
        # Only MSVC targets carry a runtime library property; GNU providers
        # receive an empty generator value.
        crt = {'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd',
               'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd'}[args.msvc_runtime]
        flags = ['/nologo', '/std:c11', '/W4', '/WX', '/utf-8', crt]
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
            publications = re.findall(
                r'XrtPlacePublication publication = \{(xrl_publish_\d+_\d+_\d+), &context\};', emitted)
            assert publications and all('static void ' + callback + '(void *context)' in emitted
                                        for callback in publications)
            assert 'xrt_place_edge_transfer(owners, ' in emitted
            assert 'xrl_edge_' in emitted and 'xrl_return_' in emitted
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
    state_cases = [
        ('managed-state-scalar', True, False, b'17\n42\n47\n'),
        ('managed-state-string', False, False, 'seed\né\0next\né\0next\né\0next!!!\nseed\n'.encode()),
        ('managed-state-panic', False, True, b''),
    ]
    for name, scalar, panic, expected_state in state_cases:
        generated = root / (name + '-generated.c')
        run([args.driver.resolve(), programs / (name + '.xr'), generated], root)
        emitted = generated.read_text(encoding='ascii')
        assert re.search(r'static const XrtValueImage xrl_value_image\s*=', emitted)
        assert re.search(r'static const XrtModuleLayoutSpec xrl_state_spec\s*=\s*\{\s*&xrl_value_image,', emitted)
        assert 'XrtModuleTypeSpec' not in emitted
        if scalar:
            assert 'xrt_native_managed_root(' not in emitted and 'xrt_managed_frame_enter' not in emitted
        else:
            assert 'xrt_module_external_owner(' in emitted and 'xrt_place_copy_replace(' in emitted
            assert 'xrt_string_retain(' not in emitted
        if not panic:
            normal = compile_run(name, '#include ' + json.dumps(generated.as_posix()) +
                                 '\nint main(void) { return xray_lir_entry() != 0; }\n')
            assert (normal.stdout, normal.stderr) == (expected_state, b'')
        observer = STATE_RECOVERY.replace('GENERATED', generated.name).replace(
            'SCALAR_MODE', str(int(scalar))).replace('PANIC_MODE', str(int(panic)))
        assert not compile_run(name + '-recovery', observer, private=True).stderr
    generated = root / 'mixed-state-generated.c'
    run([args.faults.resolve(), '--managed-state', generated], root)
    assert not compile_run('mixed-state', '#include ' + json.dumps(generated.as_posix()) +
                           '\nint main(void) { return xray_lir_entry() != 3311; }\n').stdout
    generated = root / 'record-state-first-read-generated.c'
    first_read = Path(__file__).resolve().parents[1] / 'fixtures/lir_cli/array_lower/record_state_first_read.xr'
    run([args.driver.resolve(), first_read, generated], root)
    observer = (programs / 'record-state-first-read.c').read_text(encoding='ascii').replace('GENERATED', generated.name)
    observed = compile_run('record-state-first-read', observer, private=True)
    assert (observed.stdout, observed.stderr) == (b'', b'')
    for name, text, expected in [
        ('format', 'print("n=${42}")\n', b'n=42\n'),
        ('array', 'var xs=Array<i64>(2,0)\nprint(len(xs))\n', b'2\n'),
        ('cell', 'fn work(){var text="local"\nprint(len(text))}\nwork()\n', b'5\n'),
    ]:
        source, generated = root / (name + '.xr'), root / (name + '-generated.c')
        source.write_text(text, encoding='utf-8')
        run([args.driver.resolve(), source, generated], root)
        normal = compile_run(name, '#include ' + json.dumps(generated.as_posix()) +
                             '\nint main(void) { (void)xray_lir_entry(); return 0; }\n')
        assert (normal.stdout, normal.stderr) == (expected, b'')
    print('ordinary C String workers: real sources, borrowed calls, reused landing, selected edges, quota/panic drain and retired producers PASS', flush=True)


if __name__ == '__main__':
    main()
