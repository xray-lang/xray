"""Measure one archived Array kernel on the original runner class."""

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


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archived(path, expected):
    data = (ARCHIVE / path).read_bytes()
    if digest(data) != expected:
        raise ValueError(f'Unexpected archived input: {path}')
    return data


def single_span(data):
    header = b'xrl_fixed_1_bb7: ;\n'
    tail = b'    for (; v11 < (int64_t)xrl_fixed_cache_1.length; ++v11) {\n'
    if data.count(header) != 1:
        raise ValueError('Missing witnessed loop header')
    start = data.index(header) + len(header)
    end = data.index(tail, start)
    if b'enum { xrl_fixed_span = 64 };' not in data[start:end]:
        raise ValueError('Missing witnessed fixed-size span')
    return data[:start] + data[end:]


def one_owner(data):
    exit_edge = b'    xrl_fixed_cache_1 = (XrtArrayI64RegionCache){0};\n    goto xrl_fixed_exit_1;'
    if data.count(exit_edge) != 1:
        raise ValueError('Missing witnessed loop exit')
    # Witness only: the original entry guard and exit remain live. Repeat the
    # fixed workload under the same backing, leaving the readback unchanged.
    repeat = b'''    if (v12 < INT64_C(19999)) {
        ++v12;
        v11 = 0;
        goto xrl_fixed_1_bb7;
    }
'''
    return data.replace(exit_edge, repeat + exit_edge)


def compile_case(name, data, library):
    source = RESULTS / f'{name}.c'
    executable = RESULTS / name
    source.write_bytes(data)
    command = ['cc', '-std=c11', '-O2', '-ffp-contract=off', '-Wall', '-Wextra',
               '-Werror', '-pedantic', '-Iinclude', str(source)]
    if name != 'reference':
        command.extend([str(library), '-pthread', '-lm', '-ldl'])
    command.extend(['-o', str(executable)])
    with (RESULTS / f'{name}.build.log').open('wb') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    assembly = subprocess.check_output(['objdump', '-d', str(executable)], timeout=20)
    (RESULTS / f'{name}.asm').write_bytes(assembly)
    return {'command': command, 'sourceSha256': digest(data),
            'binarySha256': digest(executable.read_bytes())}


def child_usage():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return (usage.ru_utime + usage.ru_stime, usage.ru_nvcsw + usage.ru_nivcsw)


def run_case(name, index):
    started = time.perf_counter()
    cpu_before, switches_before = child_usage()
    sample = subprocess.run([str(RESULTS / name)], capture_output=True, timeout=10, check=False)
    cpu_after, switches_after = child_usage()
    elapsed = time.perf_counter() - started
    if sample.returncode or sample.stdout != EXPECTED or sample.stderr:
        (RESULTS / f'{index}-{name}.stdout').write_bytes(sample.stdout)
        (RESULTS / f'{index}-{name}.stderr').write_bytes(sample.stderr)
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
            samples = [round_[name][metric] for round_ in rounds]
            ratios = [round_[name][metric] / round_['base'][metric] for round_ in rounds]
            comparisons[name][metric] = {'median': statistics.median(samples),
                                         'pairedMedianVsBase': statistics.median(ratios),
                                         'pairedMinVsBase': min(ratios),
                                         'pairedMaxVsBase': max(ratios)}
    return {'rounds': rounds, 'statistics': comparisons}


def main():
    RESULTS.mkdir()
    original = archived('inplace.c', 'be99886fd42ef1d4529473474df8ae0d988479e66d20b548c6dc3aa0677b5abf')
    reference = archived('inputs/inplace.c', '1b0900bb2ff9313c5fbb68b8b6f8785a5a55c17e65c2095b3471ed4f81484d27')
    library_sha = '981ec28dd48ecb12ebbff1a0deac0a5bb659634d11802075bd58365f80ff7f48'
    library = ARCHIVE / 'libraries/libxray_native_runtime.a'
    archived('libraries/libxray_native_runtime.a', library_sha)
    retained = one_owner(original)
    variants = {'base': original, 'single-span': single_span(original),
                'one-owner': retained, 'one-owner-single-span': single_span(retained),
                'reference': reference}
    identities = {name: compile_case(name, data, library) for name, data in variants.items()}
    allowed_cpus = sorted(os.sched_getaffinity(0))
    pinned_cpu = allowed_cpus[-1]
    record = {'archiveCommit': '940e171bbcfa9052e0c5d4fce0c82d6da8979c63',
              'archiveRun': 44726072579, 'librarySha256': library_sha,
              'compiler': subprocess.check_output(['cc', '--version'], text=True, timeout=10),
              'binaries': identities, 'availableCpus': allowed_cpus, 'pinnedCpu': pinned_cpu}
    (RESULTS / 'cpuinfo.txt').write_bytes(Path('/proc/cpuinfo').read_bytes())
    record['unpinned'] = measure(list(variants))
    os.sched_setaffinity(0, {pinned_cpu})
    record['pinned'] = measure(list(variants))
    (RESULTS / 'result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: record[key]['statistics'] for key in ('unpinned', 'pinned')}, indent=2))


if __name__ == '__main__':
    main()
