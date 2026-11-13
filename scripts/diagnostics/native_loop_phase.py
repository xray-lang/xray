"""Diagnostic-only CPU phases using an unchanged original native object."""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
if os.name == 'posix':
    import resource

sys.dont_write_bytecode = True
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--product-ref', required=True)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--cc', type=Path, required=True)
parser.add_argument('--alignment', type=int, choices=(16, 32, 64), required=True)
parser.add_argument('--objdump', type=Path, required=True)
parser.add_argument('--workload', choices=('fib', 'loop10m', 'loop100m'), default='fib')
parser.add_argument('--l0', action='store_true', help='One functionality sample, not formal Mac diagnostics')
parser.add_argument('--slots', type=Path)
args = parser.parse_args()
for name in ('root', 'build', 'baseline', 'output', 'cc', 'objdump'):
    setattr(args, name, getattr(args, name).resolve())
args.output.mkdir(parents=True)
HERE = Path(__file__).resolve().parent
def verify_checkout(root, product_ref, identity):
    def git(*arguments):
        return subprocess.check_output(['git', '-C', str(root), *arguments], text=True).strip()
    reference = git('rev-parse', product_ref)
    actual = git('rev-parse', 'HEAD')
    assert not git('status', '--porcelain=v1'), 'Checkout must be clean'
    assert identity['commit'] == actual and not identity['dirty'], 'Baseline must identify the actual clean checkout'
    assert identity['buildProfile'] == 'Release', 'Baseline must be Release'
    dependencies = ['src', 'include', 'stdlib', 'cmake', 'CMakeLists.txt', 'CMakePresets.json',
        'scripts/run_scalar_performance.py', 'scripts/new_path_baseline.py']
    assert not git('diff', '--no-renames', '--name-only', reference, actual, '--', *dependencies), 'Product dependency content changed'
    changed = git('diff', '--no-renames', '--name-only', reference, actual).splitlines()
    diagnostic_files = {
        'scripts/diagnostics/native_loop_phase.py',
        'scripts/diagnostics/native_loop_phase_clock.c',
        'scripts/diagnostics/native_loop_phase_clock.h',
        'scripts/diagnostics/capture_reference_crash.py'}
    unexpected = [name for name in changed if not name.startswith('.github/workflows/') and name not in diagnostic_files]
    assert not unexpected, ('Changes outside diagnostic scripts/workflow metadata', unexpected)
    return {'product_ref': reference, 'checkout_head': actual, 'changed_metadata': changed,
        'product_dependency_paths': dependencies, 'product_diff_empty': True,
        'baseline_identity': identity, 'clean': True}

baseline = json.loads(args.baseline.read_text(encoding='utf-8'))
checkout = verify_checkout(args.root, args.product_ref, baseline['identity'])
PRODUCT_REF = checkout['product_ref']
sys.path.insert(0, str(args.root / 'scripts'))
import xray_process
from xray_process import run_child
from xray_slot import Lease
from new_path_baseline import LIBS, FIB_C, FIB_XR, LOOP_C, LOOP_XR
msvc = os.name == 'nt'
suffix = '.obj' if msvc else '.o'
exe_suffix = '.exe' if msvc else '.out'
assert baseline['alignment'] == args.alignment
bench = args.baseline.parent / 'bench'
flags = baseline['nativeCompilerFlags']
workloads = {'fib': ('fib(32)', 'fib', 'fib(32)', 'xrl_f3'),
    'loop10m': ('loop 10M', 'work', 'work(10000000)', 'xrl_invoke'),
    'loop100m': ('loop 100M', 'work', 'work(100000000)', 'xrl_invoke')}
selected_label, reference_function, reference_call, native_function = workloads[args.workload]
report = {'schema': 1, 'kind': 'DIAGNOSTIC_ONLY', 'head': checkout['checkout_head'], 'product_ref': PRODUCT_REF, 'checkout_verification': checkout, 'alignment': args.alignment,
    'declared_plan': {'alignments': [args.alignment], 'workloads': [selected_label],
        'samples_per_layout': 1 if args.l0 else 9, 'order': 'even native-phase,C-phase,native-normal,C-original,native-original; odd reverse'},
    'l0': args.l0, 'phase_order': ['root', 'work_or_callback', 'output'],
    'clock_scope': 'process CPU, user plus system; no clock overhead is subtracted',
    'interpretation': ['Native WORK encloses the callback and its nested OUTPUT. C WORK ends before OUTPUT.',
        'ROOT and WORK include their cold wrappers and clock calls. Whole-child CPU outside ROOT includes clock reporting, startup and exit.',
        'Whole wall outside child CPU also includes scheduling, parent I/O and reaping. Separate sample medians do not isolate those costs.'],
    'commands': [], 'samples': [], 'origins': {}, 'code_comparison': {}}
