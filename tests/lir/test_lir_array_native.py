"""Compile and execute real Array C emitted after its LIR owner retires."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xraytest.toolchain import archive_link_arguments
from xraytest.artifacts import complete_native_artifacts
from lir_slice_native_observer import slice_boundary_observer_source
from lir_header_edge_native_observer import header_edge_observer_source
from lir_array_forward_native_observer import array_forward_observer_source
from lir_array_local_native_observer import array_local_observer_source

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cli"))
from owned_case_workers import SpawnGate, run_ordered
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from xray_process import RecoveryRequired, run_child


def check_exit(result, expected_exit):
    if expected_exit is not None and result.returncode != expected_exit:
        raise RuntimeError((result.args, result.returncode, result.stdout, result.stderr))
    return result


def run_serial(command, cwd, expected_exit=0):
    command = [str(value) for value in command]
    print('$ ' + json.dumps(command), flush=True)
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=30)
    return check_exit(result, expected_exit)


def supported_parallel_setup(args):
    if (os.name != 'nt' or args.kind != 'msvc' or args.asan
            or args.compiler.name.lower() != 'cl.exe'
            or any(os.environ.get(name) for name in ('CL', '_CL_', 'LINK', '_LINK_'))):
        return False
    if not all(path.is_absolute() for path in (args.compiler, args.driver, args.include)):
        return False
    # The serial runner shares one cwd. Moving a case must not change the
    # directories its compiler searches for headers or libraries.
    for setting in ('INCLUDE', 'EXTERNAL_INCLUDE', 'LIB', 'LIBPATH'):
        for entry in os.environ.get(setting, '').split(';'):
            if entry and not Path(entry.strip()).is_absolute():
                return False
    if any(not path.is_absolute() and path not in (Path('advapi32.lib'), Path('userenv.lib'))
           for path in args.library):
        return False
    parents = args.driver.resolve().parents
    if len(parents) < 3:
        return False
    build = parents[2]
    try:
        settings = set((build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines())
    except (OSError, UnicodeError):
        return False
    compiler = next((line.split('=', 1)[1] for line in settings
                     if line.startswith(('CMAKE_C_COMPILER:FILEPATH=', 'CMAKE_C_COMPILER:STRING='))),
                    None)
    return (compiler is not None and Path(compiler).resolve() == args.compiler.resolve()
            and 'CMAKE_BUILD_TYPE:STRING=Release' in settings
            and all(f'ENABLE_{name}:BOOL=OFF' in settings
                    for name in ('ASAN', 'UBSAN', 'TSAN', 'MSAN', 'SANITIZERS')))


def run_parallel_cases(args, work, cases):
    gate = SpawnGate()
    progress = threading.Lock()
    finished = {}
    published = 0

    def execute(index, case):
        nonlocal published
        name, answer = case
        folder = work / name
        folder.mkdir()
        trace = []

        def run(command, cwd, expected_exit=0):
            arguments = [str(value) for value in command]
            base = cwd / f'command-{len(trace) + 1}'
            line = json.dumps(arguments)
            trace.append('$ ' + line)
            (base.with_suffix('.json')).write_text(line + '\n', encoding='utf-8')
            out, err = base.with_suffix('.stdout'), base.with_suffix('.stderr')
            with out.open('xb') as stdout, err.open('xb') as stderr:
                status = run_child(arguments, cwd=cwd, stdout=stdout, stderr=stderr,
                                   timeout=30, spawn_guard=gate.launch)
            (base.with_suffix('.exit')).write_text(str(status), encoding='ascii')
            result = subprocess.CompletedProcess(arguments, status, out.read_bytes(), err.read_bytes())
            return check_exit(result, expected_exit)

        result = run_case(args, name, answer, folder, run)
        with progress:
            finished[index] = trace
            while published in finished:
                lines = finished.pop(published)
                print('\n'.join(lines), flush=True)
                published += 1
        return name, result

    return run_ordered(cases, execute, 2, gate, RecoveryRequired)


def adapter(name, generated):
    prefix = '#include <inttypes.h>\n#include <stdio.h>\n#include <string.h>\n' + f'#include "{generated.name}"\n'
    if name == 'state-layout':
        return prefix + '''int main(void) {
    const XrtModuleLayoutSpec *spec = &xrl_state_spec;
    const XrtValueImage *image = spec->values;
    if (!image || !image->types || !spec->slots || spec->slot_count != 1 || spec->module_count != 1)
        return 1;
    const XrtModuleSlotSpec *slot = &spec->slots[0];
    if (!slot->type || slot->type > image->type_count || slot->module || slot->offset ||
        slot->flags != XRT_MODULE_SLOT_MUTABLE || slot->size != sizeof(XrtManagedHeader *) ||
        slot->alignment != _Alignof(XrtManagedHeader *) || spec->payload_size != slot->size ||
        spec->payload_alignment != slot->alignment) return 1;
    const XrtValueImageType *type = &image->types[slot->type - 1];
    if (type->kind != XRT_VALUE_ARRAY_I64 || type->size != slot->size || type->alignment != slot->alignment ||
        type->owned_count != 1 || !image->owned_offsets || type->first_owned >= image->owned_count ||
        image->owned_offsets[type->first_owned] != 0) return 1;
    printf("%" PRId64 "\\n", xray_lir_entry());
    return 0;
}
'''
    if not name.startswith('return-'):
        return prefix + 'int main(void) { printf("%" PRId64 "\\n", xray_lir_entry()); return 0; }\n'
    kind = ('XRT_BACKING_ARRAY_STRING' if name.endswith('string') else
            'XRT_BACKING_ARRAY_U8' if name.endswith('u8') else
            'XRT_BACKING_ARRAY_U64' if name.endswith('u64') else 'XRT_BACKING_ARRAY_I64')
    observe = ('p->value = (int64_t)item.value.string->runes;' if name.endswith('string') else
               'p->value = item.value.u8;' if name.endswith('u8') else
               'memcpy(&p->value, &item.value.u64, sizeof(p->value));' if name.endswith('u64') else
               'p->value = item.value.i64;')
    return prefix + f'''typedef struct Probe {{ XrtManagedHeader *array; int64_t value; }} Probe;
static void probe(void *context) {{
    Probe *p = context;
    XrtArrayItem item = {{0}};
    xrt_native_array_get(p->array, {kind}, 0, &item, 0);
    {observe}
    p->value += (int64_t)((const XrtArrayBacking *)p->array)->length;
}}
int main(void) {{
    Probe p = {{xray_lir_entry(), 0}};
    XrtSourceMap map = {{NULL, 0}};
    xrt_run_standalone(&map, probe, &p);
    printf("%" PRId64 "\\n", p.value);
    xrt_managed_drop(p.array);
    return 0;
}}
'''


def native_observer(args, generated, client, binary, command, work, run, source_text, suffix):
    source = work / (generated.stem + suffix + '.c')
    executable = work / (generated.stem + suffix + ('.exe' if os.name == 'nt' else ''))
    source.write_text(source_text, encoding='ascii')
    compile_command = []
    for value in command:
        if str(value) == str(client):
            compile_command.append(source)
        elif str(value) == str(binary):
            compile_command.append(executable)
        elif str(value) == '/Fe:' + str(binary):
            compile_command.append('/Fe:' + str(executable))
        else:
            compile_command.append(value)
    internal = args.include.parent / 'src'
    assert internal.is_dir(), internal
    compile_command.insert(1, ('/I' if args.kind == 'msvc' else '-I') + str(internal))
    if args.kind == 'msvc':
        compile_command[1:1] = ['/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_WARNINGS']
    (work / (generated.stem + suffix + '-command.json')).write_text(
        json.dumps(list(map(str, compile_command)), indent=2), encoding='utf-8')
    run(compile_command, work)
    result = run([executable], work, None)
    (work / (generated.stem + suffix + '.stdout')).write_bytes(result.stdout)
    (work / (generated.stem + suffix + '.stderr')).write_bytes(result.stderr)
    assert result.returncode == 0 and not result.stderr, (source, result.returncode, result.stdout, result.stderr)
    cases = [json.loads(line) for line in result.stdout.splitlines()]
    return {'source': str(source), 'sourceSHA256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'cases': cases, 'guestRefund': 'exact live-owner control baseline',
            'finalOwnerFree': 'existing billing API and sanitizer coverage; no post-free statistics', 'result': 'PASS'}


def slice_observer(args, generated, client, binary, command, work, run):
    record = native_observer(args, generated, client, binary, command, work, run,
                             slice_boundary_observer_source(generated), '-ownership-observer')
    cases = record['cases']
    assert [row['mode'] for row in cases] == list(range(10)), cases
    assert all(row['bytes'] == row['controlBytes'] and row['blocks'] == row['controlBlocks']
               for row in cases), cases
    return record


def header_observer(args, generated, client, binary, command, work, run):
    record = native_observer(args, generated, client, binary, command, work, run,
                             header_edge_observer_source(generated), '-header-edge-observer')
    cases = record['cases']
    assert [row['mode'] for row in cases] == list(range(9)), cases
    assert all(row['fallback'] == (row['mode'] != 0) and row['allocationDelta'] == 0 and
               row['bytes'] == row['controlBytes'] and row['blocks'] == row['controlBlocks']
               for row in cases), cases
    return record


def forward_observer(args, generated, client, binary, command, work, run):
    record = native_observer(args, generated, client, binary, command, work, run,
                             array_forward_observer_source(generated), '-array-forward-observer')
    cases = record['cases']
    assert [(row['pair'], row['mode']) for row in cases] == [(pair, mode) for pair in range(2) for mode in range(12)]
    assert all(row['attempts'] == 1 and row['fast'] == (row['mode'] in (0, 1)) and
               row['bytes'] == row['controlBytes'] and row['blocks'] == row['controlBlocks'] for row in cases), cases
    return record


def local_observer(args, generated, client, binary, command, work, run):
    record = native_observer(args, generated, client, binary, command, work, run,
                             array_local_observer_source(generated), '-array-local-observer')
    cases = record['cases']
    assert [(row['operation'], row['mode']) for row in cases] == [(op, mode) for op in range(2) for mode in range(12)]
    assert all(row['fast'] == (row['mode'] == 0) and row['bytes'] == row['controlBytes'] and
               row['blocks'] == row['controlBlocks'] for row in cases), cases
    return record


def run_case(args, name, answer, work, run):
    generated, client = work / (name + '.c'), work / (name + '-client.c')
    binary = work / (name + ('.exe' if os.name == 'nt' else ''))
    run([args.driver, name, generated], work)
    text = generated.read_text(encoding='ascii')
    assert '({' not in text
    if name in ('i64', 'u8', 'u64', 'phi-i64', 'phi-u8', 'phi-u64'):
        reads = re.findall(r'if \(array && array->length <= array->capacity && index >= 0 && '
                           r'\(uint64_t\)index < array->length\) \{(.*?)\n    \} else \{', text, re.DOTALL)
        assert reads and all('xrt_' not in body for body in reads), name
    if name in ('string', 'phi-string'):
        assert 'xrt_native_array_string_read_copy(' in text, name
    if name == 'array-spills-string':
        assert text.count('xrt_native_array_string_read_copy(') == 300
        assert 'XrtArrayItem item' not in text
    if name in ('string', 'scalar-edge-swap'):
        assert 'xray_native_array_local.h' not in text and 'xrl_array_ready' not in text, name
        assert 'XrlArrayLocals_' not in text and 'xrl_array_local_prepare_' not in text, name
    if name.startswith('map-literal-'):
        assert 'xrt_native_map_literal_create(' in text
        assert 'static const XrtMapLiteral xrl_map_literal_2' in text
        assert 'xrl_map_literal_1' not in text
        assert not re.search(r'\b(?:XrtMap|XrtString)\b', text)
    if name == 'large':
        assert ', 300}, UINT32_C(' in text and '(int64_t[]){' in text
    client.write_text(adapter(name, generated), encoding='ascii')
    if args.kind == 'msvc':
        crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
               'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
        command = [args.compiler, '/nologo', '/std:c11', '/utf-8', '/O2', '/W4', '/WX', crt,
                   '/I' + str(args.include), client, *archive_link_arguments(args.library, driver=args.kind), '/Fe:' + str(binary)]
        if args.compiler.name.lower() in ('cl', 'cl.exe'):
            command.insert(1, '/experimental:c11atomics')
        if args.asan:
            command += ['/fsanitize=address', '/Zi']
            if 'clang' in args.compiler.name.lower(): command += ['/clang:-fsanitize=undefined']
            command += ['/link', '/INCREMENTAL:NO']
    else:
        command = [args.compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-I' + str(args.include), client, *archive_link_arguments(args.library, driver=args.kind), '-lm', '-pthread', '-o', binary]
        command += ['-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections']
        if args.asan:
            debug = '-gline-tables-only' if args.line_tables_only else '-g'
            command += ['-fsanitize=address,undefined', debug]
    run(command, work)
    result = run([binary], work, 1 if answer is None else 0)
    if answer is None:
        if name.startswith('map-literal-'):
            sites = (b'entries at 3:12 and 5:16' if name == 'map-literal-unknown-duplicate' else
                     'entries at map-\u00e9.xr:3:12 and map-\u00e9.xr:5:16'.encode())
            assert not result.stdout and b'E0434' in result.stderr and sites in result.stderr, (
                name, result.stdout, result.stderr)
            return {'stdout': result.stdout.decode(), 'exit': result.returncode,
                    'stderr': result.stderr.decode(),
                    'sourceSHA256': hashlib.sha256(generated.read_bytes()).hexdigest()}
        position = (b'unknown source' if name.startswith('slice-state-unready-') else
                    b'array-c.xr:3:1' if name.startswith('repeat-negative') else
                    b'array-c.xr:37:9' if name.endswith('read') else b'array-c.xr:41:7')
        assert not result.stdout and position in result.stderr, (name, result.stdout, result.stderr)
        if name.startswith('slice-state-unready-'):
            assert result.stderr.startswith(b'XR_RUN_6002:'), (name, result.stderr)
    else:
        expected_stdout = (str(answer) + os.linesep).encode()
        assert result.stdout == expected_stdout and not result.stderr, (name, result.stdout, result.stderr)
    record = {'stdout': result.stdout.decode(), 'exit': result.returncode,
              'stderr': result.stderr.decode(),
              'sourceSHA256': hashlib.sha256(generated.read_bytes()).hexdigest()}
    if name.startswith('slice-boundary-'):
        record['ownershipObserver'] = slice_observer(
            args, generated, client, binary, command, work, run)
    if name == 'phi-i64':
        record['headerEdgeObserver'] = header_observer(
            args, generated, client, binary, command, work, run)
    if name == 'repeat-i64':
        record['arrayForwardObserver'] = forward_observer(
            args, generated, client, binary, command, work, run)
    if name == 'i64':
        record['arrayLocalObserver'] = local_observer(
            args, generated, client, binary, command, work, run)
    return record


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
    parser.add_argument('--line-tables-only', action='store_true')
    parser.add_argument('--jobs', type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    parallel = args.jobs == 2 and supported_parallel_setup(args)
    if args.jobs == 2 and not parallel:
        print('Using the serial Array runner because its MSVC setup may depend on the shared directory.',
              file=sys.stderr, flush=True)
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='array-native-', dir=args.work))
    expected = {'nullable-array-u8': 231, 'nullable-array-i64': -91,
                'scalar-edge-swap': 2011, 'scalar-edge-mixed': 2011, 'scalar-edge-pod': 2018,
                'slice-callback-direct-i64': 4, 'slice-callback-direct-u8': 4,
                'slice-callback-indirect-i64': 4, 'slice-callback-indirect-u8': 4,
                'slice-proof-branch-i64': 40, 'slice-proof-branch-u8': 40,
                'slice-proof-live-i64': 43, 'slice-proof-live-u8': 43,
                'slice-proof-bind-i64': 43, 'slice-proof-bind-u8': 43,
                'slice-proof-loop-i64': 43, 'slice-proof-loop-u8': 43,
                'slice-proof-call-before-i64': 43, 'slice-proof-call-before-u8': 43,
                'slice-proof-call-after-i64': 43, 'slice-proof-call-after-u8': 43,
                'string-bindings': 14, 'string-ref-bindings': 2, 'frame-scalar-cells': 73, 'tuple-frame-capture': 42,
                'set-i64-local': 1, 'set-i64-state': 1,
                'set-formal-cell': 6, 'set-formal-ref': 6, 'set-formal-state': 6,
                'set-header-copy': 3, 'set-header-duplicate': 4, 'set-block-cell-loop': 3,
                'slice-boundary-i64': 68, 'slice-boundary-u8': 68,
                'slice-state-const-i64': 25, 'slice-state-const-u8': 25,
                'slice-state-mutable-i64': 25, 'slice-state-mutable-u8': 25,
                'slice-state-initializing-const-i64': 25, 'slice-state-initializing-const-u8': 25,
                'slice-state-initializing-mutable-i64': 25, 'slice-state-initializing-mutable-u8': 25,
                'slice-state-unready-const-i64': None, 'slice-state-unready-const-u8': None,
                'slice-state-unready-mutable-i64': None, 'slice-state-unready-mutable-u8': None,
                'map-formal-new': 1, 'map-formal-closure': 1, 'map-formal-write': 1, 'map-formal-ref': 1,
                'map-formal-state': 1, 'map-formal-construct': 1, 'map-formal-initialize': 1,
                'map-literal-explicit': 42, 'map-literal-spread': 42, 'map-literal-spread-wins': 42,
                'map-literal-empty': 0,
                'map-literal-duplicate': None, 'map-literal-unknown-duplicate': None,
                'callable-nonentry-frame': 42, 'callable-loop-before-frame': 42,
                'cell-i64-frame': 42, 'cell-i64-owned': 42, 'cell-map-frame': 42, 'cell-map-owned': 42,
                'callable-weaken': 42, 'i64': 1202, 'string': 511, 'large': 1500, 'state-i64': 1202,
                'state-string': 511, 'state-layout': 9, 'return-i64': 13, 'return-string': 7,
                'array-spills': 44850, 'array-spills-string': 1500,
                'phi-i64': 1202, 'phi-string': 511,
                'state-uninitialized-read': None, 'state-uninitialized-write': None, 'state-uninitialized-ref-write': None,
                'repeat-i64': 19, 'repeat-string': 10, 'repeat-empty-i64': 0, 'repeat-empty-string': 0,
                'repeat-negative-i64': None, 'repeat-negative-string': None,
                'u8': 264, 'u64': 8, 'phi-u8': 262, 'phi-u64': 6, 'state-u8': 264, 'state-u64': 8,
                'return-u8': 258, 'return-u64': 2, 'unsigned-spills-u8': 33586, 'unsigned-spills-u64': -45150}
    record = {'compiler': str(args.compiler), 'cases': {}, 'libraries': {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in args.library if p.is_file()}}
    cases = tuple(expected.items())
    if parallel:
        record['cases'].update(run_parallel_cases(args, work, cases))
    else:
        for name, answer in cases:
            record['cases'][name] = run_case(args, name, answer, work, run_serial)
    (work / 'result.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    complete_native_artifacts(work)
    print(json.dumps(record), flush=True)


if __name__ == '__main__':
    main()
