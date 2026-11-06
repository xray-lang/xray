"""Real native transactions; injected gates use the same owner implementation."""
from pathlib import Path
import hashlib, os, re, subprocess, sys, tempfile, time

exe, root, compiler, linker, sdk = map(lambda value: Path(value).resolve(), sys.argv[1:6])
faults = sys.argv[6:] == ['faults']
profile = sys.argv[6:] == ['profile']
preflight = sys.argv[6:] == ['preflight']
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
        launcher = root / 'tests/unit/xir/xir_sdk_consumer.c'
        selected_compiler, selected_linker = compiler, linker
        command = [exe, authority, source, root / 'stdlib', sdk, selected_compiler, selected_linker, inputs,
                   launcher, *libraries, output, mode,
                   Path(os.environ['VCToolsInstallDir']) / 'include',
                   Path(os.environ['WindowsSdkDir']) / 'Include' / os.environ['WindowsSDKVersion'].strip('/\\') / 'ucrt',
                   Path(os.environ['WindowsSdkDir']) / 'Include' / os.environ['WindowsSDKVersion'].strip('/\\') / 'shared',
                   Path(os.environ['WindowsSdkDir']) / 'Include' / os.environ['WindowsSDKVersion'].strip('/\\') / 'um',
                   os.environ['SystemRoot'], kit / 'um/x64/bcrypt.lib', kit / 'um/x64/advapi32.lib', kit / 'um/x64/userenv.lib']
        started = time.perf_counter()
        result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
        sys.stdout.buffer.write(result.stdout); sys.stderr.buffer.write(result.stderr)
        print(f'case={mode} wall={time.perf_counter()-started:.3f}s', flush=True)
        assert result.returncode == 0, (mode, result.returncode)
        if mode in ('positive', 'full-program', 'cancel', 'source-input-mismatch', 'launcher-input-mismatch') or mode.startswith(('oom-', 'process-oom-')):
            native = subprocess.run([str(output / 'program.exe')], capture_output=True, timeout=30)
            expected = b'owner\xe4\xb8\xad-ok 42 true\r\n' if mode == 'full-program' else b'native-invocation-ok\r\n'
            assert native.returncode == 0 and native.stdout == expected and native.stderr == b'', (native.returncode, native.stdout, native.stderr)
        return result.stdout.decode('utf-8', 'replace')


positive=run('positive')
if not faults and not profile and not preflight:
    run('full-program')
if profile or preflight:
    for mode in ['foreign-sdk','foreign-0','foreign-1','foreign-2','occupied-out','source-shape','launcher-shape','bad-recipe','same-directory','case-directory','ancestor-directory','reverse-ancestor','nonempty','child','timeout']:
        run(mode)
if faults:
    allocations=int(re.search(r'current explicit file table \d+; producer retirement; three compiler commands; allocations=(\d+)',positive).group(1))
    assert allocations>0
    # Representative actual allocator sites retain strict OOM and same-ledger retry.
    for index in sorted({0,allocations//2,allocations-1}):
        run(f'oom-{index}')
    for stage in range(3):
        run(f'process-oom-{stage}')
    for mode in ['cancel','source-input-mismatch','launcher-input-mismatch']:
        run(mode)
print('current three-command native owner: explicit inputs/file table, representative OOM/retry, independent output lifetime; no replay/loader proof',flush=True)