environment = dict(os.environ, NO_COLOR='1', PYTHONDONTWRITEBYTECODE='1')
held_leases = []

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def save():
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

def run(name, argv, timeout=30):
    row = {'name': name, 'argv': list(map(str, argv)), 'timeout': timeout}
    original_popen = subprocess.Popen
    captured_cpu = {}
    if msvc:
        class CpuPopen(original_popen):
            def wait(self, timeout=None):
                result = super().wait(timeout=timeout)
                kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                values = [ctypes.c_uint64() for _ in range(4)]
                kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.c_void_p] * 4
                if not kernel.GetProcessTimes(int(self._handle), *(ctypes.byref(v) for v in values)):
                    raise ctypes.WinError(ctypes.get_last_error())
                captured_cpu.update(user=values[3].value / 10000000, system=values[2].value / 10000000)
                return result
        subprocess.Popen = CpuPopen
    before = resource.getrusage(resource.RUSAGE_CHILDREN) if not msvc else None
    start = time.perf_counter()
    out_path, err_path = args.output / (name + '.stdout'), args.output / (name + '.stderr')
    try:
        with out_path.open('xb') as out, err_path.open('xb') as err:
            row['exit'] = run_child(row['argv'], cwd=str(args.output), env=environment,
                stdin=subprocess.DEVNULL, stdout=out, stderr=err, timeout=timeout)
        row['terminal_drained'] = True
    finally:
        subprocess.Popen = original_popen
        row['wall_seconds'] = time.perf_counter() - start
        if before:
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            captured_cpu.update(user=after.ru_utime - before.ru_utime, system=after.ru_stime - before.ru_stime)
        row['child_cpu_seconds'] = captured_cpu
        row['cpu_scope'] = 'leader GetProcessTimes' if msvc else 'RUSAGE_CHILDREN delta'
        row['stdout'] = {'path': str(out_path), 'sha256': sha(out_path)}
        row['stderr'] = {'path': str(err_path), 'sha256': sha(err_path)}
        report['commands'].append(row)
        save()
    if row['exit']:
        raise RuntimeError(name + ' failed, see retained logs')
    return row, out_path.read_bytes(), err_path.read_bytes()

def fixed_source(relative):
    data = subprocess.check_output(['git', '-C', str(args.root), 'show', PRODUCT_REF + ':' + relative])
    report['origins'][relative] = {'head': PRODUCT_REF, 'sha256': hashlib.sha256(data).hexdigest()}
    return data.decode('utf-8')

def cache_options(text):
    result = {}
    for line in text.splitlines():
        if line and not line.startswith(('#', '//')) and ':' in line and '=' in line:
            key, value = line.split('=', 1)
            result[key.split(':', 1)[0]] = value
    return result

def verify_configuration(current_baseline, current_cache):
    expected = cache_options(current_baseline['cmakeCache'])
    current = cache_options(current_cache)
    keys = ('CMAKE_BUILD_TYPE', 'CMAKE_C_FLAGS', 'CMAKE_C_FLAGS_RELEASE',
        'ENABLE_ASAN', 'ENABLE_UBSAN', 'XR_XIR_PARANOID', 'CMAKE_C_COMPILER')
    assert all(key in expected for key in keys), 'Baseline configuration is incomplete'
    assert {key: current.get(key) for key in keys} == {key: expected[key] for key in keys}, 'Build differs from the baseline'
    assert expected['CMAKE_BUILD_TYPE'] == 'Release'
    assert all(expected[key] == 'OFF' for key in ('ENABLE_ASAN', 'ENABLE_UBSAN', 'XR_XIR_PARANOID'))
    assert Path(current_baseline['compiler']).resolve() == args.cc, 'Native compiler path changed'
    expected_flags = (['/nologo', '/std:c11', '/O2', '/Ob2', '/DNDEBUG', '/MT',
        '/clang:-ffp-contract=off', f'/clang:-falign-functions={args.alignment}'] if msvc else
        ['-std=c11', '-O2', '-DNDEBUG', '-ffp-contract=off', f'-falign-functions={args.alignment}'])
    assert current_baseline['nativeCompilerFlags'] == expected_flags, 'Native compiler flags changed'
    libraries = {Path(name).name: value for name, value in current_baseline['libraries'].items()}
    names = [name + '.lib' if msvc else 'lib' + name + '.a' for _, name in LIBS]
    assert len(libraries) == len(current_baseline['libraries']) and sorted(libraries) == sorted(names), 'Baseline library inventory changed'
    return libraries

