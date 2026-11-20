"""Measure fixed-span boundary control with the same packed Array update."""

import hashlib
import json
import os
from pathlib import Path
import resource
import statistics
import subprocess
import time


ARCHIVE = Path('ci-artifact/build/array-performance-prepare')
RESULTS = Path('probe-results')
EXPECTED = b'-742713184288245760\n'
SPAN_HEADER = b'xrl_fixed_1_bb7: ;\n'
SPAN_TAIL = b'    for (; v11 < (int64_t)xrl_fixed_cache_1.length; ++v11) {'
FORMULA = '((bits << 1) | UINT64_C(1)) + bits'
ORIGINAL_SPAN = b'enum { xrl_fixed_span = 64 };'
WIDE_SPAN = b'enum { xrl_fixed_span = 512 };'
SPAN_HASHES = {
    'odd-add': 'a70fc044bc4d5e21ebcff020beeda63447ae0d0d6062688ff669d24cbec9d52d',
    'odd-add-span-512': 'acf67011e5371a193aeca1d460087f989c3ebdd381c3b68d9f00f2dd6eb7be11',
}



def digest(data):
    return hashlib.sha256(data).hexdigest()


def archived(path, expected):
    data = (ARCHIVE / path).read_bytes()
    if digest(data) != expected:
        raise ValueError(f'Unexpected archived input: {path}')
    return data


def replace_span_arithmetic(original, formula):
    first_span = original.index(SPAN_HEADER)
    tail = original.index(SPAN_TAIL, first_span)
    first = original.index(b'    XRT_ARRAY_I64_REGION_COPY(&v49,', first_span, tail)
    after = original.index(b'\n    v56 =', first, tail)
    segment = original[first:after]
    if (b'v68 = ((uint64_t)v49 * (uint64_t)v50)' not in segment or
            b'v68 = ((uint64_t)v51 + (uint64_t)v52)' not in segment or
            segment.count(b'XRT_ARRAY_I64_REGION_COPY(') != 4):
        raise ValueError('Unexpected archived arithmetic segment')
    address = b'xrl_fixed_cache_1.elements + (size_t)v14 * sizeof(int64_t)'
    if segment.count(address) != 2:
        raise ValueError('Unexpected archived load or store')
    replacement = (
        b'    uint64_t bits;\n'
        b'    XRT_ARRAY_I64_REGION_COPY(&bits, ' + address + b');\n'
        b'    bits = ' + formula.encode('ascii') + b';\n'
        b'    XRT_ARRAY_I64_REGION_COPY(' + address + b', &bits);'
    )
    return original[:first] + replacement + original[after:]


