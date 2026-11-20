"""Isolate owner slot writes in one archived Array kernel."""

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
HOLD_PATCH = Path(__file__).with_name('task345_x64_owner_hold.patch')
HOLD_SHA = 'a8da52e988ede12d338d276d96275f2fedecbc790dc7b1eeb11c368087e4a75c'
HOLD_ARGS = ('--hold-owner',)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def archived(path, expected):
    data = (ARCHIVE / path).read_bytes()
    if digest(data) != expected:
        raise ValueError(f'Unexpected archived input: {path}')
    return data


def hold_source():
    patched = RESULTS / 'owner-patched.c'
    command = ['patch', '--batch', '--forward', '--fuzz=0', '--silent',
               '--output', str(patched), '--input', str(HOLD_PATCH), str(ARCHIVE / 'inplace.c')]
    with (RESULTS / 'owner-patch.log').open('wb') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=20)
    data = patched.read_bytes()
    if digest(data) != HOLD_SHA:
        raise ValueError('Guarded owner probe differs from the reviewed source')
    return data


def verify_hold():
    command = [str(RESULTS / 'owner-witness'), *HOLD_ARGS]
    sample = subprocess.run(command, capture_output=True, timeout=10, check=False)
    (RESULTS / 'owner-witness.stdout').write_bytes(sample.stdout)
    (RESULTS / 'owner-witness.stderr').write_bytes(sample.stderr)
    expected = {
        'hold_owner_witness': True,
        'skipped': [20000, 20000, 20000, 19999, 19999, 19999],
        'rejected': [0, 0, 0, 0, 0, 0],
        'slow': [0, 0, 0, 0, 0, 0],
        'skipped_owner_writes': 239994,
        'arm_attempts': 1,
        'arm_successes': 1,
        'entry_guard_accepts': 20000,
        'entry_guard_declines': 0,
        'latch_guard_attempts': 0,
        'latch_guard_declines': 0,
        'real_exit_reached': 1,
        'real_exit_restored': 1,
        'real_exit_published': 1,
        'passed': True,
    }
    if sample.returncode or sample.stdout != EXPECTED:
        raise RuntimeError(f'Owner witness failed with exit {sample.returncode}')
    witness = json.loads(sample.stderr)
    if witness != expected:
        raise RuntimeError(f'Owner witness differs from the independently derived counts: {witness}')
    return {'argv': command, 'stdoutSha256': digest(sample.stdout),
            'stderrSha256': digest(sample.stderr), 'counters': witness}


def compile_case(name, data, library, *, definitions=()):
    source = RESULTS / f'{name}.c'
    executable = RESULTS / name
    source.write_bytes(data)
    command = ['cc', '-std=c11', '-O2', '-ffp-contract=off', '-Wall', '-Wextra',
               '-Werror', '-pedantic', '-Iinclude', *definitions, str(source)]
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


def run_case(name, argv, index):
    started = time.perf_counter()
    cpu_before, switches_before = child_usage()
    sample = subprocess.run(argv, capture_output=True, timeout=10, check=False)
    cpu_after, switches_after = child_usage()
    elapsed = time.perf_counter() - started
    if sample.returncode or sample.stdout != EXPECTED or sample.stderr:
        (RESULTS / f'{index}-{name}.stdout').write_bytes(sample.stdout)
        (RESULTS / f'{index}-{name}.stderr').write_bytes(sample.stderr)
        raise RuntimeError(f'Unexpected output for {name} in round {index}: {sample.returncode}')
    return {'wallSeconds': elapsed, 'cpuSeconds': cpu_after - cpu_before,
            'contextSwitches': switches_after - switches_before,
            'stdoutSha256': digest(sample.stdout)}


def measure(cases):
    names = list(cases)
    for name in names:
        run_case(name, cases[name], 'warmup')
    rounds = []
    for index in range(20):
        rotation = index % len(names)
        order = names[rotation:] + names[:rotation]
        if (index // len(names)) % 2:
            order.reverse()
        rounds.append({name: run_case(name, cases[name], index) for name in order})
    comparisons = {}
    for name in names:
        comparisons[name] = {}
        for metric in ('wallSeconds', 'cpuSeconds'):
            samples = [round_[name][metric] for round_ in rounds]
            row = {'median': statistics.median(samples)}
            for control in ('base', 'off', 'reference'):
                ratios = [round_[name][metric] / round_[control][metric] for round_ in rounds]
                row[control] = {'median': statistics.median(ratios),
                                'min': min(ratios), 'max': max(ratios)}
            comparisons[name][metric] = row
    return {'rounds': rounds, 'statistics': comparisons}


def main():
    RESULTS.mkdir()
    original = archived('inplace.c', 'be99886fd42ef1d4529473474df8ae0d988479e66d20b548c6dc3aa0677b5abf')
    reference = archived('inputs/inplace.c', '1b0900bb2ff9313c5fbb68b8b6f8785a5a55c17e65c2095b3471ed4f81484d27')
    library_sha = '981ec28dd48ecb12ebbff1a0deac0a5bb659634d11802075bd58365f80ff7f48'
    library = ARCHIVE / 'libraries/libxray_native_runtime.a'
    archived('libraries/libxray_native_runtime.a', library_sha)
    guarded = hold_source()
    variants = {'base': original, 'owner-hold': guarded, 'reference': reference}
    identities = {name: compile_case(name, data, library) for name, data in variants.items()}
    identities['owner-witness'] = compile_case(
        'owner-witness', guarded, library, definitions=('-DXRL_VERIFY_HOLD_OWNER=1',))
    witness = verify_hold()
    cases = {'base': [str(RESULTS / 'base')], 'off': [str(RESULTS / 'owner-hold')],
             'on': [str(RESULTS / 'owner-hold'), *HOLD_ARGS],
             'reference': [str(RESULTS / 'reference')]}
    allowed_cpus = sorted(os.sched_getaffinity(0))
    pinned_cpu = allowed_cpus[-1]
    record = {'archiveCommit': '940e171bbcfa9052e0c5d4fce0c82d6da8979c63',
              'archiveRun': 44726072579, 'librarySha256': library_sha,
              'compiler': subprocess.check_output(['cc', '--version'], text=True, timeout=10),
              'binaries': identities, 'availableCpus': allowed_cpus, 'pinnedCpu': pinned_cpu,
              'diagnosticCommit': os.environ['GITHUB_SHA'],
              'timedCases': cases, 'witness': witness, 'patchSha256': digest(HOLD_PATCH.read_bytes())}
    (RESULTS / 'cpuinfo.txt').write_bytes(Path('/proc/cpuinfo').read_bytes())
    record['unpinned'] = measure(cases)
    os.sched_setaffinity(0, {pinned_cpu})
    record['pinned'] = measure(cases)
    (RESULTS / 'result.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: record[key]['statistics'] for key in ('unpinned', 'pinned')}, indent=2))


if __name__ == '__main__':
    main()