def library_snapshot(paths, expected):
    result = {}
    for path in paths:
        value = sha(path)
        assert value == expected[path.name], ('Current baseline library changed', path, value, expected[path.name])
        result[path.name] = value
    return result

def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)

def compile_c(name, code, native=False):
    obj = args.output / (name + suffix)
    command = [args.cc, *flags]
    if native:
        paths = [args.root / 'src', args.root / 'include', args.root / 'src/runtime/native',
            args.build / 'src/xir/generated', HERE]
        command += [('/I' if msvc else '-I') + str(path) for path in paths]
    else:
        command += [('/I' if msvc else '-I') + str(path) for path in (args.root / 'src', HERE)]
    command += ['/c', code, '/Fo' + str(obj)] if msvc else ['-c', code, '-o', obj]
    run('compile-' + name, command)
    return obj

def link(name, objects, native):
    binary = args.output / (name + exe_suffix)
    command = [args.cc, '/MT', *objects] if msvc else [args.cc, *objects]
    if native:
        command += libraries if msvc else libraries + libraries
        command += ['advapi32.lib', 'userenv.lib'] if msvc else ([] if sys.platform == 'darwin' else ['-lm', '-lpthread', '-ldl'])
    command += ['/Fe' + str(binary), '/link', '/INCREMENTAL:NO', '/OPT:REF'] if msvc else ['-o', binary, '-Wl,-dead_strip']
    run('link-' + name, command)
    return binary

def macho_functions(path):
    data = path.read_bytes()
    if data[:4] != b'\xcf\xfa\xed\xfe':
        return None
    sections, symtab, starts, command = [], None, [], 32
    base, start_data = 0, b''
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        kind, size = struct.unpack_from('<II', data, command)
        if kind == 0x19:
            if data[command + 8:command + 24].rstrip(b'\0') == b'__TEXT':
                base = struct.unpack_from('<Q', data, command + 24)[0]
            for index in range(struct.unpack_from('<I', data, command + 64)[0]):
                at = command + 72 + index * 80
                sections.append(struct.unpack_from('<16s16sQQI', data, at))
        elif kind == 2:
            symtab = struct.unpack_from('<IIII', data, command + 8)
        elif kind == 0x26:
            offset, count = struct.unpack_from('<II', data, command + 8)
            start_data = data[offset:offset + count]
        command += size
    current, value, shift = base, 0, 0
    for byte in start_data:
        value |= (byte & 127) << shift
        if byte & 128:
            shift += 7
            continue
        if not value:
            break
        current += value
        starts.append(current)
        value, shift = 0, 0
    symoff, count, stroff, _ = symtab
    result = {}
    for index in range(count):
        name, typ, section, _, address = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        if typ & 0xe0 or typ & 14 != 14 or not section:
            continue
        label = data[stroff + name:].split(b'\0', 1)[0].decode()
        if label not in ('_xrl_invoke', '_xrl_f3', '_fib', '_work', '_run_bound', '_main', '_xrt_print_i64'):
            continue
        entry = sections[section - 1]
        stop = next((s for s in starts if s > address), entry[2] + entry[3])
        offset = entry[4] + address - entry[2]
        code = data[offset:offset + stop - address]
        loop = None
        for i in range(0, len(code), 4):
            word = struct.unpack_from('<I', code, i)[0]
            if word & 0xff000010 != 0x54000000:
                continue
            immediate = (word >> 5) & 0x7ffff
            if immediate & 0x40000:
                immediate -= 0x80000
            target = i + immediate * 4
            if 0 <= target < i and (loop is None or i - target > loop['bytes']):
                loop = {'address': address + target, 'bytes': i + 4 - target,
                    'sha256': hashlib.sha256(code[target:i + 4]).hexdigest()}
        result[label] = {'address': address, 'file_offset': offset, 'mod16': address % 16,
            'mod64': address % 64, 'bytes': len(code), 'sha256': hashlib.sha256(code).hexdigest(), 'loop': loop}
    return result