def vector_report(command, data, name, source):
    report = RESULTS / f'{name}.vec'
    code = RESULTS / f'{name}.report.s'
    with (RESULTS / f'{name}.report.log').open('wb') as log:
        subprocess.run(command + [f'-fopt-info-vec-all={report}', '-S', str(source), '-o', str(code)],
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    lines = report.read_text(encoding='utf-8').splitlines()
    if name == 'reference':
        return [line for line in lines if 'optimized: loop vectorized' in line]
    start = data.index(SPAN_HEADER)
    first = data[:start].count(b'\n') + 1
    last = data[:data.index(SPAN_TAIL, start)].count(b'\n') + 1
    prefix = str(source) + ':'
    focused = []
    for line in lines:
        if line.startswith(prefix):
            row = line.removeprefix(prefix).split(':', 1)[0]
            if row.isdigit() and first <= int(row) < last:
                focused.append(line)
    return focused


def span_variants(original):
    fixed = replace_span_arithmetic(original, FORMULA)
    if fixed.count(ORIGINAL_SPAN) != 1:
        raise ValueError('Missing the known fixed span')
    variants = {'odd-add': fixed, 'odd-add-span-512': fixed.replace(ORIGINAL_SPAN, WIDE_SPAN)}
    for name, data in variants.items():
        if digest(data) != SPAN_HASHES[name]:
            raise ValueError(f'Span source differs from the reviewed variant: {name}')
    return variants


def packed_loop(assembly):
    lines = assembly.splitlines()
    for index, line in enumerate(lines):
        if line.split()[:2] == ['psllq', '$1,']:
            core = [item.strip() for item in lines[index - 3:index + 6]]
            operators = [item.split()[0] for item in core if item]
            if 'por' in operators and 'paddq' in operators:
                return {'instructions': core, 'operators': operators}
    return None


def compile_case(name, data, library):
    source = RESULTS / f'{name}.c'
    executable = RESULTS / name
    source.write_bytes(data)
    command = ['cc', '-std=c11', '-O2', '-ffp-contract=off', '-Wall', '-Wextra',
               '-Werror', '-pedantic', '-Iinclude']
    focused = vector_report(command, data, name, source)
    command.append(str(source))
    if name != 'reference':
        command.extend([str(library), '-pthread', '-lm', '-ldl'])
    command.extend(['-o', str(executable)])
    with (RESULTS / f'{name}.build.log').open('wb') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    assembly = subprocess.check_output(['objdump', '-d', str(executable)], timeout=20)
    (RESULTS / f'{name}.asm').write_bytes(assembly)
    report_assembly = (RESULTS / f'{name}.report.s').read_text(encoding='utf-8')
    return {'command': command, 'sourceSha256': digest(data),
            'binarySha256': digest(executable.read_bytes()), 'focusedVectorReport': focused,
            'packedLoop': packed_loop(report_assembly)}


def child_usage():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return (usage.ru_utime + usage.ru_stime, usage.ru_nvcsw + usage.ru_nivcsw)


def run_case(name, index):
    started = time.perf_counter()
    cpu_before, switches_before = child_usage()
    sample = subprocess.run([str(RESULTS / name)], capture_output=True, timeout=10, check=False)
    cpu_after, switches_after = child_usage()
    elapsed = time.perf_counter() - started
    if index == 'warmup' or sample.returncode or sample.stdout != EXPECTED or sample.stderr:
        (RESULTS / f'{index}-{name}.stdout').write_bytes(sample.stdout)
        (RESULTS / f'{index}-{name}.stderr').write_bytes(sample.stderr)
    if sample.returncode or sample.stdout != EXPECTED or sample.stderr:
        raise RuntimeError(f'Unexpected output for {name} in round {index}: {sample.returncode}')
    return {'wallSeconds': elapsed, 'cpuSeconds': cpu_after - cpu_before,
            'contextSwitches': switches_after - switches_before,
            'stdoutSha256': digest(sample.stdout)}


def measure(names):
    for name in names:
        run_case(name, 'warmup')
    rounds = []
    for index in range(20):
        rotation = index % len(names)
        order = names[rotation:] + names[:rotation]
        if (index // len(names)) % 2:
            order.reverse()
        rounds.append({name: run_case(name, index) for name in order})
    comparisons = {}
    for name in names:
        comparisons[name] = {}
        for metric in ('wallSeconds', 'cpuSeconds'):
            samples = [row[name][metric] for row in rounds]
            summary = {'median': statistics.median(samples)}
            for control in ('odd-add', 'reference'):
                ratios = [row[name][metric] / row[control][metric] for row in rounds]
                summary[control] = {'median': statistics.median(ratios),
                                    'min': min(ratios), 'max': max(ratios)}
            comparisons[name][metric] = summary
    return {'rounds': rounds, 'statistics': comparisons}


def main():
    RESULTS.mkdir()
    original = archived('inplace.c', 'be99886fd42ef1d4529473474df8ae0d988479e66d20b548c6dc3aa0677b5abf')
    reference = archived('inputs/inplace.c', '1b0900bb2ff9313c5fbb68b8b6f8785a5a55c17e65c2095b3471ed4f81484d27')
    library_sha = '981ec28dd48ecb12ebbff1a0deac0a5bb659634d11802075bd58365f80ff7f48'
    library = ARCHIVE / 'libraries/libxray_native_runtime.a'
    archived('libraries/libxray_native_runtime.a', library_sha)
    variants = span_variants(original)
    variants['reference'] = reference
    identities = {name: compile_case(name, data, library) for name, data in variants.items()}
    allowed_cpus = sorted(os.sched_getaffinity(0))
    pinned_cpu = allowed_cpus[-1]
    record = {'archiveCommit': '940e171bbcfa9052e0c5d4fce0c82d6da8979c63',
              'archiveRun': 44726072579, 'librarySha256': library_sha,
              'compiler': subprocess.check_output(['cc', '--version'], text=True, timeout=10),
              'binaries': identities, 'availableCpus': allowed_cpus, 'pinnedCpu': pinned_cpu,
              'diagnosticCommit': os.environ['GITHUB_SHA']}
    narrow = identities['odd-add']['packedLoop']
    wide = identities['odd-add-span-512']['packedLoop']
    record['samePackedOpcodes'] = bool(narrow and wide and narrow['operators'] == wide['operators'])
    record['modeledBoundaryCounts'] = {
        'elements': 4096, 'outerRepetitions': 20000, 'packedIterations': 40960000,
        'odd-add': 1280000, 'odd-add-span-512': 160000,
    }
    (RESULTS / 'cpuinfo.txt').write_bytes(Path('/proc/cpuinfo').read_bytes())
    record['unpinned'] = measure(list(variants))
    os.sched_setaffinity(0, {pinned_cpu})
    record['pinned'] = measure(list(variants))
    (RESULTS / 'result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({
        'samePackedOpcodes': record['samePackedOpcodes'],
        'packedLoops': {name: info['packedLoop'] for name, info in identities.items()},
        'statistics': {key: record[key]['statistics'] for key in ('unpinned', 'pinned')},
    }, indent=2))


if __name__ == '__main__':
    main()
