"""Real native transactions; injected gates use the same owner implementation."""
from pathlib import Path
import hashlib, os, re, subprocess, sys, tempfile, time

exe, root, compiler, linker, sdk = map(lambda value: Path(value).resolve(), sys.argv[1:6])
faults = sys.argv[6:] == ['faults']
guard = sys.argv[6:] == ['guard']
vc = Path(os.environ['VCToolsInstallDir']) / 'lib/x64'
kit = Path(os.environ['WindowsSdkDir']) / 'Lib' / os.environ['WindowsSDKVersion'].strip('/\\')
libraries = [vc / name for name in ('msvcrt.lib', 'oldnames.lib', 'vcruntime.lib')]
libraries += [kit / 'ucrt/x64/ucrt.lib', kit / 'um/x64/kernel32.Lib']
assert all(path.is_file() for path in libraries)
def run(mode):
    with tempfile.TemporaryDirectory(prefix='xray-native-invocation-') as temporary:
        directory = Path(temporary)
        source = directory / 'entry.xr'; source.write_text('print("native-invocation-ok")\n', encoding='utf-8')
        authority = directory
        if mode == 'full-program':
            authority = root / 'tests/fixtures/xir_compile_owner'
            source = authority / 'root.xr'
            for name in ('root.xr', 'counter.xr', 'label.xr'):
                print(f'actual fixture {name} sha256={hashlib.sha256((authority / name).read_bytes()).hexdigest()}', flush=True)
        inputs = directory / 'input'; inputs.mkdir()
        output = directory / 'input-output'; output.mkdir()
        command = [exe, authority, source, root / 'stdlib', sdk, compiler, linker, inputs,
                   root / 'tests/unit/xir/xir_sdk_consumer.c', *libraries, output, mode]
        started = time.perf_counter()
        result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
        sys.stdout.buffer.write(result.stdout); sys.stderr.buffer.write(result.stderr)
        print(f'case={mode} wall={time.perf_counter()-started:.3f}s', flush=True)
        assert result.returncode == 0, (mode, result.returncode)
        if mode in ('positive', 'full-program'):
            native = subprocess.run([str(output / 'program.exe')], capture_output=True, timeout=30)
            expected = b'owner\xe4\xb8\xad-ok 42 true\r\n' if mode == 'full-program' else b'native-invocation-ok\r\n'
            assert native.returncode == 0 and native.stdout == expected and native.stderr == b'', (native.returncode, native.stdout, native.stderr)
        return result.stdout.decode('utf-8', 'replace')

positive = run('positive')
if not faults and not guard:
    run('full-program')
if guard:
    for mode in ['null-guard', 'foreign-guard', 'armed-guard', 'failed-guard', 'closing-guard', 'guard-arm-oom',
                 'guard-check-budget', 'guard-check-io',
                 'guard-break', 'guard-final-break', 'guard-pending', 'read-oom', 'cancel']:
        run(mode)
if faults:
    allocations, comparisons, runs = map(int, re.search(
        r'owner direct allocations=(\d+) comparisons=(\d+) attempted-runs=(\d+)', positive).groups())
    assert runs == 6 and allocations and comparisons
    for mode in ['foreign-sdk', 'foreign-0', 'foreign-1', 'foreign-2', 'occupied-out', 'bad-recipe', 'nonempty',
                 'same-directory', 'case-directory', 'ancestor-directory', 'reverse-ancestor', 'source-shape', 'launcher-shape',
                 'launcher-race', 'source-race', 'cancel', 'timeout', 'child', 'replay',
                 'includes-replay', 'link-replay', 'image-replay']:
        run(mode)
    for index in range(allocations):
        run(f'oom-{index}')
    for index in range(6):
        run(f'process-oom-{index}')
    for axis, cuts in [('work', [50000000, 250000000, 500000000, 800000000]),
                       ('bytes', [10000000, 30000000, 60000000]), ('live', [5000000, 15000000, 28000000])]:
        for cut in cuts:
            run(f'budget-{axis}-{cut}')
    run(f'io-all-{comparisons}')
print('actual MSVC Source/SDK six calls; positive leases and exact native output; no full Target authority', flush=True)