def coff_code(path, wanted):
    data = path.read_bytes()
    _, section_count, _, symbol_at, symbol_count, optional, _ = struct.unpack_from('<HHIIIHH', data)
    strings = symbol_at + symbol_count * 18
    index = 0
    while index < symbol_count:
        at = symbol_at + index * 18
        name, value, section, _, _, auxiliary = struct.unpack_from('<8sIhHBB', data, at)
        if name[:4] == b'\0' * 4:
            offset = struct.unpack_from('<I', name, 4)[0]
            name = data[strings + offset:].split(b'\0', 1)[0]
        else:
            name = name.rstrip(b'\0')
        if name.decode() == wanted:
            assert 1 <= section <= section_count
            entry = 20 + optional + (section - 1) * 40
            size, offset = struct.unpack_from('<II', data, entry + 16)
            return data[offset + value:offset + size]
        index += 1 + auxiliary
    raise ValueError('COFF function missing ' + wanted)

def pe_locations(path, needle):
    data = path.read_bytes()
    header = struct.unpack_from('<I', data, 0x3c)[0]
    optional_size = struct.unpack_from('<H', data, header + 20)[0]
    image = struct.unpack_from('<Q', data, header + 48)[0]
    locations = []
    for index in range(struct.unpack_from('<H', data, header + 6)[0]):
        at = header + 24 + optional_size + index * 40
        if data[at:at + 8].rstrip(b'\0') != b'.text':
            continue
        virtual, size, raw = struct.unpack_from('<III', data, at + 12)
        code = data[raw:raw + size]
        start = code.find(needle)
        while start >= 0:
            locations.append(hex(image + virtual + start))
            start = code.find(needle, start + 1)
    return locations

def execute():
    global libraries
    assert msvc or sys.platform == 'darwin', 'Only Windows/macOS diagnostic adapters are implemented'
    report['checkout_verification'] = verify_checkout(args.root, args.product_ref, baseline['identity'])

    report['diagnostic_files'] = {name: sha(HERE / name) for name in ('native_loop_phase.py', 'native_loop_phase_clock.c', 'native_loop_phase_clock.h')}
    historical = HERE / 'native_loop_phase_reference.json'
    report['historical_reference'] = {'path': str(historical), 'sha256': sha(historical),
        'product_ref': json.loads(historical.read_text(encoding='utf-8'))['product_ref'],
        'role': 'Preserved historical evidence; current products are checked against the selected baseline.'}
    shutil.copyfile(args.baseline, args.output / 'baseline-result.json')
    report['baseline'] = {'path': str(args.baseline), 'sha256': sha(args.baseline),
        'snapshot': str(args.output / 'baseline-result.json'),
        'initial_samples': len(baseline['samples']), 'serial_samples': len(baseline['nativeSerialSamples'])}
    binary = baseline['executables']['Xray']
    assert sha(binary['path']) == binary['sha256'], 'Baseline CLI entity changed'
    assert json.loads((args.baseline.parent / 'identity.stdout').read_text(encoding='utf-8')) == baseline['identity']
    for command in baseline['commands']:
        for stream in ('stdout', 'stderr'):
            saved = command[stream]
            assert sha(saved['path']) == saved['sha256'], ('Baseline stream changed', command['name'], stream)
    _, compiler_version, _ = run('compiler-version', [args.cc, '--version'])
    assert compiler_version == (args.baseline.parent / 'compiler-version.stdout').read_bytes(), 'Compiler differs from the current baseline'
    libraries = [args.build / directory / (name + '.lib' if msvc else 'lib' + name + '.a') for directory, name in LIBS]
    current_cache = (args.build / 'CMakeCache.txt').read_text(encoding='utf-8')
    expected_libraries = verify_configuration(baseline, current_cache)
    assert Path(cache_options(current_cache)['CMAKE_HOME_DIRECTORY']).resolve() == args.root, 'Build belongs to another source root'
    before_libraries = library_snapshot(libraries, expected_libraries)
    report['libraries'] = {}
    for path in libraries:
        report['libraries'][path.name] = {'path': str(path), 'baseline_sha256': expected_libraries[path.name],
            'before_sha256': before_libraries[path.name]}
    report['configuration'] = {'baseline_cache_sha256': hashlib.sha256(baseline['cmakeCache'].encode()).hexdigest(),
        'current_cache_sha256': hashlib.sha256(current_cache.encode()).hexdigest(),
        'native_flags': flags, 'compiler_version_sha256': hashlib.sha256(compiler_version).hexdigest(),
        'runtime_hooks': 'Normal and phase hooks are rebuilt from the selected product using identical flags and library dependencies.'}
    execution = fixed_source('src/runtime/native/xnative_execution.c')
    (args.output / 'execution-normal.c').write_text(execution, encoding='utf-8')
    execution = replace_once(execution, '#include "xnative_execution.h"', '#include "xnative_execution.h"\n#include "native_loop_phase_clock.h"')
    execution = replace_once(execution, '        root(context);', '        phase_begin(PHASE_WORK);\n        root(context);\n        phase_end(PHASE_WORK);')
    marker = 'XRAY_API void xrt_run_standalone('
    begin, end = execution.index(marker), execution.index('typedef struct XrtStateInvocation')
    function = execution[begin:end]
    function = replace_once(function, '    XR_CHECK(root', '    phase_reset();\n    phase_begin(PHASE_ROOT);\n    XR_CHECK(root')
    function = replace_once(function,
        '    if (run_bound(map, &failure, root, context, &strings) == XRT_EXECUTION_FAILURE)\n        native_failure_exit(map, &failure);',
        '    XrtExecutionStatus phase_status = run_bound(map, &failure, root, context, &strings);\n    phase_end(PHASE_ROOT);\n    phase_report("native", phase_status == XRT_EXECUTION_COMPLETED);\n    if (phase_status == XRT_EXECUTION_FAILURE) native_failure_exit(map, &failure);')
    execution = execution[:begin] + function + execution[end:]
    output = fixed_source('src/runtime/native/xnative_output.c')
    (args.output / 'output-normal.c').write_text(output, encoding='utf-8')
    output = replace_once(output, '#include "xnative_execution.h"', '#include "xnative_execution.h"\n#include "native_loop_phase_clock.h"')
    begin, end = output.index('XRAY_API void xrt_print_i64('), output.index('XRAY_API void xrt_print_string(')
    function = output[begin:end]
    function = replace_once(function, '    char bytes[22];', '    phase_begin(PHASE_OUTPUT);\n    char bytes[22];')
    function = function.rstrip()
    assert function.endswith('}')
    function = function[:-1] + '    phase_end(PHASE_OUTPUT);\n}\n\n'
    output = output[:begin] + function + output[end:]
    for name, text in (('execution.c', execution), ('output.c', output)):
        (args.output / name).write_text(text, encoding='utf-8')
    clock = compile_c('phase_clock', HERE / 'native_loop_phase_clock.c')
    normal_execution_obj = compile_c('execution-normal', args.output / 'execution-normal.c', True)
    normal_output_obj = compile_c('output-normal', args.output / 'output-normal.c', True)
    execution_obj = compile_c('execution', args.output / 'execution.c', True)
    output_obj = compile_c('output', args.output / 'output.c', True)
    report['rebuilt_objects'] = {str(path): sha(path) for path in
        (clock, normal_execution_obj, normal_output_obj, execution_obj, output_obj)}
    report['products'] = {}
    engines = {}
    for label in (selected_label,):
        stem = re.sub(r'\W', '_', label)
        original_obj = bench / (stem + '.native' + suffix)
        original_native = bench / (stem + '.native' + exe_suffix)
        original_c = bench / (stem + '_ref' + exe_suffix)
        assert sha(original_native) == baseline['products'][label]['native']['sha256']
        assert sha(original_c) == baseline['products'][label]['C']['sha256']
        original_reference_obj = bench / (stem + '_ref' + suffix)
        original_reference_source = bench / (stem + '_ref.c')
        count = {'loop10m': '10000000', 'loop100m': '100000000'}.get(args.workload)
        expected_c = FIB_C if count is None else LOOP_C.replace('@N@', count)
        expected_xr = FIB_XR if count is None else LOOP_XR.replace('@N@', count)
        assert original_reference_source.read_text(encoding='utf-8') == expected_c, 'Reference numerical source changed'
        assert (bench / (stem + '.xr')).read_text(encoding='utf-8') == expected_xr, 'Original workload source changed'
        inputs = [original_obj, original_native, original_c, original_reference_obj,
            original_reference_source, bench / (stem + '.native.c'), bench / (stem + '.xr')]
        originals = args.output / 'originals'
        originals.mkdir()
        input_hashes = {}
        for path in inputs:
            input_hashes[str(path)] = sha(path)
            shutil.copyfile(path, originals / path.name)
        report['origins'][label] = {'object': str(original_obj), 'sha256': sha(original_obj),
            'baseline': str(args.baseline), 'baseline_sha256': sha(args.baseline),
            'native_executable_sha256': sha(original_native), 'C_executable_sha256': sha(original_c),
            'inputs': input_hashes}
        normal = link(stem + '-native-normal', [original_obj, normal_execution_obj, normal_output_obj], True)
        native = link(stem + '-native-phase', [original_obj, execution_obj, output_obj, clock], True)
        text = original_reference_source.read_text(encoding='utf-8')
        prefix = text[:text.index('int main(void)')]
        assert re.search(r'\b' + reference_function + r'\s*\(', prefix), 'Reference numerical function missing'
        assert reference_call in re.sub(r'\s+', '', text[text.index('int main(void)'):]), 'Reference call changed'
        text = '#include "native_loop_phase_clock.h"\n' + prefix + '''int main(void) {
    phase_reset(); phase_begin(PHASE_ROOT); phase_begin(PHASE_WORK);
    int64_t value = ''' + reference_call + ''';
    phase_end(PHASE_WORK); phase_begin(PHASE_OUTPUT);
    printf("%lld\\n", (long long)value);
    phase_end(PHASE_OUTPUT); phase_end(PHASE_ROOT); phase_report("C", 1);
    return 0;
}
'''
        reference = args.output / (stem + '-reference.c')
        reference.write_text(text, encoding='utf-8')
        reference_obj = compile_c(stem + '-reference', reference)
        report['rebuilt_objects'][str(reference_obj)] = sha(reference_obj)
        reference_binary = link(stem + '-C-phase', [reference_obj, clock], False)
        engines[label] = {'native-phase': native, 'C-phase': reference_binary,
            'native-normal': normal, 'C-original': original_c, 'native-original': original_native}
        report['products'][label] = {name: {'path': str(path), 'sha256': sha(path)}
            for name, path in engines[label].items()}
        comparison = {'original_native_object_unchanged': sha(original_obj) == report['origins'][label]['sha256']}
        if sys.platform == 'darwin':
            maps = {name: macho_functions(binary) for name, binary in engines[label].items()}
            comparison['functions'] = maps
            for engine, symbol in (('native-normal', '_' + native_function), ('native-phase', '_' + native_function),
                ('C-phase', '_' + reference_function)):
                control = 'C-original' if engine == 'C-phase' else 'native-original'
                actual, original = maps[engine][symbol], maps[control][symbol]
                comparison[engine] = {'symbol': symbol, 'control': control,
                    'body_bytes_equal': actual['bytes'] == original['bytes'] and actual['sha256'] == original['sha256'],
                    'body_address_equal': actual['address'] == original['address']}
            symbol = '_' + native_function
            comparison['native_phase_normal_body_equal'] = (
                maps['native-phase'][symbol]['bytes'] == maps['native-normal'][symbol]['bytes'] and
                maps['native-phase'][symbol]['sha256'] == maps['native-normal'][symbol]['sha256'])
            comparison['byte_comparability'] = 'Per-function results only; a mismatch grants no unchanged numerical-code claim.'
        disassemblies = {}
        for role, path in (('native-original-object', original_obj), ('reference-original-object', original_reference_obj),
            ('reference-phase-object', reference_obj)):
            options = ['-tvV'] if args.objdump.name == 'otool' else ['-d']
            _, assembly, _ = run(stem + '-disasm-' + role, [args.objdump, *options, path])
            disassemblies[role] = assembly.decode('utf-8')
        if msvc:
            original_work = coff_code(original_reference_obj, reference_function)
            phase_work = coff_code(reference_obj, reference_function)
            comparison['C_work_section_bytes_equal'] = original_work == phase_work
            comparison['C_work_addresses'] = {'original': pe_locations(original_c, original_work),
                'phase': pe_locations(reference_binary, phase_work)}
            code = coff_code(original_obj, native_function)
            assembly = disassemblies['native-original-object'].split('<' + native_function + '>:', 1)[1].split('Disassembly of section', 1)[0]
            back_edges = []
            for match in re.finditer(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s+j\w+\s+0x([0-9a-f]+)', assembly, re.M):
                at, target = int(match[1], 16), int(match[3], 16)
                stop = at + len(bytes.fromhex(match[2]))
                if target < at:
                    back_edges.append((stop - target, target, stop))
            _, start, stop = max(back_edges) if back_edges else (len(code), 0, len(code))
            hot = code[start:stop]
            comparison['native_hot_sha256'] = hashlib.sha256(hot).hexdigest()
            comparison['native_hot_addresses'] = {'original': pe_locations(original_native, hot), 'phase': pe_locations(native, hot)}
            assert all(len(values) == 1 for values in comparison['native_hot_addresses'].values())
        report['code_comparison'][label] = comparison
    for sample in range(1 if args.l0 else 9):
        for label, row in engines.items():
            order = list(row)
            if sample % 2:
                order.reverse()
            outputs = []
            for engine in order:
                command, out, err = run('sample-' + str(sample) + '-' + label.replace(' ', '_') + '-' + engine, [row[engine]])
                outputs.append(out.replace(b'\r\n', b'\n') if engine.startswith('C-') else out)
                if engine.endswith('-phase'):
                    assert err.startswith(b'XR_PHASE ') and len(err.splitlines()) == 1
                    phase = json.loads(err[len(b'XR_PHASE '):])
                    assert phase['completed'] == 1 and phase['begins'] == [1, 1, 1] and phase['ends'] == [1, 1, 1] and phase['active'] == [0, 0, 0]
                    assert phase['cpu_ns'][0] >= phase['cpu_ns'][1]
                    if engine == 'native-phase':
                        assert phase['cpu_ns'][1] >= phase['cpu_ns'][2]
                    else:
                        assert phase['cpu_ns'][0] >= phase['cpu_ns'][1] + phase['cpu_ns'][2]
                    command['phase'] = phase
                else:
                    assert not err
                report['samples'].append({'sample': sample, 'workload': label, 'engine': engine,
                    'command': command['name'], 'wall_seconds': command['wall_seconds'],
                    'child_cpu_seconds': command['child_cpu_seconds'], 'phase': command.get('phase')})
            assert len(set(outputs)) == 1
            if args.workload == 'fib':
                assert outputs == [b'2178309\n'] * len(order), 'Original fib golden changed'
    for label, origin in report['origins'].items():
        if 'object' in origin:
            assert sha(origin['object']) == origin['sha256']
            for path, expected in origin['inputs'].items():
                assert sha(path) == expected, ('Original product changed', path)
    for name, value in library_snapshot(libraries, expected_libraries).items():
        report['libraries'][name]['after_sha256'] = value
    for path, expected in report['rebuilt_objects'].items():
        assert sha(path) == expected, ('Diagnostic object changed', path)
    for products in report['products'].values():
        for product in products.values():
            assert sha(product['path']) == product['sha256'], ('Diagnostic executable changed', product['path'])
    report['checkout_verification_after'] = verify_checkout(args.root, args.product_ref, baseline['identity'])
    report['state'] = 'L0_PASS' if args.l0 else 'DIAGNOSTIC_COMPLETE'
    save()

try:
    if args.slots:
        with Lease(args.slots, 'heavy', payload={'task': 'diagnostic CPU phases', 'source': PRODUCT_REF}) as heavy:
            held_leases.append(heavy)
            with Lease(args.slots, 'build', payload={'task': 'diagnostic runtime direct Clang compile/link', 'source': PRODUCT_REF}) as build:
                held_leases.append(build)
                execute()
    else:
        execute()
except BaseException as error:
    report.update(state='FAIL', error=str(error), error_type=type(error).__name__)
    raise
finally:
    report['leases'] = [json.loads(lease.path.read_text()) for lease in held_leases]
    save()
print(json.dumps({'state': report.get('state'), 'samples': len(report['samples']), 'output': str(args.output)}))
